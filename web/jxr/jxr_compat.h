/* Two things jxrlib uses without declaring them, which compilers of its day
 * let pass and today's do not. Given to the compiler before each of its
 * files (-include), so its sources stay as they came.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <wchar.h>

unsigned int _byteswap_ulong(unsigned int bits); /* defined in image/sys/strcodec.c */
