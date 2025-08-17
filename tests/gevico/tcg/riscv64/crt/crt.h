/*
 * Gevico TCG system testcase head file
 *
 *  Copyright (c) chao.liu@yeah.net
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef GEVICO_CRT_H
#define GEVICO_CRT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <limits.h>
#include <stdarg.h>

#define HARTID ({                                \
    uint64_t _id;                                \
    asm volatile("csrr %0,mhartid" : "=r"(_id)); \
    _id;                                         \
})

#define crt_assert(condition) do {              \
    if (!(condition)) {                         \
        printf("Assertion failed: %s\n"         \
               "File: %s\n"                     \
               "Line: %d\n",                    \
               #condition, __FILE__, __LINE__); \
        crt_abort();                            \
        }                                       \
    } while(0)

/* CRT */
void crt_abort(void);

/* Memory */
void *memset(void *s, int c, size_t n);
void *memcpy(void *dest, const void *src, size_t n);

/* Console */
int printf(const char *s, ...);

#endif /* #define GEVICO_CRT_H */
