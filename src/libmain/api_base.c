/** @file api_base.c
 *
 *  C API base functions.
 */

// Copyright (C) 2015-2023 Sanford Rockowitz <rockowitz@minsoft.com>
// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#define _GNU_SOURCE 1
#include <assert.h>
#include <dlfcn.h>     // _GNU_SOURCE for dladdr()
#include <errno.h>
#include <glib-2.0/glib.h>
#include <signal.h>
#include <string.h>
#include <syslog.h>

#include "public/ddcutil_c_api.h"

#include "util/ddcutil_config_file.h"
#include "util/debug_util.h"
#include "util/file_util.h"
#include "util/report_util.h"
#include "util/sysfs_filter_functions.h"
#include "util/xdg_util.h"

#include "base/base_services.h"
#include "base/build_info.h"
#include "base/core_per_thread_settings.h"
#include "base/core.h"
#include "base/dsa2.h"
#include "base/parms.h"
#include "base/per_display_data.h"
#include "base/per_thread_data.h"
#include "base/rtti.h"
#include "base/trace_control.h"
#include "base/tuned_sleep.h"

#include "cmdline/cmd_parser.h"
#include "cmdline/parsed_cmd.h"

#include "i2c/i2c_bus_core.h"   // for testing watch_devices
#include "i2c/i2c_execute.h"    // for i2c_set_addr()

#include "ddc/ddc_common_init.h"
#include "ddc/ddc_display_lock.h"
#include "ddc/ddc_displays.h"
#include "ddc/ddc_multi_part_io.h"
#include "ddc/ddc_packet_io.h"
#include "ddc/ddc_serialize.h"
#include "ddc/ddc_services.h"
#include "ddc/ddc_try_stats.h"
#include "ddc/ddc_vcp.h"
#include "ddc/ddc_watch_displays.h"

#include "libmain/api_error_info_internal.h"
#include "libmain/api_base_internal.h"
#include "libmain/api_services_internal.h"

//
// Forward Declarations
//

void init_api_base();

//
// Globals
//

static bool client_opened_syslog;

//
// Precondition Failure
//

DDCI_Api_Precondition_Failure_Mode api_failure_mode = DDCI_PRECOND_STDERR_RETURN;

#ifdef UNUSED
static DDCI_Api_Precondition_Failure_Mode
ddci_set_precondition_failure_mode(
      DDCI_Api_Precondition_Failure_Mode failure_mode)
{
   DDCI_Api_Precondition_Failure_Mode old = api_failure_mode;
   api_failure_mode = failure_mode;
   return old;
}

static DDCI_Api_Precondition_Failure_Mode
ddci_get_precondition_failure_mode()
{
   return api_failure_mode;
}
#endif


//
// Library Build Information
//

DDCA_Ddcutil_Version_Spec
ddca_ddcutil_version(void) {
   static DDCA_Ddcutil_Version_Spec vspec = {255,255,255};
   static bool vspec_init = false;

   if (!vspec_init) {
#ifndef NDEBUG
      int ct =
#endif
            sscanf(get_base_ddcutil_version(),
                      "%hhu.%hhu.%hhu", &vspec.major, &vspec.minor, &vspec.micro);
      assert(ct == 3);
      vspec_init = true;
   }
   DBGMSG("Returning: %d.%d.%d", vspec.major, vspec.minor, vspec.micro);
   return vspec;
}


/**  Returns the ddcutil version as a string in the form "major.minor.micro".
 *
 */
const char *
ddca_ddcutil_version_string(void) {
   return get_base_ddcutil_version();
}


// Returns the full ddcutil version as a string that may be suffixed with an extension
const char *
ddca_ddcutil_extended_version_string(void) {
   return get_full_ddcutil_version();
}


#ifdef UNUSED
// Indicates whether the ddcutil library was built with support for USB connected monitors.
bool
ddca_built_with_usb(void) {
#ifdef USE_USB
   return true;
#else
   return false;
#endif
}
#endif

// Alternative to individual ddca_built_with...() functions.
// conciseness vs documentability
// how to document bits?   should doxygen doc be in header instead?

DDCA_Build_Option_Flags
ddca_build_options(void) {
   uint8_t result = 0x00;
#ifdef USE_USB
         result |= DDCA_BUILT_WITH_USB;
#endif
#ifdef FAILSIM_ENABLED
         result |= DDCA_BUILT_WITH_FAILSIM;
#endif
   // DBGMSG("Returning 0x%02x", result);
   return result;
}


const char *
ddca_libddcutil_filename(void) {
   Dl_info info = {NULL,NULL,NULL,NULL};
   static char fullname[PATH_MAX];
   static char * p = NULL;
   if (!p) {
      dladdr(ddca_build_options, &info);
      p = realpath(info.dli_fname, fullname);
      assert(p == fullname);
   }
   return p;
}


//
// Initialization
//
static Error_Info *
get_parsed_libmain_config(const char * libopts_string,
                          bool         disable_config_file,
                          Parsed_Cmd** parsed_cmd_loc)
{
   bool debug = false;
   DBGF(debug, "Starting. disable_config_file = %s, libopts_string = %sn",
               sbool(disable_config_file), libopts_string);

   if (libopts_string) {
      fprintf(fout(), "Using libddcutil options passed from client: %s\n", libopts_string);
      SYSLOG2(DDCA_SYSLOG_NOTICE,"Using libddcutil options passed from client: %s",   libopts_string);
   }

   Error_Info * result = NULL;
   *parsed_cmd_loc = NULL;

   char ** libopts_tokens = NULL;
   int libopts_token_ct = 0;
   if (libopts_string) {
      libopts_token_ct = tokenize_options_line(libopts_string, &libopts_tokens);
      DBGF(debug, "libopts_token_ct = %d, libopts_tokens:", libopts_token_ct);
      if (debug)
         rpt_ntsa(libopts_tokens, 3);
   }
   Null_Terminated_String_Array cmd_name_array = calloc(2 + libopts_token_ct, sizeof(char*));
   cmd_name_array[0] = "libddcutil";   // so libddcutil not a special case for parser
   int ndx = 0;
   for (; ndx < libopts_token_ct; ndx++)
      cmd_name_array[ndx+1] = libopts_tokens[ndx];
   cmd_name_array[ndx+1] = NULL;

   DBGF(debug, "cmd_name_array=%p, cmd_name_array[1]=%p -> %s",
                cmd_name_array, cmd_name_array[0], cmd_name_array[0]);

   char ** new_argv = NULL;
   int     new_argc = 0;
   char *  untokenized_option_string = NULL;
   GPtrArray * errmsgs = g_ptr_array_new_with_free_func(g_free);
   if (disable_config_file) {
      DBGF(debug, "config file disabled");
      new_argv = cmd_name_array;
      new_argc = ntsa_length(cmd_name_array);
   }
   else {
      char *  config_fn = NULL;
      DBGF(debug, "Calling apply_config_file()...");
      int apply_config_rc = apply_config_file(
                                    "libddcutil",  // use this section of config file
                                    1, cmd_name_array,
                                    &new_argc,
                                    &new_argv,
                                    &untokenized_option_string,
                                    &config_fn,
                                    errmsgs);
      assert(apply_config_rc <= 0);
      ASSERT_IFF(apply_config_rc == 0, errmsgs->len == 0);
      // DBGF(debug, "Calling ntsa_free(cmd_name_array=%p", cmd_name_array);
      ntsa_free(cmd_name_array, false);
      DBGF(debug, "apply_config_file() returned: %d (%s), new_argc=%d, new_argv=%p:",
                  apply_config_rc, psc_desc(apply_config_rc), new_argc, new_argv);

      if (apply_config_rc == -EBADMSG) {
         result = errinfo_new(DDCRC_INVALID_CONFIG_FILE, __func__,
                              "Error(s) processing configuration file: %s", config_fn);
         for (int ndx = 0; ndx < errmsgs->len; ndx++) {
            errinfo_add_cause(result,
                  errinfo_new(DDCRC_INVALID_CONFIG_FILE, __func__, g_ptr_array_index(errmsgs, ndx)));
         }
      }
      else if (apply_config_rc == -ENOENT) {
         result = errinfo_new(-ENOENT, __func__, "Configuration file not found");
      }
      else if (apply_config_rc < 0) {
         result = errinfo_new(apply_config_rc, __func__,
                     "Unexpected error reading configuration file: %s", psc_desc(apply_config_rc));
      }
      else {
         assert( new_argc == ntsa_length(new_argv) );
         if (debug)
            ntsa_show(new_argv);

   #ifdef OUT
      // TODO: set msgs in Error_Info records
      if (errmsgs->len > 0) {
         f0printf(ferr(),    "Error(s) reading libddcutil configuration from file %s:\n", config_fn);
         SYSLOG(LOG_WARNING, "Error(s) reading libddcutil configuration from file %s:",   config_fn);
         for (int ndx = 0; ndx < errinfo_accumulator->len; ndx++) {
            f0printf(fout(),     "   %s\n", g_ptr_array_index(errmsgs, ndx));
            SYSLOG(LOG_WARNING,  "   %s",   (char*) g_ptr_array_index(errmsgs, ndx));
         }
      }
      // alt:
      if (errinfo_accumulator->len > 0) {
         f0printf(ferr(),    "Error(s) reading libddcutil configuration from file %s:\n", config_fn);
         SYSLOG(LOG_WARNING, "Error(s) reading libddcutil configuration from file %s:",   config_fn);
         for (int ndx = 0; ndx < errinfo_accumulator->len; ndx++) {
            f0printf(fout(),     "   %s\n", errinfo_summary( g_ptr_array_index(errinfo_accumulator, ndx)));
            SYSLOG(LOG_WARNING,  "   %s",   errinfo_summary( g_ptr_array_index(errinfo_accumulator, ndx)));
         }
      }
   #endif
         if (untokenized_option_string && strlen(untokenized_option_string) > 0) {
            fprintf(fout(), "Using libddcutil options from %s: %s\n", config_fn, untokenized_option_string);
            SYSLOG2(DDCA_SYSLOG_NOTICE,"Using libddcutil options from %s: %s",   config_fn, untokenized_option_string);
         }
      }
      free(config_fn);
   }

   if (!result) {
      assert(new_argc >= 1);
      if (libopts_token_ct > 0) {
        int newest_argc = new_argc + libopts_token_ct;
        Null_Terminated_String_Array newest_argv = reallocarray(new_argv, newest_argc+1, sizeof(char*));
        for (int ndx = 0; ndx < libopts_token_ct; ndx++) {
           newest_argv[new_argc+ndx] = libopts_tokens[ndx];   // strdup?
           newest_argv[new_argc+libopts_token_ct] = NULL;
        }
        new_argv = newest_argv;
        new_argc = new_argc + libopts_token_ct;
        if (debug) {
           DBGF(true, "After libopts tokens applied:");
           ntsa_show(new_argv);
        }
      }

      char * combined = strjoin((const char**)(new_argv+1), new_argc, " ");
      fprintf(fout(), "Applying combined libddcutil options: %s\n", combined);
      SYSLOG2(DDCA_SYSLOG_NOTICE,"Applying combined libddcutil options: %s",   combined);
      DBGF(debug, "Calling parse_command(), errmsgs=%p\n", errmsgs);
      *parsed_cmd_loc = parse_command(new_argc, new_argv, MODE_LIBDDCUTIL, errmsgs);
      if (debug)
      DBGF(debug, "*parsed_cmd_loc=%p, errmsgs->len=%d", *parsed_cmd_loc, errmsgs->len);
      ASSERT_IFF(*parsed_cmd_loc, errmsgs->len == 0);
      if (!*parsed_cmd_loc) {
         if (test_emit_syslog(DDCA_SYSLOG_ERROR)) {
            syslog(LOG_ERR, "Invalid option string: %s",  combined);
            for (int ndx = 0; ndx < errmsgs->len; ndx++) {
                char * msg =  g_ptr_array_index(errmsgs,ndx);
                syslog(LOG_ERR, "%s", msg);
            }
         }
         result = errinfo_new(DDCRC_INVALID_CONFIG_FILE, __func__,
               "Invalid option string: %s",  combined);
         for (int ndx = 0; ndx < errmsgs->len; ndx++) {
            char * msg =  g_ptr_array_index(errmsgs, ndx);
            errinfo_add_cause(result, errinfo_new(DDCRC_INVALID_CONFIG_FILE, __func__, msg));
         }
      }
      else {
         assert(*parsed_cmd_loc);
         if (debug)
            dbgrpt_parsed_cmd(*parsed_cmd_loc, 1);
         ntsa_free(new_argv, false);
      }
      // DBGF(debug, "Calling ntsa_free(cmd_name_array=%p", cmd_name_array);
      // ntsa_free(cmd_name_array, false);
      // ntsa_free(new_argv, true);
      free(combined);
      free(untokenized_option_string);
   }
   if (libopts_tokens)
      ntsa_free(libopts_tokens, false);

   DBGF(debug, "Done.     *parsed_cmd_loc=%p. Returning %s",
              *parsed_cmd_loc, errinfo_summary(result));

   ASSERT_IFF(*parsed_cmd_loc, !result);
   return result;
}


#ifdef TESTING_CLEANUP
void done() {
   printf("(%s) Starting\n", __func__);
   _ddca_terminate();
   SYSLOG(LOG_INFO, "(%s) executing done()", __func__);
   printf("(%s) Done.\n", __func__);
}

void dummy_sigterm_handler() {
   printf("(%s) Executing. library_initialized = %s\n",
         __func__, SBOOL(library_initialized));
}

void atexit_func() {
   printf("(%s) Executing. library_initalized = %s\n",
         __func__, SBOOL(library_initialized));
}
#endif

static FILE * flog = NULL;

bool library_initialized = false;
DDCA_Stats_Type requested_stats = 0;
bool per_display_stats = false;
bool dsa_detail_stats;


/** Initializes the ddcutil library module.
 *
 *  Called automatically when the shared library is loaded.
 *
 *  Registers functions in rtti table, performs additional initialization
 *  that cannot fail.
 */
void  __attribute__ ((constructor))
_ddca_new_init(void) {
   bool debug = false;
   char * s = getenv("DDCUTIL_DEBUG_LIBINIT");
   if (s && strlen(s) > 0)
      debug = true;

   DBGF(debug, "Starting. library_initialized=%s\n", sbool(library_initialized));

   init_api_base();
   init_base_services();    // initializes tracing related modules
   init_ddc_services();     // initializes i2c, usb, ddc, vcp, dynvcp
   init_api_services();     // other files in directory libmain

#ifdef TESTING_CLEANUP
   // int atexit_rc = atexit(done);   // TESTING CLEANUP
   // printf("(%s) atexit() returned %d\n", __func__, atexit_rc);
#endif

   DBGF(debug, "Done.");
}


//
// Profiling
//

void profiling_enable(bool enabled) {
   ptd_api_profiling_enabled = enabled;
}

void profiling_reset() {
   ptd_profile_reset_all_stats();
}

void profile_start_call(void * func) {
   ptd_profile_function_start(func);
}

void profile_end_call(void * func) {
   ptd_profile_function_end(func);
}

void profile_report(FILE * dest, bool by_thread) {
   if (dest) {
      rpt_push_output_dest(dest);
   }
   ptd_profile_report_all_threads(0);
   ptd_profile_report_stats_summary(0);
   if (dest) {
      rpt_pop_output_dest();
   }
}


//
// Tracing
//

void
init_library_trace_file(char * library_trace_file, bool enable_syslog, bool debug) {
   DBGF(debug, "library_trace_file = \"%s\", enable_syslog = %s", library_trace_file, sbool(enable_syslog));
   char * trace_file = (library_trace_file[0] != '/')
          ? xdg_state_home_file("ddcutil", library_trace_file)
          : g_strdup(library_trace_file);
   DBGF(debug, "Setting trace destination %s", trace_file);
   SYSLOG2(DDCA_SYSLOG_NOTICE, "Trace destination: %s", trace_file);

   fopen_mkdir(trace_file, "a", stderr, &flog);
   if (flog) {
      time_t trace_start_time = time(NULL);
      char * trace_start_time_s = asctime(localtime(&trace_start_time));
      if (trace_start_time_s[strlen(trace_start_time_s)-1] == 0x0a)
           trace_start_time_s[strlen(trace_start_time_s)-1] = 0;
      fprintf(flog, "%s tracing started %s\n", "libddcutil", trace_start_time_s);
      DBGF(debug, "Writing %s trace output to %s", "libddcutil",trace_file);
      set_default_thread_output_settings(flog, flog);
      set_fout(flog);
      set_ferr(flog);

      rpt_set_default_output_dest(flog);    // for future threads
      rpt_push_output_dest(flog);           // for this thread
   }
   else {
      fprintf(stderr, "Error opening libddcutil trace file %s: %s\n",
                      trace_file, strerror(errno));
      SYSLOG2(DDCA_SYSLOG_ERROR, "Error opening libddcutil trace file %s: %s",
                             trace_file, strerror(errno));
   }
   free(trace_file);
   DBGF(debug, "Done.");
}


/** Cleanup at library termination
 *
 *  - Terminates thread that watches for display addition or removal.
 *  - Releases heap memory to avoid error reports from memory analyzers.
 */
void __attribute__ ((destructor))
_ddca_terminate(void) {
   bool debug = false;
   DBGTRC_STARTING(debug, DDCA_TRC_API, "library_initialized = %s", SBOOL(library_initialized));
   if (library_initialized) {
      if (debug)
         dbgrpt_distinct_display_descriptors(2);
      if (dsa2_enabled)
         dsa2_save_persistent_stats();
      if (display_caching_enabled)
         ddc_store_displays_cache();
      ddc_discard_detected_displays();
      if (requested_stats)
         ddc_report_stats_main(requested_stats, per_display_stats, dsa_detail_stats, 0);
      terminate_ddc_services();
      terminate_base_services();
      ddc_stop_watch_displays();
      free_regex_hash_table();
      library_initialized = false;
      if (flog)
         fclose(flog);
      DBGTRC_DONE(debug, DDCA_TRC_API, "library termination complete");
   }
   else {
      DBGTRC_DONE(debug, DDCA_TRC_API, "library was already terminated");   // should be impossible
   }
   // special handling for termination msg
   if (syslog_level > DDCA_SYSLOG_NEVER)
      syslog(LOG_NOTICE, "libddcutil terminating.");
   if (syslog_level > DDCA_SYSLOG_NEVER && !client_opened_syslog)
      closelog();
}


Error_Info *
set_master_errinfo_from_init_errors(
      GPtrArray * errs) // array of Error_Info *
{
   bool debug = false;
   DBGF(debug, "Starting. errs=%p", errs);
   Error_Info * master_error = NULL;
   if (errs && errs->len > 0) {
      master_error = errinfo_new(DDCRC_BAD_DATA, __func__, "Invalid configuration options");
      for (int ndx = 0; ndx < errs->len; ndx++) {
         Error_Info * cur = g_ptr_array_index(errs, ndx);
         errinfo_add_cause(master_error, cur);
      }
      g_ptr_array_free(errs, false);
   }
   DBGF(debug, "Done.  Returning %p");
   return master_error;
}


DDCA_Status
set_ddca_error_detail_from_init_errors(
      GPtrArray * errs) // array of Error_Info *
{
   bool debug = false;
   DDCA_Status ddcrc = 0;
   if (errs && errs->len > 0) {
      Error_Info * master_error = errinfo_new(DDCRC_BAD_DATA, __func__, "Invalid configuration options");
      ddcrc = DDCRC_BAD_DATA;
      for (int ndx = 0; ndx < errs->len; ndx++) {
         Error_Info * cur = g_ptr_array_index(errs, ndx);
         errinfo_add_cause(master_error, cur);
      }
      DDCA_Error_Detail * public_error_detail = error_info_to_ddca_detail(master_error);
      errinfo_free_with_report(master_error, debug, __func__);
      save_thread_error_detail(public_error_detail);
   }
   // clear if no errors?
   return ddcrc;
}


DDCA_Syslog_Level ddca_syslog_level_from_name(const char * name) {
   return syslog_level_name_to_value(name);
}


DDCA_Status
ddca_init(const char *      libopts,
          DDCA_Syslog_Level syslog_level_arg,
          DDCA_Init_Options opts)
{
   bool debug = false;
   char * s = getenv("DDCUTIL_DEBUG_LIBINIT");
   if (s && strlen(s) > 0)
      debug = true;

   DBGF(debug, "Starting. library_initialized=%s", sbool(library_initialized));

   client_opened_syslog = opts & DDCA_INIT_OPTIONS_CLIENT_OPENED_SYSLOG;
   if (syslog_level_arg == DDCA_SYSLOG_NOT_SET)
      syslog_level_arg = DEFAULT_LIBDDCUTIL_SYSLOG_LEVEL;              // libddcutil default

   if (syslog_level_arg > DDCA_SYSLOG_NEVER) {
      enable_syslog = true;
      if (!client_opened_syslog) {
      openlog("libddcutil",       // prepended to every log message
              LOG_CONS | LOG_PID, // write to system console if error sending to system logger
                                  // include caller's process id
              LOG_USER);          // generic user program, syslogger can use to determine how to handle
      }
      // special handling for start and termination msgs
      // always output if syslog is opened
      syslog(LOG_NOTICE, "Initializing libddcutil.  ddcutil version: %s, shared library: %s",
                get_full_ddcutil_version(), ddca_libddcutil_filename());
   }
   syslog_level = syslog_level_arg;  // global in trace_control.h

   Error_Info * master_error = NULL;
   if (library_initialized) {
      master_error = errinfo_new(DDCRC_INVALID_OPERATION, __func__, "libddcutil already initialized");
   }
   else {
      Parsed_Cmd * parsed_cmd = NULL;
      if ((opts & DDCA_INIT_OPTIONS_DISABLE_CONFIG_FILE) && !libopts) {
         parsed_cmd = new_parsed_cmd();
      }
      else {
         master_error = get_parsed_libmain_config(
                           libopts,
                           opts & DDCA_INIT_OPTIONS_DISABLE_CONFIG_FILE,
                           &parsed_cmd);
         ASSERT_IFF(master_error, !parsed_cmd);
         if (!master_error) {
            if (parsed_cmd->trace_destination) {
               DBGF(debug, "Setting library trace file: %s", parsed_cmd->trace_destination);
               init_library_trace_file(parsed_cmd->trace_destination, enable_syslog, debug);
            }
            master_error = init_tracing(parsed_cmd);
            requested_stats = parsed_cmd->stats_types;
            ptd_api_profiling_enabled = parsed_cmd->flags & CMD_FLAG_PROFILE_API;
            per_display_stats = parsed_cmd->flags & CMD_FLAG_VERBOSE_STATS;
            dsa_detail_stats = parsed_cmd->flags & CMD_FLAG_F6;
            submaster_initializer(parsed_cmd);
         }

         ddc_start_watch_displays(/*use_udev_if_possible=*/ false);
      }
      if (parsed_cmd)
         free_parsed_cmd(parsed_cmd);
   }

   DDCA_Status ddcrc = 0;
   if (master_error) {
      ddcrc = master_error->status_code;
      DDCA_Error_Detail * public_error_detail = error_info_to_ddca_detail(master_error);
      save_thread_error_detail(public_error_detail);
      if (test_emit_syslog(DDCA_SYSLOG_ERROR)) {
         SYSLOG2(DDCA_SYSLOG_ERROR, "Library initialization failed: %s", psc_desc(master_error->status_code));
         for (int ndx = 0; ndx < master_error->cause_ct; ndx++) {
            SYSLOG2(DDCA_SYSLOG_ERROR, "%s", master_error->causes[ndx]->detail);
         }
      }
      errinfo_free(master_error);
   }
   else {
      library_initialized = true;
      // SYSLOG2(DDCA_SYSLOG_NOTICE, "Library initialization complete.");
   }

   DBGF(debug, "Done.    Returning: %s", psc_desc(ddcrc));

   return ddcrc;
}


//
// Error Detail
//

DDCA_Error_Detail *
ddca_get_error_detail() {
   bool debug = false;
   DBGMSF(debug, "Starting");

   DDCA_Error_Detail * result = dup_error_detail(get_thread_error_detail());

   if (debug) {
      DBGMSG("Done.     Returning: %p", result);
      if (result)
         report_error_detail(result, 2);
   }
   return result;
}


void
ddca_free_error_detail(DDCA_Error_Detail * ddca_erec) {
   free_error_detail(ddca_erec);
}


void
ddca_report_error_detail(DDCA_Error_Detail * ddca_erec, int depth) {
   report_error_detail(ddca_erec, depth);
}


// DDCA_Error_Detail * ddca_dup_error_detail(DDCA_Error_Detail * original) {
//     return dup_error_detail(original);
// }


//
// Status Code Management
//

const char *
ddca_rc_name(DDCA_Status status_code) {
   char * result = NULL;
   Status_Code_Info * code_info = find_status_code_info(status_code);
   if (code_info)
      result = code_info->name;
   return result;
}


const char *
ddca_rc_desc(DDCA_Status status_code) {
   char * result = "unknown status code";
   Status_Code_Info * code_info = find_status_code_info(status_code);
   if (code_info)
      result = code_info->description;
   return result;
}


#ifdef REMOVED
/** Enable display of internal exception reports (Error_Info).
 *
 *  @param[in] enable  true/false
 *  @return prior value
 */
bool
ddca_enable_error_info(
      bool enable);
#endif

#ifdef REMOVED
// quick and dirty for now
// TODO: make thread safe, wrap in mutex
bool
ddca_enable_error_info(bool enable) {
   bool old_value = report_freed_exceptions;
   report_freed_exceptions = enable;            // global in core.c
   return old_value;
}
#endif

//
// Output redirection
//

// Redirects output that normally would go to STDOUT
void
ddca_set_fout(FILE * fout) {
   // DBGMSG("Starting. fout=%p", fout);
   set_fout(fout);
}


void
ddca_set_fout_to_default(void) {
   set_fout_to_default();
}


// Redirects output that normally would go to STDERR
void
ddca_set_ferr(FILE * ferr) {
   set_ferr(ferr);
}


void
ddca_set_ferr_to_default(void) {
   set_ferr_to_default();
}


//
// Output capture - convenience functions
//

typedef struct {
   FILE * in_memory_file;
   char * in_memory_bufstart; ;
   size_t in_memory_bufsize;
   DDCA_Capture_Option_Flags flags;
} In_Memory_File_Desc;


static In_Memory_File_Desc *
get_thread_capture_buf_desc() {
   static GPrivate  in_memory_key = G_PRIVATE_INIT(g_free);

   In_Memory_File_Desc* fdesc = g_private_get(&in_memory_key);

   // GThread * this_thread = g_thread_self();
   // printf("(%s) this_thread=%p, fdesc=%p\n", __func__, this_thread, fdesc);

   if (!fdesc) {
      fdesc = g_new0(In_Memory_File_Desc, 1);
      g_private_set(&in_memory_key, fdesc);
   }

   // printf("(%s) Returning: %p\n", __func__, fdesc);
   return fdesc;
}


void
ddca_start_capture(DDCA_Capture_Option_Flags flags) {
   In_Memory_File_Desc * fdesc = get_thread_capture_buf_desc();

   if (!fdesc->in_memory_file) {
      fdesc->in_memory_file = open_memstream(&fdesc->in_memory_bufstart, &fdesc->in_memory_bufsize);
      ddca_set_fout(fdesc->in_memory_file);   // n. ddca_set_fout() is thread specific
      fdesc->flags = flags;
      if (flags & DDCA_CAPTURE_STDERR)
         ddca_set_ferr(fdesc->in_memory_file);
   }
   // printf("(%s) Done.\n", __func__);
}


char *
ddca_end_capture(void) {
   In_Memory_File_Desc * fdesc = get_thread_capture_buf_desc();
   // In_Memory_File_Desc * fdesc = &in_memory_file_desc;

   char * result = "\0";
   // printf("(%s) Starting.\n", __func__);
   assert(fdesc->in_memory_file);
   if (fflush(fdesc->in_memory_file) < 0) {
      ddca_set_ferr_to_default();
      SEVEREMSG("flush() failed. errno=%d", errno);
      return g_strdup(result);
   }
   // n. open_memstream() maintains a null byte at end of buffer, not included in in_memory_bufsize
   result = g_strdup(fdesc->in_memory_bufstart);
   if (fclose(fdesc->in_memory_file) < 0) {
      ddca_set_ferr_to_default();
      SEVEREMSG("fclose() failed. errno=%d", errno);
      return result;
   }
   // free(fdesc->in_memory_file); // double free, fclose() frees in memory file
   fdesc->in_memory_file = NULL;
   ddca_set_fout_to_default();
   if (fdesc->flags & DDCA_CAPTURE_STDERR)
      ddca_set_ferr_to_default();

   // printf("(%s) Done. result=%p\n", __func__, result);
   return result;
}


#ifdef UNUSED
/** Returns the current size of the in-memory capture buffer.
 *
 *  @return number of characters in current buffer, plus 1 for
 *          terminating null
 *  @retval -1 no capture buffer on current thread
 *
 *  @remark defined and tested but does not appear useful
 */
int ddca_captured_size() {
   // printf("(%s) Starting.\n", __func__);
   In_Memory_File_Desc * fdesc = get_thread_capture_buf_desc();

   int result = -1;
   // n. open_memstream() maintains a null byte at end of buffer, not included in in_memory_bufsize
   if (fdesc->in_memory_file) {
      fflush(fdesc->in_memory_file);
      result = fdesc->in_memory_bufsize + 1;   // +1 for trailing \0
   }
   // printf("(%s) Done. result=%d\n", __func__, result);
   return result;
}
#endif


//
// Message Control
//

DDCA_Output_Level
ddca_get_output_level(void) {
   return get_output_level();
}


DDCA_Output_Level
ddca_set_output_level(DDCA_Output_Level newval) {
     return set_output_level(newval);
}


char *
ddca_output_level_name(DDCA_Output_Level val) {
   return output_level_name(val);
}

#ifdef REMOVED


/** Controls whether messages describing DDC protocol errors are output
 *
 *  @param[in] onoff    if true, errors will be issued
 *  @return    prior value
 *
 *  This setting is global to all threads.
 */
bool
ddca_enable_report_ddc_errors(
      bool onoff);

/** Indicates whether messages describing DDC protocol errors are output.
 *
 *  This setting is global to all threads.
 */
bool
ddca_is_report_ddc_errors_enabled(void);
#endif

#ifdef REMOVED
bool
ddca_enable_report_ddc_errors(bool onoff) {
   return enable_report_ddc_errors(onoff);
}


bool
ddca_is_report_ddc_errors_enabled(void) {
   return is_report_ddc_errors_enabled();
}
#endif

//
// Global Settings
//

#ifdef REMOVED
int
ddca_max_max_tries(void) {
   return MAX_MAX_TRIES;
}


//  *** THIS IS FOR THE CURRENT THREAD
//  *** replace using function specifying display
//  *** for now, revert to old try_data_get_maxtries2()
int
ddca_get_max_tries(DDCA_Retry_Type retry_type) {
   // stats for multi part writes and reads are separate, but the
   // max tries for both are identical
// #ifndef NDEBUG
   Retry_Op_Value result3 = try_data_get_maxtries2((Retry_Operation) retry_type);
// #endif
   // // new way using retry_mgt
   // Retry_Op_Value result2 = trd_get_thread_max_tries((Retry_Operation) retry_type);
   // assert(result == result2);
   // assert(result2 == result3);
   return result3;
}


// ** THIS IS FOR CURRENT THREAD - FIX
DDCA_Status
ddca_set_max_tries(
      DDCA_Retry_Type retry_type,
      int             max_tries)
{
   DDCA_Status rc = 0;
   free_thread_error_detail();
   if (max_tries < 1 || max_tries > MAX_MAX_TRIES)
      rc = DDCRC_ARG;
   else {
      try_data_set_maxtries2((Retry_Operation) retry_type, max_tries);
      // for DDCA_MULTI_PART_TRIES, set both  MULTI_PART_WRITE_OP and MULTI_PART_READ_OP
      if (retry_type == DDCA_MULTI_PART_TRIES)
         try_data_set_maxtries2(MULTI_PART_WRITE_OP, max_tries);

      // new way, set in retry_mgt
#ifdef TRD
      trd_set_thread_max_tries((Retry_Operation) retry_type, max_tries);
      if (retry_type == DDCA_MULTI_PART_TRIES)
           trd_set_thread_max_tries(MULTI_PART_WRITE_OP, max_tries);
#endif
   }
   return rc;
}
#endif

bool
ddca_enable_verify(bool onoff) {
   return ddc_set_verify_setvcp(onoff);
}


bool
ddca_is_verify_enabled() {
   return ddc_get_verify_setvcp();
}

#ifdef NOT_NEEDED
void ddca_lock_default_sleep_multiplier() {
   lock_default_sleep_multiplier();
}

void ddca_unlock_sleep_multiplier() {
   unlock_default_sleep_multiplier();
}
#endif

#ifdef REMOVED
// deprecated, now a NOOP
bool
ddca_enable_sleep_suppression(bool newval) {
   return false;
}

// deprecated, now a NOOP
bool
ddca_is_sleep_suppression_enabled() {
   return false;
}


// *** FOR CURRENT THREAD
double
ddca_set_default_sleep_multiplier(double multiplier)
{
   bool debug = false;
   DBGTRC_STARTING(debug, DDCA_TRC_API, "Setting multiplier = %6.3f", multiplier);

   double old_value = -1.0;
   if (multiplier >= 0.0 && multiplier <= 10.0) {
// #ifdef TSD
      old_value = pdd_get_default_sleep_multiplier_factor();
      pdd_set_default_sleep_multiplier_factor(multiplier, Reset);
// #endif
    }

   DBGTRC_DONE(debug, DDCA_TRC_API, "Returning: %6.3f", old_value);
   return old_value;
}


double
ddca_get_default_sleep_multiplier()
{
   bool debug = false;
   DBGTRC_STARTING(debug, DDCA_TRC_API, "");
   double result = pdd_get_default_sleep_multiplier_factor();
   DBGTRC(debug, DDCA_TRC_API, "Returning %6.3f", result);
   return result;
}


void
ddca_set_global_sleep_multiplier(double multiplier)
{
   ddca_set_default_sleep_multiplier(multiplier);
   return;
}

double
ddca_get_global_sleep_multiplier()
{
   return ddca_get_default_sleep_multiplier();
}
#endif

// for display on current thread
double
ddca_set_sleep_multiplier(double multiplier)
{
   bool debug = false;
   DBGTRC_STARTING(debug, DDCA_TRC_API, "Setting multiplier = %6.3f", multiplier);

   double old_value = -1.0;
   if (multiplier >= 0.0 && multiplier <= 10.0) {
      Per_Thread_Data * ptd = ptd_get_per_thread_data();
      if (ptd->cur_dh) {
         Per_Display_Data * pdd = ptd->cur_dh->dref->pdd;
         old_value = pdd->user_sleep_multiplier;
         pdd_reset_multiplier(pdd, multiplier);
      }
   }

   DBGTRC_DONE(debug, DDCA_TRC_API, "Returning: %6.3f", old_value);
   return old_value;
}

double
ddca_get_sleep_multiplier()
{
   bool debug = false;
   DBGTRC(debug, DDCA_TRC_API, "");

   Per_Thread_Data * ptd = ptd_get_per_thread_data();
   double result =  -1.0f;
   if (ptd->cur_dh) {
      Per_Display_Data * pdd = ptd->cur_dh->dref->pdd;
      result = pdd->user_sleep_multiplier;
   }
#ifdef TSD
   double result = tsd_get_sleep_multiplier_factor();
#endif
   DBGTRC(debug, DDCA_TRC_API, "Returning %6.3f", result);
   return result;
}


#ifdef FUTURE

/** Gets the I2C timeout in milliseconds for the specified timeout class.
 * @param timeout_type timeout type
 * @return timeout in milliseconds
 */
int
ddca_get_timeout_millis(
      DDCA_Timeout_Type timeout_type) {
   return 0;    // *** UNIMPLEMENTED ***
}

/** Sets the I2C timeout in milliseconds for the specified timeout class
 * @param timeout_type  timeout class
 * @param millisec      timeout to set, in milliseconds
 */
void
ddca_set_timeout_millis(
      DDCA_Timeout_Type timeout_type,
      int               millisec)
{
   // *** UNIMPLEMENTED
}
#endif

#ifdef FUTURE
bool
ddca_enable_force_slave_address(bool onoff) {
   return false;
}


bool
ddca_is_force_slave_address_enabled(void) {
   return false;
}
#endif


#ifdef REMOVED

/** Controls the force I2C slave address setting.
 *
 *  Normally, ioctl operation I2C_SLAVE is used to set the I2C slave address.
 *  If that returns EBUSY and this setting is in effect, slave address setting
 *  is retried using operation I2C_SLAVE_FORCE.
 *
 *  @param[in] onoff true/false
 *  @return  prior value
 *  @since 1.2.2
 */
bool
ddca_enable_force_slave_address(bool onoff);

/** Query the force I2C slave address setting.
 *
 *  @return true/false
 *  @since 1.2.2
 */
bool
ddca_is_force_slave_address_enabled(void);
#endif

#ifdef REMOVED
bool
ddca_enable_force_slave_address(bool onoff) {
   bool old = i2c_forceable_slave_addr_flag;
   i2c_forceable_slave_addr_flag = onoff;
   return old;
}


bool
ddca_is_force_slave_address_enabled(void) {
   return i2c_forceable_slave_addr_flag;
}
#endif


#ifdef REMOVED
//
// Tracing
//

bool
ddca_add_traced_function(const char * funcname) {
   return add_traced_function(funcname);
}

#ifdef UNNEEDED
bool
ddca_add_traced_api_call(const char * funcname) {
   return add_traced_api_call(funcname);
}
#endif


void
ddca_add_traced_file(const char * filename) {
   add_traced_file(filename);
}


void
ddca_set_trace_groups(DDCA_Trace_Group trace_flags) {
   set_trace_groups(trace_flags);
}


void
ddca_add_trace_groups(DDCA_Trace_Group trace_flags) {
   add_trace_groups(trace_flags);
}


DDCA_Trace_Group
ddca_trace_group_name_to_value(char * name) {
   return trace_class_name_to_value(name);
}


void
ddca_set_trace_options(DDCA_Trace_Options  options) {
   // DBGMSG("options = 0x%02x", options);
   // global variables in core.c

   if (options & DDCA_TRCOPT_TIMESTAMP)
      dbgtrc_show_time = true;
   if (options & DDCA_TRCOPT_WALLTIME)
      dbgtrc_show_time = true;
   if (options & DDCA_TRCOPT_THREAD_ID)
      dbgtrc_show_thread_id = true;
}
#endif

//
// Statistics
//

// TODO: Add functions to access ddcutil's runtime error statistics


#ifdef UNUSED
void
ddca_register_thread_dref(DDCA_Display_Ref dref) {
   ptd_register_thread_dref( (Display_Ref *) dref);
}
#endif

#ifdef REMOVED
void
ddca_set_thread_description(
      const char * description)
{
   ptd_set_thread_description( description );
}

void
ddca_append_thread_description(
      const char * description)
{
   ptd_append_thread_description(description);
}

const char *
ddca_get_thread_descripton() {
   return ptd_get_thread_description_t();
}
#endif

void
ddca_reset_stats(void) {
   // DBGMSG("Executing");
   ddc_reset_stats_main();
}

// TODO: Functions that return stats in data structures
void
ddca_show_stats(
      DDCA_Stats_Type stats_types,
      bool            per_display_stats,
      int             depth)
{
   if (stats_types)
      ddc_report_stats_main( stats_types, per_display_stats, per_display_stats, depth);
}


void init_api_base() {
   // DBGMSG("Executing");
#ifdef REMOVED
   RTTI_ADD_FUNC(ddca_set_sleep_multiplier);
   RTTI_ADD_FUNC(ddca_set_default_sleep_multiplier);
#endif
}

