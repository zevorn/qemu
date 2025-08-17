/*
 * Gevico TCG system testcase memory source file
 *
 *  Copyright (c) chao.liu@yeah.net
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "crt.h"

void *memset(void *s, int c, size_t n)
{
    uint8_t *p = s;
    while (n--) {
        *p++ = c;
    }
    return s;
}

void *memcpy(void *dest, const void *src, size_t n)
{
    uint8_t *d = dest;
    const uint8_t *s = src;
    while (n--) {
        *d++ = *s++;
    }
    return dest;
}
