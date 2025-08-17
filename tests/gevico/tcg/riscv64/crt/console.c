/*
 * Gevico TCG testcase console source file
 *
 *  Copyright (c) chao.liu@yeah.net
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "crt.h"
#include "pl011.h"

#define PL011_IO_BASE        0x10000000UL
#define UART_DR        0x00 /* data register */
#define UART_RSR_ECR    0x04 /* receive status or error clear */
#define UART_DMAWM    0x08 /* DMA watermark configure */
#define UART_TIMEOUT    0x0C /* Timeout period */

/* reserved space */
#define UART_FR        0x18 /* flag register */
#define UART_ILPR    0x20 /* IrDA low-poer */
#define UART_IBRD    0x24 /* integer baud register */
#define UART_FBRD    0x28 /* fractional baud register */
#define UART_LCR_H    0x2C /* line control register */
#define UART_CR        0x30 /* control register */
#define UART_IFLS    0x34 /* interrupt FIFO level select */
#define UART_IMSC    0x38 /* interrupt mask set/clear */
#define UART_RIS    0x3C /* raw interrupt register */
#define UART_MIS    0x40 /* masked interrupt register */
#define UART_ICR    0x44 /* interrupt clear register */
#define UART_DMACR    0x48 /* DMA control register */

/* flag register bits */
#define UART_FR_RTXDIS    (1 << 13)
#define UART_FR_TERI    (1 << 12)
#define UART_FR_DDCD    (1 << 11)
#define UART_FR_DDSR    (1 << 10)
#define UART_FR_DCTS    (1 << 9)
#define UART_FR_RI    (1 << 8)
#define UART_FR_TXFE    (1 << 7)
#define UART_FR_RXFF    (1 << 6)
#define UART_FR_TXFF    (1 << 5)
#define UART_FR_RXFE    (1 << 4)
#define UART_FR_BUSY    (1 << 3)
#define UART_FR_DCD    (1 << 2)
#define UART_FR_DSR    (1 << 1)
#define UART_FR_CTS    (1 << 0)

static char *printbuf = (char *)0x82000000UL;
static uint32_t printlen[256];
static double exp10_table[] = {1e1L, 1e2L, 1e4L, 1e8L, 1e16L, 1e32L, 1e64L,
                               1e128L, 1e256L};

#define BUFLEN 0x1000
#define DEFAULT_PREC 6
#define GET_MAX(m, n) (m > n ? m : n)
#define GET_MIN(m, n) (m < n ? m : n)
#define EXP10_TABLE_SIZE (sizeof(exp10_table) / sizeof(exp10_table[0]))
#define EXP10_TABLE_MAX (1U << (EXP10_TABLE_SIZE - 1))
#define DIGITS_PER_BLOCK 9
#define lower_bnd 1e8
#define upper_bnd 1e9
#define power_table exp10_table
#define dpb DIGITS_PER_BLOCK
#define base 10
#define FLT_MANT_DIG 24
#define DBL_MANT_DIG 53
#define FPMAX_MANT_DIG DBL_MANT_DIG /* all treate as double */
#define DECIMAL_DIG (1 + (((FPMAX_MANT_DIG * 100) + 331) / 332)) /* 17.96 */
#define NUM_DIGIT_BLOCKS \
        ((DECIMAL_DIG + DIGITS_PER_BLOCK - 1) / DIGITS_PER_BLOCK)
#define ndb NUM_DIGIT_BLOCKS
#define nd DECIMAL_DIG
#define DBL_MAX_10_EXP 308
#define BUF_SIZE (DBL_MAX_10_EXP + 2)

static void clear_buffer(void)
{
    char *core_printbuf = HARTID * BUFLEN + printbuf;
    memset(core_printbuf, 0x0, BUFLEN);
}

static bool myisdigit(char c)
{
    return ('0' <= c) && (c <= '9');
}

static long mypow(long a, long b)
{
    while (b) {
        a = a * base;
        b--;
    }
    return a;
}

static double getRoundNum(double a, long b)
{
    long t1 = mypow(1, b);
    double tmp;
    tmp = (long)(a * t1 + 0.5) / (double)t1;
    return tmp;
}

static char *handle_floating_point(double num, int *expnum)
{
    int exp = DIGITS_PER_BLOCK - 1;
    int i = EXP10_TABLE_SIZE;
    int j = EXP10_TABLE_MAX;
    char buf[BUF_SIZE];
    char *s, *e;
    int round, o_exp;
    double x = num;
    {
        int exp_neg = 0;
        if (x < lower_bnd) {
            exp_neg = 1;
        }

        do {
            --i;
            if (exp_neg) {
                if (x * power_table[i] < upper_bnd) {
                    x *= power_table[i];
                    exp -= j;
                }
            } else {
                if (x / power_table[i] >= lower_bnd) {
                    x /= power_table[i];
                    exp += j;
                }
            }
            j >>= 1;
        } while (i);
    }

    if (x >= upper_bnd) {
        x /= power_table[0];
        ++exp;
    }

    {
        i = 0;
        s = buf;
        do {
            unsigned long digit_block = (unsigned long)x;
            x = (x - digit_block) * upper_bnd;
            s += dpb;
            j = 0;
            do {
                s[-(++j)] = '0' + (digit_block % base);
                digit_block /= base;
            } while (j < dpb);
        } while (++i < ndb);
    }
    s = buf;
    expnum[0] = exp;
    return s;
}

static void get_floating_point_str(char *str, char (*buf)[BUF_SIZE],
                                          int *exp)
{
    int i, j;
    if (exp[0] >= 0) {
        i = 0, j = 0;
        if (exp[0] <= DECIMAL_DIG - 1) {
            while (i <= exp[0]) {
                buf[0][i] = str[i];
                i++;
            }
            buf[0][i] = '\0';
            while (i <= DECIMAL_DIG - 1) {
                buf[1][j] = str[i];
                i++, j++;
            }
            buf[1][j] = '\0';
        } else {
            while (i <= DECIMAL_DIG - 1) {
                buf[0][i] = str[i];
                i++;
            }
            while (i <= exp[0]) {
                buf[0][i] = '0';
                i++;
            }
            buf[0][i] = '\0';
            buf[1][0] = '0';
            buf[1][1] = '\0';
        }
    } else {
        i = 0, j = 0;
        exp[1] = (-exp[0]) - 1;
        while (i <= exp[1] - 1) {
            buf[1][i] = '0';
            i++;
        }
        while (i <= DECIMAL_DIG - 1) {
            buf[1][i] = str[j];
            i++, j++;
        }
        buf[1][i] = '\0';
        buf[0][0] = '0';
        buf[0][1] = '\0';
    }
}

#define INFINITY (__builtin_inff())
static int my_isinf(double x)
{
    /* 判断是否为正无穷或负无穷 */
    if (x == INFINITY || x == -INFINITY) {
        return 1; /* 返回非零值表示是无穷大 */
    }
    return 0; /* 返回 0 表示不是无穷大 */
}

static int my_isnan(double x)
{
    /*
     * According to the IEEE 754 standard, the property of NaN is that it
     * is not equal to any value, including itself.
     */
    if (x != x) {
        return 1; /* 返回非零值表示是 NaN */
    }
    return 0; /* 返回 0 表示不是 NaN */
}

static char *my_strcpy(char *dest, const char *src)
{
    if (dest == NULL || src == NULL) {
        return NULL;
    }

    char *ptr = dest;
    while ((*ptr++ = *src++) != '\0') {
        /* nothing */
    };
    return dest;
}

static int my_vsnprintf(char *out, size_t n, const char *s, va_list vl)
{
    bool format = false;
    bool longarg = false;
    bool is_specified_prec = false;
    long whole_prec = 0;
    long frac_prec = 0;
    int tmp = 0;
    const char *p;
    for (; *s; s++) {
        if (format) {
            p = s;
            while (myisdigit(*s)) {
                if (tmp < INT_MAX / base ||
                    (tmp == INT_MAX / base && (*s - '0') <= INT_MAX % base)) {
                    tmp = (tmp * base) + (*s - '0');
                } else {
                    tmp = INT_MAX; /* best we can do... */
                }
                ++s;
            }
            if (p[-1] == '%' && p[0] != '.') {
                whole_prec = tmp;
                tmp = 0;
            }
            if (*s == '.') {
                p = ++s;
                while (myisdigit(*s)) {
                    if (tmp < INT_MAX / base || (tmp == INT_MAX / base &&
                        (*s - '0') <= INT_MAX % base)) {
                        tmp = (tmp * base) + (*s - '0');
                    } else {
                        tmp = INT_MAX; /* best we can do... */
                    }
                    ++s;
                }
            }
            if (p[-1] == '.') {

                frac_prec = tmp;
                tmp = 0;
                is_specified_prec = true;
            }
            switch (*s) {
            case 'l':
                longarg = true;
                break;
            case 'p':
                longarg = true;
                if (++printlen[HARTID] < n) {
                    out[printlen[HARTID] - 1] = '0';
                }
                if (++printlen[HARTID] < n) {
                    out[printlen[HARTID] - 1] = 'x';
                }
            case 'x':
            {
                long num = longarg ? va_arg(vl, long) : va_arg(vl, int);
                for (int i = 2 * (longarg ? sizeof(long) : sizeof(int)) - 1;
                     i >= 0; i--) {
                    int d = (num >> (4 * i)) & 0xF;
                    if (++printlen[HARTID] < n) {
                        out[printlen[HARTID] - 1] = (d < base ? '0' + d :
                                             'a' + d - base);
                    }
                }
                longarg = false;
                format = false;
                break;
            }
            case 'd':
            {
                long num = longarg ? va_arg(vl, long) : va_arg(vl, int);
                if (num < 0) {
                    num = -num;
                    if (++printlen[HARTID] < n) {
                        out[printlen[HARTID] - 1] = '-';
                    }
                }
                long digits = 1;
                for (long nn = num; nn /= base; digits++) {
                    /* nothing */
                };
                long width = GET_MAX(digits, whole_prec);
                for (int i = width - 1; i >= 0; i--) {
                    if (printlen[HARTID] + i + 1 < n) {
                        if (i > width - digits - 1) {
                            out[printlen[HARTID] + i] = '0' + (num % base);
                            num /= base;
                        } else {
                            out[printlen[HARTID] + i] = ' ';
                        }
                    }
                }
                printlen[HARTID] += width;
                longarg = false;
                format = false;
                break;
            }
            case 's':
            {
                const char *s2 = va_arg(vl, const char*);
                while (*s2) {
                    if (++printlen[HARTID] < n) {
                        out[printlen[HARTID] - 1] = *s2;
                    }
                    s2++;
                }
                longarg = false;
                format = false;
                break;
            }
            case 'c':
            {
                if (++printlen[HARTID] < n) {
                    out[printlen[HARTID] - 1] = (char)va_arg(vl, int);
                }
                longarg = false;
                format = false;
                break;
            }
            case 'f':
            {
                double num = va_arg(vl, double);

                int sign_flag = 0;
                if (num < 0) {
                    num = -num;
                    sign_flag = 1;
                }

                /* handle inf. */
                if (my_isinf(num)) {
                    int str_size = 3;
                    char inf_str[] = "fni";
                    if (sign_flag) {
                        if (++printlen[HARTID] < n) {
                            out[printlen[HARTID] - 1] = '-';
                        }
                    }
                    while (str_size > 0) {
                        if (++printlen[HARTID] < n) {
                            out[printlen[HARTID] - 1] = inf_str[--str_size];
                        }
                    }
                    longarg = false;
                    format = false;
                    break;
                }
                /* handle nan. */
                if (my_isnan(num)) {
                    int str_size = 3;
                    char nan_str[] = "nan";
                    while (str_size > 0) {
                        if (++printlen[HARTID] < n) {
                            out[printlen[HARTID] - 1] = nan_str[--str_size];
                        }
                    }
                    longarg = false;
                    format = false;
                    break;
                }

                char str[BUF_SIZE];
                /* buf[0] : integer part, buf[1] : fractional part. */
                char buf[2][BUF_SIZE] = {
                    {0}, {0}
                };
                int exp[2];
                /* get whole string. */
                my_strcpy(str, handle_floating_point(num, exp));
                /* spilt the string to integer part and fractional part. */
                get_floating_point_str(str, buf, exp);

                /* handle integer part. */
                char whole_str[BUF_SIZE] = {0};
                int whole_pos = 0;
                int whole_size = 0;
                while (buf[0][++whole_size] != '\0') {
                    /* nothing */
                };
                while (whole_size > 0) {
                    whole_str[whole_pos++] = buf[0][--whole_size];
                }
                if (sign_flag) {
                    whole_str[whole_pos++] = '-';
                }
                long width = GET_MAX(whole_pos, whole_prec);
                while (width > whole_pos) {
                    whole_str[whole_pos++] = ' ';
                }
                while (whole_pos > 0) {
                    if (++printlen[HARTID] < n) {
                        out[printlen[HARTID] - 1] = whole_str[--whole_pos];
                    }
                }

                /* handle fractional part. */
                long prec;
                if (is_specified_prec) {
                    if (frac_prec > (long)DECIMAL_DIG + 1) {
                        prec = (long)DECIMAL_DIG + 1;
                    } else {
                        prec = frac_prec;
                    }
                } else {
                    prec = DEFAULT_PREC;
                }
                int frac_size = 0;
                long frac_part = 0;
                while (buf[1][frac_size] != '\0') {
                    frac_part = frac_part * base + (buf[1][frac_size] - '0');
                    frac_size++;
                }
                double frac = (double)frac_part;
                while (frac_size-- != 0) {
                    frac /= (double)base;
                }

                frac_part = (long)(getRoundNum(frac, prec) * mypow(1, prec));
                if (++printlen[HARTID] < n) {
                    out[printlen[HARTID] - 1] = '.';
                }
                char frac_str[BUF_SIZE] = {0};
                int frac_pos = 0;
                while (prec < frac_prec) {
                    frac_str[frac_pos++] = '0';
                    prec++;
                }
                while (frac_part > 0) {
                    frac_str[frac_pos++] = '0' + frac_part % base;
                    frac_part /= base;
                }
                /* Handle zeros at the beginning of fractional parts. */
                while (frac_pos < prec) {
                    frac_str[frac_pos++] = '0';
                }
                while (frac_pos > 0) {
                    if (++printlen[HARTID] < n) {
                        out[printlen[HARTID] - 1] = frac_str[--frac_pos];
                    }
                }
                is_specified_prec = false;
                longarg = false;
                format = false;
                break;
            }
            default:
                break;
            }
        } else if (*s == '%') {
            format = true;
        } else if (++printlen[HARTID] < n) {
            out[printlen[HARTID] - 1] = *s;
        }
    }
    if (printlen[HARTID] < n) {
        out[printlen[HARTID]] = 0;
    } else if (n) {
        out[n - 1] = 0;
    }

    return printlen[HARTID];
}

static inline uint32_t io_read32(uint64_t addr)
{
    return *((volatile uint32_t *)(uintptr_t)addr); /* read register */
}

static inline void io_write32(uint64_t addr, uint32_t val)
{
    *((volatile uint32_t *)(uintptr_t)addr) = val; /* write register */
}

static void pl011_putc(int ch)
{
    uint32_t uart_base = PL011_IO_BASE;

    /* Wait until there is space in the FIFO or device is disabled */
    while (io_read32(uart_base + UART_FR) & UART_FR_TXFF) {
        /* nothing */
    };

    /* Send the character */
    io_write32(uart_base + UART_DR, ch);
}

int printf(const char *s, ...)
{
    va_list vl;
    va_start(vl, s);
    char *core_printbuf = HARTID * BUFLEN + printbuf;
    int res = my_vsnprintf(core_printbuf, BUFLEN, s, vl);
    va_end(vl);

    for (char *ch = core_printbuf; *ch; ch++) {
        pl011_putc(*ch);
    }

    memset(core_printbuf, 0, res);
    printlen[HARTID] = 0;
    return res;
}
