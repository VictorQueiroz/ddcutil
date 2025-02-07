/** @file ddc_displays.c
 *
 *  Access displays, whether DDC or USB
 *
 *  This file and ddc_display_ref_reports.c cross-reference each other.
 */

// Copyright (C) 2014-2023 Sanford Rockowitz <rockowitz@minsoft.com>
// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

/** \cond */
#include <assert.h>
#include <errno.h>
#include <glib-2.0/glib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "util/data_structures.h"
#include "util/debug_util.h"
#include "util/edid.h"
#include "util/error_info.h"
#include "util/failsim.h"
#include "util/report_util.h"
#include "util/string_util.h"
#include "util/sysfs_util.h"
#ifdef ENABLE_UDEV
#include "util/udev_usb_util.h"
#include "util/udev_util.h"
#endif
/** \endcond */

#include "public/ddcutil_types.h"

#include "base/core.h"
#include "base/ddc_packets.h"
#include "base/feature_metadata.h"
#include "base/linux_errno.h"
#include "base/monitor_model_key.h"
#include "base/parms.h"
#include "base/rtti.h"

#include "vcp/vcp_feature_codes.h"

#include "i2c/i2c_bus_core.h"
#include "i2c/i2c_strategy_dispatcher.h"
#include "i2c/i2c_sysfs.h"

#ifdef USE_USB
#include "usb/usb_displays.h"
#endif

#include "dynvcp/dyn_feature_files.h"

#include "ddc/ddc_packet_io.h"
#include "ddc/ddc_serialize.h"
#include "ddc/ddc_vcp.h"
#include "ddc/ddc_vcp_version.h"

#include "ddc/ddc_display_ref_reports.h"
#include "ddc/ddc_displays.h"

// Default trace class for this file
static DDCA_Trace_Group TRACE_GROUP = DDCA_TRC_DDC;

static GPtrArray * all_displays = NULL;         // all detected displays, array of Display_Ref *
static GPtrArray * display_open_errors = NULL;  // array of Bus_Open_Error
static int dispno_max = 0;                      // highest assigned display number
static int async_threshold = DISPLAY_CHECK_ASYNC_THRESHOLD_DEFAULT;
#ifdef USE_USB
static bool detect_usb_displays = true;
#else
static bool detect_usb_displays = false;
#endif

//
// Functions to perform initial checks
//

/** Sets the threshold for async display examination.
 *  If the number of /dev/i2c devices for which DDC communication is to be
 *  checked is greater than or equal to the threshold value, examine each
 *  device in a separate thread.
 *
 *  @param threshold  threshold value
 */
void
ddc_set_async_threshold(int threshold) {
   // DBGMSG("threshold = %d", threshold);
   async_threshold = threshold;
}


static inline bool
value_bytes_zero_for_any_value(DDCA_Any_Vcp_Value * pvalrec) {
   assert(pvalrec);
   bool result = pvalrec && pvalrec->value_type ==  DDCA_NON_TABLE_VCP_VALUE &&
                 pvalrec->val.c_nc.mh == 0 &&
                 pvalrec->val.c_nc.ml == 0 &&
                 pvalrec->val.c_nc.sh == 0 &&
                 pvalrec->val.c_nc.sl == 0;
   return result;
}


static inline bool
value_bytes_zero_for_nontable_value(Parsed_Nontable_Vcp_Response* valrec) {
   assert(valrec);
   bool result =  valrec->mh == 0 && valrec->ml == 0 && valrec->sh == 0 && valrec->sl == 0;
   return result;
}


static inline bool
all_causes_same_status(Error_Info * ddc_excp, DDCA_Status psc) {
   bool all_same = true;
   for (int ndx = 0; ndx < ddc_excp->cause_ct; ndx++) {
      if (ddc_excp->causes[ndx]->status_code != psc) {
         all_same = false;
         break;
      }
   }
   return all_same;
}



/** Collects initial monitor checks to perform them on a single open of the
 *  monitor device, and to avoid repeating them.
 *
 *  Performs the following tests:
 *  - Checks that DDC communication is working.
 *  - Checks if the monitor uses DDC Null Response to indicate invalid VCP code
 *  - Checks if the monitor uses mh=ml=sh=sl=0 to indicate invalid VCP code
 *
 *  @param dh  pointer to #Display_Handle for open monitor device
 *  @return **true** if DDC communication with the display succeeded, **false** otherwise.
 *
 *  @remark
 *  Sets bits in dh->dref->flags
 *   *  @remark
 *  It has been observed that DDC communication can fail even if slave address x37
 *  is valid on the I2C bus.
 *  @remark
 *  ADL does not notice that a reported display, e.g. Dell 1905FP, does not support
 *  DDC.
 *  @remark
 *  Monitors are supposed to set the unsupported feature bit in a valid DDC
 *  response, but a few monitors (mis)use the Null Response instead to indicate
 *  an unsupported feature. Others return with the unsupported feature bit not
 *  set, but all bytes (mh, ml, sh, sl) zero.
 *  @remark
 *  Note that the test here is not perfect, as a Null Response might
 *  in fact indicate a transient error, but that is rare.
 *  @remark
 *  Output level should have been set <= DDCA_OL_NORMAL prior to this call since
 *  verbose output is distracting.
 */
// static   // non-static for backtrace
bool
ddc_initial_checks_by_dh(Display_Handle * dh) {
   bool debug = false;
   TRACED_ASSERT(dh && dh->dref);
   DBGTRC_STARTING(debug, TRACE_GROUP, "dh=%s", dh_repr(dh));
   DBGTRC_NOPREFIX(debug, TRACE_GROUP, "communication flags: %s", interpret_dref_flags_t(dh->dref->flags));

   DDCA_Any_Vcp_Value * pvalrec;

   if (!(dh->dref->flags & DREF_DDC_COMMUNICATION_CHECKED)) {
      Error_Info * ddc_excp = ddc_get_vcp_value(dh, 0x00, DDCA_NON_TABLE_VCP_VALUE, &pvalrec);
      Public_Status_Code psc = (ddc_excp) ? ddc_excp->status_code : 0;
      DBGTRC_NOPREFIX(debug, TRACE_GROUP, "ddc_get_vcp_value() for feature 0x00 returned: %s, pvalrec=%p",
                             errinfo_summary(ddc_excp), pvalrec);
      TRACED_ASSERT( (psc == 0 && pvalrec) || (psc != 0 && !pvalrec) );

      DDCA_IO_Mode io_mode = dh->dref->io_path.io_mode;
      if (io_mode == DDCA_IO_USB) {
         if (psc == 0 || psc == DDCRC_DETERMINED_UNSUPPORTED) {
            dh->dref->flags |= DREF_DDC_COMMUNICATION_WORKING;
         }
      }
      else {
         TRACED_ASSERT(psc != DDCRC_DETERMINED_UNSUPPORTED);  // only set at higher levels, unless USB

         if (psc == DDCRC_RETRIES) {
            DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "Try errors: %s", errinfo_causes_string(ddc_excp));
            if (all_causes_same_status(ddc_excp, DDCRC_NULL_RESPONSE))
               psc = DDCRC_ALL_RESPONSES_NULL;
         }

         // What if returns -EIO?  Dell AW3418D returns -EIO for unsupported features
         // EXCEPT that it returns mh=ml=sh=sl=0 for feature 0x00  (2/2019)

         if ( psc == DDCRC_NULL_RESPONSE        ||
              psc == DDCRC_ALL_RESPONSES_NULL   ||
              psc == 0                          ||
              psc == DDCRC_REPORTED_UNSUPPORTED )
         {
            dh->dref->flags |= DREF_DDC_COMMUNICATION_WORKING;

            if (psc == DDCRC_REPORTED_UNSUPPORTED)
               dh->dref->flags |= DREF_DDC_USES_DDC_FLAG_FOR_UNSUPPORTED;

            else if ( (psc == DDCRC_NULL_RESPONSE || psc == DDCRC_ALL_RESPONSES_NULL) &&
                      !ddc_never_uses_null_response_for_unsupported)
            {
               // get a feature that always exists
               Parsed_Nontable_Vcp_Response* parsed_response_loc = NULL;
               Error_Info * ddc_excp = ddc_get_nontable_vcp_value(dh, 0x10, &parsed_response_loc);
               Public_Status_Code psc = ERRINFO_STATUS(ddc_excp);
               DBGTRC_NOPREFIX(debug, TRACE_GROUP,
                     "ddc_get_nontable_vcp_value() for feature 0x10 returned: %s",
                     errinfo_summary(ddc_excp) );
               free(parsed_response_loc);
               if (psc == DDCRC_RETRIES) {
                  DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "Try errors: %s", errinfo_causes_string(ddc_excp));
                  if (all_causes_same_status(ddc_excp, DDCRC_NULL_RESPONSE))
                     psc = DDCRC_ALL_RESPONSES_NULL;
               }
               if (psc == 0) {
                  // feature x10 succeeded, so Null Msg for feature x00 really means unsupported
                  dh->dref->flags |= DREF_DDC_USES_NULL_RESPONSE_FOR_UNSUPPORTED;
               }
               else if (psc == DDCRC_NULL_RESPONSE || psc == DDCRC_ALL_RESPONSES_NULL) {
                  dh->dref->flags &= ~DREF_DDC_COMMUNICATION_WORKING;
               }
               else {
                  dh->dref->flags &= ~DREF_DDC_COMMUNICATION_WORKING;
               }
               errinfo_free(ddc_excp);
            }
            else {
               TRACED_ASSERT( psc == 0);
               TRACED_ASSERT(pvalrec && pvalrec->value_type == DDCA_NON_TABLE_VCP_VALUE );
               DBGTRC_NOPREFIX(debug, TRACE_GROUP, "pvalrec: value_type=%d, mh=%d, ml=%d, sh=%d, sl=%d",
                        pvalrec->value_type, pvalrec->val.c_nc.mh, pvalrec->val.c_nc.ml,
                        pvalrec->val.c_nc.sh, pvalrec->val.c_nc.sl);

               if (value_bytes_zero_for_any_value(pvalrec))
               {
                  // try another feature that should never exist
                  // ignoring the vanishingly small possibility that this actually is a CRT
                  Parsed_Nontable_Vcp_Response* parsed_response_loc = NULL;
                  Error_Info * ddc_excp = ddc_get_nontable_vcp_value(dh, 0x41, &parsed_response_loc);
                  Public_Status_Code psc = ERRINFO_STATUS(ddc_excp);
                  DBGTRC_NOPREFIX(debug, TRACE_GROUP,
                        "ddc_get_nontable_vcp_value() for feature 0x41 returned: %s",
                        errinfo_summary(ddc_excp) );
                  if (psc == 0) {
                     if (value_bytes_zero_for_nontable_value(parsed_response_loc)) {
                        DBGTRC_NOPREFIX(debug, TRACE_GROUP, "Setting DREF_DDC_USES_MH_ML_SH_SL_ZERO_FOR_UNSUPPORTED");
                        dh->dref->flags |= DREF_DDC_USES_MH_ML_SH_SL_ZERO_FOR_UNSUPPORTED;
                     }
                     else {
                        // Time to stop chasing cases with vanishingly small probabilities
                        MSG_W_SYSLOG(DDCA_SYSLOG_WARNING, "Feature x41 should not exist but returns non-zero value");
                        // just use the normal case
                        dh->dref->flags |= DREF_DDC_USES_NULL_RESPONSE_FOR_UNSUPPORTED;
                     }
                  }
                  else if (psc == DDCRC_REPORTED_UNSUPPORTED) {
                     // feature x00 really was a supported feature
                     dh->dref->flags |= DREF_DDC_USES_DDC_FLAG_FOR_UNSUPPORTED;
                  }
                  else if (psc == DDCRC_ALL_RESPONSES_NULL || psc == DDCRC_NULL_RESPONSE) {
                     // is it an error or indication of unsupported feature?
                     dh->dref->flags &= ~DREF_DDC_COMMUNICATION_WORKING;
                  }

                  else {
                     dh->dref->flags &= ~DREF_DDC_COMMUNICATION_WORKING;

                  }
                  free(parsed_response_loc);
               }
               else {
                  DBGTRC_NOPREFIX(debug, TRACE_GROUP, "Setting DREF_DDC_DOES_NOT_INDICATE_UNSUPPORTED");
                  dh->dref->flags |= DREF_DDC_DOES_NOT_INDICATE_UNSUPPORTED;
               }
            }
         }  // end, communication working

         else {   // communication failed
            if (psc == -EBUSY) {
               dh->dref->flags |= DREF_DDC_BUSY;
            }
            else if ( i2c_force_bus /* && psc == DDCRC_RETRIES */) {
               DBGTRC_NOPREFIX(debug || true , TRACE_GROUP, "dh=%s, Forcing DDC communication success.",
                     dh_repr(dh) );
               dh->dref->flags |= DREF_DDC_COMMUNICATION_WORKING;
            // dh->dref->flags |= DREF_DDC_DOES_NOT_INDICATE_UNSUPPORTED;
               dh->dref->flags |= DREF_DDC_USES_DDC_FLAG_FOR_UNSUPPORTED;   // good_enuf_for_test
               if ( vcp_version_eq(dh->dref->vcp_version_xdf, DDCA_VSPEC_UNQUERIED))  // may have been forced by option --mccs
                  dh->dref->vcp_version_xdf = DDCA_VSPEC_V22;   // good enuf for test
            }
         }
      }    // end, io_mode == DDC_IO_I2C
      dh->dref->flags |= DREF_DDC_COMMUNICATION_CHECKED;
      if (ddc_excp)
         errinfo_free(ddc_excp);

      if ( dh->dref->flags & DREF_DDC_COMMUNICATION_WORKING ) {
         // Would prefer to defer checking version until actually needed to avoid additional DDC io
         // during monitor detection.  Unfortunately, this would introduce ddc_open_display(), with
         // its possible error states, into other functions, e.g. ddca_get_feature_list_by_dref()
         if ( vcp_version_eq(dh->dref->vcp_version_xdf, DDCA_VSPEC_UNQUERIED)) { // may have been forced by option --mccs
            set_vcp_version_xdf_by_dh(dh);
         }
      }

   }  // end, !DREF_DDC_COMMUNICATION_CHECKED

   // can only pass a variable, not an expression or constant, to DBGTRC_RET_BOOL()
   // because failure simulation may assign a new value to the variable
   bool result =  dh->dref->flags & DREF_DDC_COMMUNICATION_WORKING;
   DBGTRC_RET_BOOL(debug, TRACE_GROUP, result, "dh=%s", dh_repr(dh));
   DBGTRC_NOPREFIX(debug, TRACE_GROUP, "communication flags: %s", interpret_dref_flags_t(dh->dref->flags));
   return  result;
}


/** Given a #Display_Ref, opens the monitor device and calls #initial_checks_by_dh()
 *  to perform initial monitor checks.
 *
 *  @param dref pointer to #Display_Ref for monitor
 *  @return **true** if DDC communication with the display succeeded, **false** otherwise.
 */
bool
ddc_initial_checks_by_dref(Display_Ref * dref) {
   bool debug = false;
   DBGTRC_STARTING(debug, TRACE_GROUP, "dref=%s", dref_repr_t(dref));
   DBGTRC_NOPREFIX(debug, TRACE_GROUP, "dref->flags: %s", interpret_dref_flags_t(dref->flags));

   bool result = false;
   Display_Handle * dh = NULL;
   Error_Info * err = NULL;

   err = ddc_open_display(dref, CALLOPT_ERR_MSG, &dh);
   if (!err)  {
      result = ddc_initial_checks_by_dh(dh);
      ddc_close_display_wo_return(dh);
   }
   else {
      char * msg = g_strdup_printf("Unable to open %s: %s", dpath_repr_t(&dref->io_path), psc_desc(err->status_code));
      SYSLOG2(DDCA_SYSLOG_WARNING, "%s", msg);
      free(msg);
   }
   dref->flags |= DREF_DDC_COMMUNICATION_CHECKED;
   if (err && err->status_code == -EBUSY)
      dref->flags |= DREF_DDC_BUSY;

   result = (!err);
   DBGTRC_DONE(debug, TRACE_GROUP, "Returning %s. dref = %s", sbool(result), dref_repr_t(dref) );
   DBGTRC_NOPREFIX(debug, TRACE_GROUP, "communication flags: %s", interpret_dref_flags_t(dref->flags));
   if (err)
      errinfo_free(err);
   return result;
}


/** Performs initial checks in a thread
 *
 *  @param data display reference
 */
void *
threaded_initial_checks_by_dref(gpointer data) {
   bool debug = false;

   Display_Ref * dref = data;
   TRACED_ASSERT(memcmp(dref->marker, DISPLAY_REF_MARKER, 4) == 0 );
   DBGTRC_STARTING(debug, TRACE_GROUP, "dref = %s", dref_repr_t(dref) );

   ddc_initial_checks_by_dref(dref);
   // g_thread_exit(NULL);
   DBGTRC_DONE(debug, TRACE_GROUP, "Returning NULL. dref = %s,", dref_repr_t(dref) );
   return NULL;
}


/** Spawns threads to perform initial checks and waits for them all to complete.
 *
 *  @param all_displays #GPtrArray of pointers to #Display_Ref
 */
void ddc_async_scan(GPtrArray * all_displays) {
   bool debug = false;
   DBGTRC_STARTING(debug, TRACE_GROUP, "all_displays=%p, display_count=%d", all_displays, all_displays->len);

   GPtrArray * threads = g_ptr_array_new();
   for (int ndx = 0; ndx < all_displays->len; ndx++) {
      Display_Ref * dref = g_ptr_array_index(all_displays, ndx);
      TRACED_ASSERT( memcmp(dref->marker, DISPLAY_REF_MARKER, 4) == 0 );

      GThread * th =
      g_thread_new(
            dref_repr_t(dref),                // thread name
            threaded_initial_checks_by_dref,
            dref);                            // pass pointer to display ref as data
      g_ptr_array_add(threads, th);
   }
   DBGMSF(debug, "Started %d threads", threads->len);
   for (int ndx = 0; ndx < threads->len; ndx++) {
      GThread * thread = g_ptr_array_index(threads, ndx);
      g_thread_join(thread);  // implicitly unrefs the GThread
   }
   DBGMSF(debug, "Threads joined");
   g_ptr_array_free(threads, true);

   DBGTRC_DONE(debug, TRACE_GROUP, "");
}


/** Loops through a list of display refs, performing initial checks on each.
 *
 *  @param all_displays #GPtrArray of pointers to #Display_Ref
 */
void
ddc_non_async_scan(GPtrArray * all_displays) {
   bool debug = false;
   DBGTRC_STARTING(debug, TRACE_GROUP, "checking %d displays", all_displays->len);

   for (int ndx = 0; ndx < all_displays->len; ndx++) {
      Display_Ref * dref = g_ptr_array_index(all_displays, ndx);
      TRACED_ASSERT( memcmp(dref->marker, DISPLAY_REF_MARKER, 4) == 0 );
      ddc_initial_checks_by_dref(dref);
   }
   DBGTRC_DONE(debug, TRACE_GROUP, "");
}


//
// Functions to get display information
//

/** Gets a list of all detected displays, whether they support DDC or not.
 *
 *  Detection must already have occurred.
 *
 *  @return **GPtrArray of #Display_Ref instances
 */
GPtrArray *
ddc_get_all_displays() {
   // ddc_ensure_displays_detected();
   TRACED_ASSERT(all_displays);
   return all_displays;
}


/** Gets a list of all detected displays, optionally excluding those
 *  that are invalid.
 *
 *  @return **GPtrArray of #Display_Ref instances
 */
GPtrArray *
ddc_get_filtered_displays(bool include_invalid_displays) {
   bool debug = false;
   DBGTRC_STARTING(debug, TRACE_GROUP, "include_invalid_displays=%s", sbool(include_invalid_displays));
   TRACED_ASSERT(all_displays);
   GPtrArray * result = g_ptr_array_sized_new(all_displays->len);
   for (int ndx = 0; ndx < all_displays->len; ndx++) {
      Display_Ref * cur = g_ptr_array_index(all_displays, ndx);
      if (include_invalid_displays || cur->dispno > 0) {
         g_ptr_array_add(result, cur);
      }
   }
   DBGTRC_DONE(debug, TRACE_GROUP, "Returning array of size %d", result->len);
   if (debug || IS_TRACING()) {
      ddc_dbgrpt_drefs("Display_Refs:", result, 2);
   }
   return result;
}


Display_Ref * ddc_get_display_ref_by_drm_connector(const char * connector_name, bool ignore_invalid) {
   bool debug = true;
   DBGTRC_STARTING(debug, TRACE_GROUP, "connector_name=%s, ignore_invalid=%s", connector_name, sbool(ignore_invalid));
   Display_Ref * result = NULL;
   TRACED_ASSERT(all_displays);
   for (int ndx = 0; ndx < all_displays->len; ndx++) {
      Display_Ref * cur = g_ptr_array_index(all_displays, ndx);
      bool pass_filter = true;
      if (ignore_invalid) {
         pass_filter = (cur->dispno > 0 || !(cur->flags&DREF_REMOVED));
      }
      if (pass_filter) {
         if (cur->io_path.io_mode == DDCA_IO_I2C) {
            I2C_Bus_Info * businfo = cur->detail;
            if (!businfo) {
               SEVEREMSG("active display ref has no bus info");
               continue;
            }
            // TODO: handle drm_connector_name not yet checked
            if (businfo->drm_connector_name && streq(businfo->drm_connector_name, connector_name)) {
               result = cur;
               break;
            }
         }
      }
   }

   DBGTRC_DONE(debug, TRACE_GROUP, "Returning %p", result);
   return result;
}


/** Returns the number of detected displays.
 *
 *  @param  include_invalid_displays
 *  @return number of displays, 0 if display detection has not yet occurred.
 */
int
ddc_get_display_count(bool include_invalid_displays) {
   int display_ct = -1;
   if (all_displays) {
      display_ct = 0;
      for (int ndx=0; ndx<all_displays->len; ndx++) {
         Display_Ref * dref = g_ptr_array_index(all_displays, ndx);
         TRACED_ASSERT(memcmp(dref->marker, DISPLAY_REF_MARKER, 4) == 0);
         if (dref->dispno > 0 || include_invalid_displays) {
            display_ct++;
         }
      }
   }
   return display_ct;
}


/** Returns list of all open() errors encountered during display detection.
 *
 *  @return **GPtrArray of #Bus_Open_Error.
 */
GPtrArray *
ddc_get_bus_open_errors() {
   return display_open_errors;
}




//
// Phantom displays
//

static bool
edid_ids_match(Parsed_Edid * edid1, Parsed_Edid * edid2) {
   bool result = false;
   result = streq(edid1->mfg_id,        edid2->mfg_id)        &&
            streq(edid1->model_name,    edid2->model_name)    &&
            edid1->product_code      == edid2->product_code   &&
            streq(edid1->serial_ascii,  edid2->serial_ascii)  &&
            edid1->serial_binary     == edid2->serial_binary;
   return result;
}


/** Check if an invalid #Display_Reference can be regarded as a phantom
 *  of a given valid #Display_Reference.
 *
 *  @param  invalid_dref
 *  @param  valid_dref
 *  @return true/false
 *
 *  - Both are /dev/i2c devices
 *  - The EDID id fields must match
 *  - For the invalid #Display_Reference:
 *    - attribute status must exist and equal "disconnected"
 *    - attribute enabled must exist and equal "disabled"
 *    - attribute edid must not exist
 */
bool
is_phantom_display(Display_Ref* invalid_dref, Display_Ref * valid_dref) {
   bool debug = false;
   char * invalid_repr = g_strdup(dref_repr_t(invalid_dref));
   char *   valid_repr = g_strdup(dref_repr_t(valid_dref));
   DBGTRC_STARTING(debug, TRACE_GROUP, "invalid_dref=%s, valid_dref=%s",
                 invalid_repr, valid_repr);
   free(invalid_repr);
   free(valid_repr);

   bool result = false;
   // User report has shown that 128 byte EDIDs can differ for the valid and
   // invalid display.  Specifically, byte 24 was seen to differ, with one
   // having RGB 4:4:4 and the other RGB 4:4:4 + YCrCb 4:2:2!.  So instead of
   // simply byte comparing the 2 EDIDs, check the identifiers.
   if (edid_ids_match(invalid_dref->pedid, valid_dref->pedid)) {
      DBGTRC_NOPREFIX(debug, TRACE_GROUP, "EDIDs match");
      if (invalid_dref->io_path.io_mode == DDCA_IO_I2C &&
            valid_dref->io_path.io_mode == DDCA_IO_I2C)
      {
         int invalid_busno = invalid_dref->io_path.path.i2c_busno;
         // int valid_busno = valid_dref->io_path.path.i2c_busno;
         char buf0[40];
         snprintf(buf0, 40, "/sys/bus/i2c/devices/i2c-%d", invalid_busno);
         bool old_silent = set_rpt_sysfs_attr_silent(!(debug|| IS_TRACING()));
         char * invalid_rpath = NULL;
         bool ok = RPT_ATTR_REALPATH(0, &invalid_rpath, buf0, "device");
         if (ok) {
            result = true;
            char * attr_value = NULL;
            ok = RPT_ATTR_TEXT(0, &attr_value, invalid_rpath, "status");
            if (!ok  || !streq(attr_value, "disconnected"))
               result = false;
            ok = RPT_ATTR_TEXT(0, &attr_value, invalid_rpath, "enabled");
            if (!ok  || !streq(attr_value, "disabled"))
               result = false;
            GByteArray * edid;
            ok = RPT_ATTR_EDID(0, &edid, invalid_rpath, "edid");    // is "edid" needed
            if (ok) {
               result = false;
               g_byte_array_free(edid, true);
            }
         }
         set_rpt_sysfs_attr_silent(old_silent);
      }
   }
   DBGTRC_DONE(debug, TRACE_GROUP,    "Returning: %s", sbool(result) );
   return result;
}


/** Mark phantom displays.
 *
 *  Solit the #Display_Ref's in a GPtrArray into those that have
 *  already been determined to be valid (dispno > 0) and those
 *  that are invalid (dispno < 0).
 *
 *  For each invalid array, check to see if it is a phantom display
 *  corresponding to one of the valid displays.  If so, set its dispno
 *  to DISPNO_INVALID and save a pointer to the valid display ref.
 *
 *  @param all_displays array of pointers to #Display_Ref
 *
 *  @remark
 *  This handles the case where DDC communication works for one
 *  /dev/i2c bus but not another.  It does not handle the case where
 *  communication succeeds on both /dev/i2c devices.
 */
void
filter_phantom_displays(GPtrArray * all_displays) {
   bool debug = false;
   DBGTRC_STARTING(debug, TRACE_GROUP, "all_displays->len = %d", all_displays->len);
   GPtrArray* valid_displays   = g_ptr_array_sized_new(all_displays->len);
   GPtrArray* invalid_displays = g_ptr_array_sized_new(all_displays->len);
   for (int ndx = 0; ndx < all_displays->len; ndx++) {
      Display_Ref * dref = g_ptr_array_index(all_displays, ndx);
      TRACED_ASSERT( memcmp(dref->marker, DISPLAY_REF_MARKER, 4) == 0 );
      if (dref->dispno < 0)     // DISPNO_INVALID, DISPNO_PHANTOM, DISPNO_REMOVED
         g_ptr_array_add(invalid_displays, dref);
      else
         g_ptr_array_add(valid_displays, dref);
   }
   DBGTRC_NOPREFIX(debug, TRACE_GROUP, "%d valid displays, %d invalid displays",
                              valid_displays->len, invalid_displays->len);
   if (invalid_displays->len > 0 || valid_displays->len == 0 ) {
      for (int invalid_ndx = 0; invalid_ndx < invalid_displays->len; invalid_ndx++) {
         Display_Ref * invalid_ref = g_ptr_array_index(invalid_displays, invalid_ndx);
         for (int valid_ndx = 0; valid_ndx < valid_displays->len; valid_ndx++) {
            Display_Ref *  valid_ref = g_ptr_array_index(valid_displays, valid_ndx);
            if (is_phantom_display(invalid_ref, valid_ref)) {
               invalid_ref->dispno = DISPNO_PHANTOM;    // -2
               invalid_ref->actual_display = valid_ref;
            }
         }
      }
   }
   g_ptr_array_free(invalid_displays, true);
   g_ptr_array_free(valid_displays, true);
   DBGTRC_DONE(debug, TRACE_GROUP, "");
}


//
// Display Detection
//

/** Emits a debug report of a list of #Bus_Open_Error.
 *
 *  @param open_errors  array of #Bus_Open_Error
 *  @param depth        logical indentation depth
 */
void dbgrpt_bus_open_errors(GPtrArray * open_errors, int depth) {
   int d1 = depth+1;
   if (!open_errors || open_errors->len == 0) {
      rpt_vstring(depth, "Bus open errors:  None");
   }
   else {
      rpt_vstring(depth, "Bus open errors:");
      for (int ndx = 0; ndx < open_errors->len; ndx++) {
         Bus_Open_Error * cur = g_ptr_array_index(open_errors, ndx);
         rpt_vstring(d1, "%s bus:  %-2d, error: %d, detail: %s",
               (cur->io_mode == DDCA_IO_I2C) ? "I2C" : "hiddev",
               cur->devno, cur->error, cur->detail);
      }
   }
}


/** Detects all connected displays by querying the I2C and USB subsystems.
 *
 *  @param  open_errors_loc where to return address of #GPtrArray of #Bus_Open_Error
 *  @return array of #Display_Ref
 */
// static
GPtrArray *
ddc_detect_all_displays(GPtrArray ** i2c_open_errors_loc) {
   bool debug = false;
   DBGTRC_STARTING(debug, TRACE_GROUP, "");
   dispno_max = 0;
   GPtrArray * bus_open_errors = g_ptr_array_new();
   GPtrArray * display_list = g_ptr_array_new();

   int busct = i2c_detect_buses();
   DBGMSF(debug, "i2c_detect_buses() returned: %d", busct);
   guint busndx = 0;
   for (busndx=0; busndx < busct; busndx++) {
      I2C_Bus_Info * businfo = i2c_get_bus_info_by_index(busndx);
      if ( (businfo->flags & I2C_BUS_ADDR_0X50)  && businfo->edid ) {
         Display_Ref * dref = NULL;
         if (display_caching_enabled) {
            dref = copy_display_ref(ddc_find_deserialized_display(businfo->busno, businfo->edid->bytes));
            if (dref)
               dref->detail = businfo;
         }
         if (!dref) {
            dref = create_bus_display_ref(businfo->busno);
            dref->dispno = DISPNO_INVALID;   // -1, guilty until proven innocent
            dref->pedid = copy_parsed_edid(businfo->edid);    // needed?
            dref->mmid  = monitor_model_key_new(dref->pedid->mfg_id,
                                                dref->pedid->model_name,
                                                dref->pedid->product_code);
            dref->detail = businfo;
            dref->flags |= DREF_DDC_IS_MONITOR_CHECKED;
            dref->flags |= DREF_DDC_IS_MONITOR;
         }
         g_ptr_array_add(display_list, dref);
         // dbgrpt_display_ref(dref,5);
      }
      else if ( !(businfo->flags & I2C_BUS_ACCESSIBLE) ) {
         Bus_Open_Error * boe = calloc(1, sizeof(Bus_Open_Error));
         boe->io_mode = DDCA_IO_I2C;
         boe->devno = businfo->busno;
         boe->error = businfo->open_errno;
         g_ptr_array_add(bus_open_errors, boe);
      }
   }

#ifdef USE_USB
   if (detect_usb_displays) {
      GPtrArray * usb_monitors = get_usb_monitor_list();  // array of USB_Monitor_Info
      // DBGMSF(debug, "Found %d USB displays", usb_monitors->len);
      for (int ndx=0; ndx<usb_monitors->len; ndx++) {
         Usb_Monitor_Info  * curmon = g_ptr_array_index(usb_monitors,ndx);
         TRACED_ASSERT(memcmp(curmon->marker, USB_MONITOR_INFO_MARKER, 4) == 0);
         Display_Ref * dref = create_usb_display_ref(
                                   curmon->hiddev_devinfo->busnum,
                                   curmon->hiddev_devinfo->devnum,
                                   curmon->hiddev_device_name);
         dref->dispno = DISPNO_INVALID;   // -1
         dref->pedid = copy_parsed_edid(curmon->edid);
         if (dref->pedid)
            dref->mmid  = monitor_model_key_new(
                             dref->pedid->mfg_id,
                             dref->pedid->model_name,
                             dref->pedid->product_code);
         else
            dref->mmid = monitor_model_key_new("UNK", "UNK", 0);
         // drec->detail.usb_detail = curmon;
         dref->detail = curmon;
         dref->flags |= DREF_DDC_IS_MONITOR_CHECKED;
         dref->flags |= DREF_DDC_IS_MONITOR;
         g_ptr_array_add(display_list, dref);
      }

      GPtrArray * usb_open_errors = get_usb_open_errors();
      if (usb_open_errors && usb_open_errors->len > 0) {
         for (int ndx = 0; ndx < usb_open_errors->len; ndx++) {
            Bus_Open_Error * usb_boe = (Bus_Open_Error *) g_ptr_array_index(usb_open_errors, ndx);
            Bus_Open_Error * boe_copy = calloc(1, sizeof(Bus_Open_Error));
            boe_copy->io_mode = DDCA_IO_USB;
            boe_copy->devno   = usb_boe->devno;
            boe_copy->error   = usb_boe->error;
            boe_copy->detail  = usb_boe->detail;
            g_ptr_array_add(bus_open_errors, boe_copy);
         }
      }
   }
#endif

   // verbose output is distracting within scans
   // saved and reset here so that async threads are not adjusting output level
   DDCA_Output_Level olev = get_output_level();
   if (olev == DDCA_OL_VERBOSE)
      set_output_level(DDCA_OL_NORMAL);

   DBGMSF(debug, "display_list->len=%d, async_threshold=%d",
                 display_list->len, async_threshold);
   if (display_list->len >= async_threshold)
      ddc_async_scan(display_list);
   else
      ddc_non_async_scan(display_list);

   if (olev == DDCA_OL_VERBOSE)
      set_output_level(olev);

   // assign display numbers
   for (int ndx = 0; ndx < display_list->len; ndx++) {
      Display_Ref * dref = g_ptr_array_index(display_list, ndx);
      TRACED_ASSERT( memcmp(dref->marker, DISPLAY_REF_MARKER, 4) == 0 );
      if (dref->flags & DREF_DDC_COMMUNICATION_WORKING)
         dref->dispno = ++dispno_max;
      else if (dref->flags & DREF_DDC_BUSY)
         dref->dispno = DISPNO_BUSY;
      else {
         dref->dispno = DISPNO_INVALID;   // -1;
      }
   }

   filter_phantom_displays(display_list);

   if (bus_open_errors->len > 0) {
      *i2c_open_errors_loc = bus_open_errors;
   }
   else {
      g_ptr_array_free(bus_open_errors, false);
      *i2c_open_errors_loc = NULL;
   }

   if (debug) {
      DBGMSG("Displays detected:");
      ddc_dbgrpt_drefs("display_list:", display_list, 1);
      dbgrpt_bus_open_errors(*i2c_open_errors_loc, 1);
   }
   DBGTRC_DONE(debug, TRACE_GROUP, "Returning %p, Detected %d valid displays",
                display_list, dispno_max);
   return display_list;
}


/** Initializes the master display list in global variable #all_displays and
 *  records open errors in global variable #display_open_errors.
 *
 *  Does nothing if the list has already been initialized.
 */
void
ddc_ensure_displays_detected() {
   bool debug = false;
   DBGTRC_STARTING(debug, TRACE_GROUP, "");
   if (!all_displays) {
      // i2c_detect_buses();  // called in ddc_detect_all_displays()
      all_displays = ddc_detect_all_displays(&display_open_errors);
   }
   DBGTRC_DONE(debug, TRACE_GROUP,
               "all_displays=%p, all_displays has %d displays",
               all_displays, all_displays->len);
}


/** Discards all detected displays.
 *
 *  - All open displays are closed
 *  - The list of open displays in #all_displays is discarded
 *  - The list of errors in #display_open_errors is discarded
 *  - The list of detected I2C buses is discarded
 *  - The USB monitor list is discarded
 */
void
ddc_discard_detected_displays() {
   bool debug = false;
   DBGTRC_STARTING(debug, TRACE_GROUP, "");
   // grab locks to prevent any opens?
   ddc_close_all_displays();
#ifdef USE_USB
   discard_usb_monitor_list();
#endif
   if (all_displays) {
      for (int ndx = 0; ndx < all_displays->len; ndx++) {
         Display_Ref * dref = g_ptr_array_index(all_displays, ndx);
         dref->flags |= DREF_TRANSIENT;  // hack to allow all Display References to be freed
#ifndef NDEBUG
         DDCA_Status ddcrc = free_display_ref(dref);
         TRACED_ASSERT(ddcrc==0);
#endif
      }
      g_ptr_array_free(all_displays, true);
      all_displays = NULL;
      if (display_open_errors) {
         g_ptr_array_free(display_open_errors, true);
         display_open_errors = NULL;
      }
   }
   free_sys_drm_connectors();
   i2c_discard_buses();
   DBGTRC_DONE(debug, TRACE_GROUP, "");
}


void
ddc_redetect_displays() {
   bool debug = false;
   DBGTRC_STARTING(debug, TRACE_GROUP, "all_displays=%p", all_displays);
   ddc_discard_detected_displays();
   // i2c_detect_buses(); // called in ddc_detect_all_displays()
   all_displays = ddc_detect_all_displays(&display_open_errors);
   if (debug) {
      ddc_dbgrpt_drefs("all_displays:", all_displays, 1);
      // dbgrpt_valid_display_refs(1);
   }
   DBGTRC_DONE(debug, TRACE_GROUP, "all_displays=%p, all_displays->len = %d",
                                   all_displays, all_displays->len);
}


/** Checks that a #Display_Ref is in array **all_displays**
 *  of all valid #Display_Ref values.
 *
 *  @param  dref  #Display_Ref
 *  @return true/false
 */
bool
ddc_is_valid_display_ref(Display_Ref * dref) {
   bool debug = false;
   DBGTRC_STARTING(debug, TRACE_GROUP, "dref=%p -> %s", dref, dref_repr_t(dref));
   bool result = false;
   if (all_displays) {
      for (int ndx = 0; ndx < all_displays->len; ndx++) {
         Display_Ref* cur = g_ptr_array_index(all_displays, ndx);
         DBGMSF(debug, "Checking vs valid dref %p", cur);

         if (cur == dref) {
            // if (cur->dispno > 0)  // why?
               result = true;
            break;
         }
      }
   }
   DBGTRC_DONE(debug, TRACE_GROUP, "Returning %s. dref=%p, dispno=%d", sbool(result), dref, dref->dispno);
   return result;
}


/** Indicates whether displays have already been detected
 *
 *  @return true/false
 */
bool
ddc_displays_already_detected()
{
   bool debug = false;
   DBGTRC_EXECUTED(debug, TRACE_GROUP, "Returning %s", SBOOL(all_displays));
   return all_displays;
}


/** Controls whether USB displays are to be detected.
 *
 *  Must be called before any function that triggers display detection.
 *
 *  @param  onoff  true if USB displays are to be detected, false if not
 *  @retval DDCRC_OK  normal
 *  @retval DDCRC_INVALID_OPERATION function called after displays have been detected
 *  @retval DDCRC_UNIMPLEMENTED ddcutil was not built with USB support
 *
 *  @remark
 *  If this function is not called, the default (if built with USB support) is on
 */
DDCA_Status
ddc_enable_usb_display_detection(bool onoff) {
   bool debug = false;
   DBGMSF(debug, "Starting. onoff=%s", sbool(onoff));

   DDCA_Status rc = DDCRC_UNIMPLEMENTED;
#ifdef USE_USB
   if (ddc_displays_already_detected()) {
      rc = DDCRC_INVALID_OPERATION;
   }
   else {
      detect_usb_displays = onoff;
      rc = DDCRC_OK;
   }
#endif
   DBGMSF(debug, "Done.     Returning %s", psc_name_code(rc));
   return rc;
}


/** Indicates whether USB displays are to be detected
 *
 *  @return true/false
 */
bool
ddc_is_usb_display_detection_enabled() {
   return detect_usb_displays;
}


//
// Simple handling of display hotplug events
//

GPtrArray* display_hotplug_callbacks = NULL;

/** Registers a display hotplug event callback
 *
 * The function must be of type DDCA_Display_Hotplub_Callback_Func
 *
 *  @param func function to register
 *
 *  The function must be of type DDDCA_Display_Hotplug_Callback_Func.
 *  It is not an error if the function is already registered.
 */
DDCA_Status ddc_register_display_hotplug_callback(DDCA_Display_Hotplug_Callback_Func func) {
   bool ok = generic_register_callback(display_hotplug_callbacks, func);
   return (ok) ? DDCRC_OK : DDCRC_INVALID_OPERATION;
}


/** Deregisters a hotplug event callback function
 *
 *  @@aram function of type DDCA_Display_Hotplug_Callback_func
 */
/** Deregisters a hotplug event callback function
 *
 *  @param  function to dregister
 *  @retval DDCRC_OK normal return
 *  #reeval DDCRC_NOT_FOUND function not in list of registered functions
 */
DDCA_Status ddc_unregister_display_hotplug_callback(DDCA_Display_Hotplug_Callback_Func func) {
   bool ok =  generic_unregister_callback(display_hotplug_callbacks, func);
   return (ok) ? DDCRC_OK : DDCRC_NOT_FOUND;
}


/** Invokes the registered callbacks for a display hotplug event.
 */
void ddc_emit_display_hotplug_event() {
   bool debug = false;
   DBGTRC_STARTING(debug, TRACE_GROUP, "");
   if (display_hotplug_callbacks) {
      for (int ndx = 0; ndx < display_hotplug_callbacks->len; ndx++)  {
         DDCA_Display_Hotplug_Callback_Func func = g_ptr_array_index(display_hotplug_callbacks, ndx);
         func();
      }
   }
   DBGTRC_DONE(debug, TRACE_GROUP, "Executed %d callbacks",
         (display_hotplug_callbacks) ? display_hotplug_callbacks->len : 0);
}


#ifdef DETAILED_DISPLAY_CHANGE_HANDLING

//
// Modify local data structures before invoking client callback functions.
// Too many edge cases
//


GPtrArray* display_detection_callbacks = NULL;

bool ddc_register_display_detection_callback(DDCA_Display_Detection_Callback_Func func) {
   bool debug = true;
   DBGTRC_STARTING(debug, TRACE_GROUP, "func=%p");
   bool result = generic_register_callback(display_detection_callbacks, func);
   DBGTRC_RET_BOOL(debug, TRACE_GROUP, result, "");
   return result;
}

bool ddc_unregister_display_detection_callback(DDCA_Display_Detection_Callback_Func fund) {
   // TO DO
   return true;
}



void
ddc_emit_display_detection_event(DDCA_Display_Detection_Report report) {
   bool debug = true;
   DBGTRC_STARTING(debug, TRACE_GROUP, "dref=%s, operation=%d", dref_repr_t(report.dref), report.operation);
   if (display_detection_callbacks) {
      for (int ndx = 0; ndx < display_detection_callbacks->len; ndx++)  {
         DDCA_Display_Detection_Callback_Func func = g_ptr_array_index(display_detection_callbacks, ndx);
         func(report);
      }
   }
   DBGTRC_DONE(debug, TRACE_GROUP, "Executed %d callbacks",
         (display_detection_callbacks) ? display_detection_callbacks->len : 0);
}


/** Process a display removal event.
 *
 *  The currently active Display_Ref for the specified DRM connector
 *  name is located.  It is marked removed, and the associated
 *  I2C_Bus_Info struct is reset.
 *
 *  @param drm_connector    connector name, e.g. card0-DP-1
 *  @remark
 *  Does not handle displays using USB for communication
 */
bool ddc_remove_display_by_drm_connector(const char * drm_connector) {
   bool debug = true;
   DBGTRC_STARTING(debug, TRACE_GROUP, "drm_connector = %s", drm_connector);

   // DBGTRC_NOPREFIX(true, TRACE_GROUP, "All existing Bus_Info recs:");
   // i2c_dbgrpt_buses(/* report_all */ true, 2);

   bool found = false;
   assert(all_displays);
   for (int ndx = 0; ndx < all_displays->len; ndx++) {
      // If a display is repeatedly removed and added on a particular connector,
      // there will be multiple Display_Ref records.  All but one should already
      // be flagged DDCA_DISPLAY_REMOVED, and should not have a pointer to
      // an I2C_Bus_Info struct.
      Display_Ref * dref = g_ptr_array_index(all_displays, ndx);
      assert(dref);
      DBGMSG("Checking dref %s", dref_repr_t(dref));
      dbgrpt_display_ref(dref, 2);
      if (dref->io_path.io_mode == DDCA_IO_I2C) {
         if (dref->flags & DDCA_DISPLAY_REMOVED)  {
            DBGMSG("DDCA_DISPLAY_REMOVED set");
            continue;
         }
         I2C_Bus_Info * businfo = dref->detail;
         // DBGMSG("businfo = %p", businfo);
         assert(businfo);
         DBGMSG("Checking I2C_Bus_Info for %d", businfo->busno);
         if (!(businfo->flags & I2C_BUS_DRM_CONNECTOR_CHECKED))
            i2c_check_businfo_connector(businfo);
         DBGMSG("drm_connector_found_by = %s (%d)",
               drm_connector_found_by_name(businfo->drm_connector_found_by),
               businfo->drm_connector_found_by);
         if (businfo->drm_connector_found_by != DRM_CONNECTOR_NOT_FOUND) {
            DBGMSG("comparing %s", businfo->drm_connector_name);
            if (streq(businfo->drm_connector_name, drm_connector)) {
               DBGMSG("Found drm_connector %s", drm_connector);
               dref->flags |= DREF_REMOVED;
               i2c_reset_bus_info(businfo);
               DDCA_Display_Detection_Report report;
               report.operation = DDCA_DISPLAY_REMOVED;
               report.dref = dref;
               ddc_emit_display_detection_event(report);
               found = true;
               break;
            }
         }
      }
   }

   DBGTRC_RET_BOOL(debug, TRACE_GROUP, found, "");
   return found;
}


bool ddc_add_display_by_drm_connector(const char * drm_connector_name) {
   bool debug = true;
   DBGTRC_STARTING(debug, TRACE_GROUP, "drm_connector_name = %s", drm_connector_name);

   bool ok = false;
   Sys_Drm_Connector * conrec = find_sys_drm_connector(-1, NULL, drm_connector_name);
   if (conrec) {
      int busno = conrec->i2c_busno;
      // TODO: ensure that there's no I2c_Bus_Info record for the bus
      I2C_Bus_Info * businfo = i2c_find_bus_info_by_busno(busno);
      if (!businfo)
         businfo = i2c_new_bus_info(busno);
      if (businfo->flags&I2C_BUS_PROBED) {
         SEVEREMSG("Display added for I2C bus %d still marked in use", busno);
         i2c_reset_bus_info(businfo);
      }

      i2c_check_bus(businfo);
      if (businfo->flags & I2C_BUS_ADDR_0X50) {
         Display_Ref * old_dref = ddc_get_display_ref_by_drm_connector(drm_connector_name, /*ignore_invalid*/ false);
         if (old_dref) {
            SEVEREMSG("Active Display_Ref already exists for DRM connector %s", drm_connector_name);
            // how to handle?
            old_dref->flags |= DREF_REMOVED;
         }
         Display_Ref * dref = create_bus_display_ref(busno);
         dref->dispno = DISPNO_INVALID;   // -1, guilty until proven innocent
         dref->pedid = businfo->edid;    // needed?
         dref->mmid  = monitor_model_key_new(
                          dref->pedid->mfg_id,
                          dref->pedid->model_name,
                          dref->pedid->product_code);

         // drec->detail.bus_detail = businfo;
         dref->detail = businfo;
         dref->flags |= DREF_DDC_IS_MONITOR_CHECKED;
         dref->flags |= DREF_DDC_IS_MONITOR;

         g_ptr_array_add(all_displays, dref);

         DDCA_Display_Detection_Report report = {dref, DDCA_DISPLAY_ADDED};
         ddc_emit_display_detection_event(report);

         ok = true;
      }
   }

   DBGTRC_RET_BOOL(debug, TRACE_GROUP, ok, "");
   return ok;
}

#endif


void
init_ddc_displays() {
   RTTI_ADD_FUNC(ddc_async_scan);
   RTTI_ADD_FUNC(ddc_detect_all_displays);
   RTTI_ADD_FUNC(ddc_displays_already_detected);
   RTTI_ADD_FUNC(ddc_discard_detected_displays);
   RTTI_ADD_FUNC(ddc_get_all_displays);
   RTTI_ADD_FUNC(ddc_initial_checks_by_dh);
   RTTI_ADD_FUNC(ddc_initial_checks_by_dref);
   RTTI_ADD_FUNC(ddc_is_valid_display_ref);
   RTTI_ADD_FUNC(ddc_non_async_scan);
   RTTI_ADD_FUNC(ddc_redetect_displays);
   RTTI_ADD_FUNC(filter_phantom_displays);
   RTTI_ADD_FUNC(is_phantom_display);
   RTTI_ADD_FUNC(threaded_initial_checks_by_dref);
   RTTI_ADD_FUNC(ddc_get_display_ref_by_drm_connector);
   RTTI_ADD_FUNC(ddc_emit_display_hotplug_event);

#ifdef DETAILED_DISPLAY_CHANGE_HANDLING
   RTTI_ADD_FUNC(ddc_add_display_by_drm_connector);
   RTTI_ADD_FUNC(ddc_remove_display_by_drm_connector);
   RTTI_ADD_FUNC(ddc_register_display_detection_callback);
   RTTI_ADD_FUNC(ddc_unregister_display_detection_callback);
#endif
}


void terminate_ddc_displays() {
   ddc_discard_detected_displays();
}

