/* Stand-in for QEMU's qemu/error-report.h. SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once
#include "qemu/osdep.h"

#define error_report(fmt, ...) fprintf(stderr, "[XEMU] " fmt "\n", ##__VA_ARGS__)
#define warn_report(fmt, ...) fprintf(stderr, "[XEMU] warning: " fmt "\n", ##__VA_ARGS__)
#define info_report(fmt, ...) fprintf(stderr, "[XEMU] " fmt "\n", ##__VA_ARGS__)
#define error_report_once(fmt, ...) do { static bool once_; if (!once_) { once_ = true; \
    error_report(fmt, ##__VA_ARGS__); } } while (0)
#define warn_report_once(fmt, ...) do { static bool once_; if (!once_) { once_ = true; \
    warn_report(fmt, ##__VA_ARGS__); } } while (0)
