/* Stand-in for xemu's ui/xemu-settings.h. SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once
#include "xemu-config.h"

extern struct config g_config;

/* Folder for xemu's on-disk state (the GL shader cache), with a trailing separator. */
const char *xemu_settings_get_base_path(void);
