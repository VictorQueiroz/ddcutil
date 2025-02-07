/** \file demo_global_settings.c
 *
 * Sample program illustrating the use of libddcutil's functions for
 * querying build information and global settings management.
 */

// Copyright (C) 2014-2023 Sanford Rockowitz <rockowitz@minsoft.com>
// SPDX-License-Identifier: GPL-2.0-or-later

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "public/ddcutil_c_api.h"
#include "public/ddcutil_status_codes.h"


#define DDC_ERRMSG(function_name,status_code) \
   printf("(%s) %s() returned %d (%s): %s\n",      \
          __func__, function_name, status_code,    \
          ddca_rc_name(status_code),      \
          ddca_rc_desc(status_code))


void demo_build_information() {
   printf("\nProbe static build information...\n");

   // Get the ddcutil version as a string in the form "major.minor.micro".
   printf("   ddcutil version by ddca_ddcutil_version_string(): %s\n", ddca_ddcutil_version_string() );

   // Get the ddcutil version as a struct of integers
   DDCA_Ddcutil_Version_Spec vspec = ddca_ddcutil_version();
   printf("   ddcutil version by ddca_ddcutil_version():  %d.%d.%d\n", vspec.major, vspec.minor, vspec.micro);

   // Get build options
   uint8_t build_options = ddca_build_options();
   printf("   Built with ADL support:        %s\n", (build_options & DDCA_BUILT_WITH_ADL)     ? "yes" : "no");
   printf("   Built with USB support:        %s\n", (build_options & DDCA_BUILT_WITH_USB)     ? "yes" : "no");
   printf("   Built with failure simulation: %s\n", (build_options & DDCA_BUILT_WITH_FAILSIM) ? "yes" : "no");
}


int main(int argc, char** argv) {
   // Query library build settings.
   demo_build_information();
}
