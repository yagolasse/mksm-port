/* Stand-in for QEMU's qapi/error.h. SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once
#include "qemu/osdep.h"

struct Error { char *msg; };

void error_setg(Error **errp, const char *fmt, ...);
const char *error_get_pretty(const Error *err);
void error_free(Error *err);
