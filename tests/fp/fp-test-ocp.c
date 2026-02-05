/*
 * fp-test-ocp.c - test QEMU's softfloat OCP FP8/FP4 and BF16 implementation
 *
 * Copyright (C) 2026, QEMU Contributors
 *
 * License: GNU GPL, version 2 or later.
 *   See the COPYING file in the top-level directory.
 *
 * This file tests the OCP (Open Compute Project) floating-point formats:
 *   - float8_e4m3: 8-bit FP with 4-bit exponent, 3-bit mantissa (no infinity)
 *   - float8_e5m2: 8-bit FP with 5-bit exponent, 2-bit mantissa (IEEE-like)
 *   - float4_e2m1: 4-bit FP with 2-bit exponent, 1-bit mantissa (no NaN/Inf)
 *   - bfloat16: Brain floating-point format
 */
#ifndef HW_POISON_H
#error Must define HW_POISON_H to work around TARGET_* poisoning
#endif

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include <math.h>
#include "fpu/softfloat.h"

static int errors;
static int tests_run;
static int tests_passed;
static bool verbose;

#define TEST_ASSERT(cond, fmt, ...) do { \
    tests_run++; \
    if (!(cond)) { \
        printf("FAIL: " fmt "\n", ##__VA_ARGS__); \
        errors++; \
        if (errors >= 20) { \
            printf("Too many errors, aborting.\n"); \
            exit(1); \
        } \
    } else { \
        tests_passed++; \
        if (verbose) { \
            printf("PASS: " fmt "\n", ##__VA_ARGS__); \
        } \
    } \
} while (0)

static float_status qsf;

static void init_status(void)
{
    memset(&qsf, 0, sizeof(qsf));
    set_float_2nan_prop_rule(float_2nan_prop_s_ab, &qsf);
    set_float_default_nan_pattern(0b01000000, &qsf);
    set_float_rounding_mode(float_round_nearest_even, &qsf);
}

/*
 * ============================================================================
 * E4M3 Format Tests (OCP FP8 with 4-bit exponent, 3-bit mantissa)
 *
 * E4M3 characteristics:
 *   - Exponent: 4 bits, bias = 7
 *   - Mantissa: 3 bits
 *   - No infinity representation
 *   - NaN: 0x7f (positive) or 0xff (negative)
 *   - Max normal: 0x7e = 448.0
 *   - Min normal: 0x08 = 2^-6
 *   - Min subnormal: 0x01 = 2^-9
 * ============================================================================
 */

/* E4M3 special value encodings */
#define E4M3_ZERO_POS       0x00
#define E4M3_ZERO_NEG       0x80
#define E4M3_NAN_POS        0x7f
#define E4M3_NAN_NEG        0xff
#define E4M3_MAX_NORMAL     0x7e  /* 448.0 */
#define E4M3_MIN_NORMAL     0x08  /* 2^-6 = 0.015625 */
#define E4M3_MIN_SUBNORMAL  0x01  /* 2^-9 = 0.001953125 */
#define E4M3_ONE            0x38  /* 1.0 */
#define E4M3_TWO            0x40  /* 2.0 */

static void test_e4m3_classification(void)
{
    printf("Testing E4M3 classification functions...\n");

    /* Zero tests */
    TEST_ASSERT(float8_e4m3_is_zero(E4M3_ZERO_POS),
                "E4M3 +0 should be zero");
    TEST_ASSERT(float8_e4m3_is_zero(E4M3_ZERO_NEG),
                "E4M3 -0 should be zero");
    TEST_ASSERT(!float8_e4m3_is_zero(E4M3_ONE),
                "E4M3 1.0 should not be zero");

    /* NaN tests */
    TEST_ASSERT(float8_e4m3_is_any_nan(E4M3_NAN_POS),
                "E4M3 0x7f should be NaN");
    TEST_ASSERT(float8_e4m3_is_any_nan(E4M3_NAN_NEG),
                "E4M3 0xff should be NaN");
    TEST_ASSERT(!float8_e4m3_is_any_nan(E4M3_MAX_NORMAL),
                "E4M3 max normal should not be NaN");

    /* Infinity tests (E4M3 has no infinity) */
    TEST_ASSERT(!float8_e4m3_is_infinity(E4M3_NAN_POS),
                "E4M3 has no infinity");
    TEST_ASSERT(!float8_e4m3_is_infinity(E4M3_MAX_NORMAL),
                "E4M3 max normal is not infinity");

    /* Normal tests */
    TEST_ASSERT(float8_e4m3_is_normal(E4M3_ONE),
                "E4M3 1.0 should be normal");
    TEST_ASSERT(float8_e4m3_is_normal(E4M3_MAX_NORMAL),
                "E4M3 max normal should be normal");
    TEST_ASSERT(float8_e4m3_is_normal(E4M3_MIN_NORMAL),
                "E4M3 min normal should be normal");
    TEST_ASSERT(!float8_e4m3_is_normal(E4M3_MIN_SUBNORMAL),
                "E4M3 min subnormal should not be normal");

    /* Subnormal tests */
    TEST_ASSERT(float8_e4m3_is_zero_or_denormal(E4M3_ZERO_POS),
                "E4M3 zero should be zero_or_denormal");
    TEST_ASSERT(float8_e4m3_is_zero_or_denormal(E4M3_MIN_SUBNORMAL),
                "E4M3 min subnormal should be zero_or_denormal");
    TEST_ASSERT(!float8_e4m3_is_zero_or_denormal(E4M3_MIN_NORMAL),
                "E4M3 min normal should not be zero_or_denormal");

    /* Sign tests */
    TEST_ASSERT(!float8_e4m3_is_neg(E4M3_ONE),
                "E4M3 +1.0 should not be negative");
    TEST_ASSERT(float8_e4m3_is_neg(E4M3_ZERO_NEG),
                "E4M3 -0 should be negative");
    TEST_ASSERT(float8_e4m3_is_neg(E4M3_NAN_NEG),
                "E4M3 -NaN should be negative");
}

/*
 * ============================================================================
 * E5M2 Format Tests (OCP FP8 with 5-bit exponent, 2-bit mantissa)
 *
 * E5M2 characteristics:
 *   - Exponent: 5 bits, bias = 15
 *   - Mantissa: 2 bits
 *   - Has infinity (0x7c positive, 0xfc negative)
 *   - NaN: exp=31, frac!=0 (0x7d, 0x7e, 0x7f, etc.)
 *   - Max normal: 0x7b = 57344.0
 *   - Min normal: 0x04 = 2^-14
 * ============================================================================
 */

#define E5M2_ZERO_POS       0x00
#define E5M2_ZERO_NEG       0x80
#define E5M2_INF_POS        0x7c
#define E5M2_INF_NEG        0xfc
#define E5M2_NAN            0x7f  /* Canonical NaN */
#define E5M2_MAX_NORMAL     0x7b  /* 57344.0 */
#define E5M2_MIN_NORMAL     0x04  /* 2^-14 */
#define E5M2_ONE            0x3c  /* 1.0 */

static void test_e5m2_classification(void)
{
    printf("Testing E5M2 classification functions...\n");

    /* Zero tests */
    TEST_ASSERT(float8_e5m2_is_zero(E5M2_ZERO_POS),
                "E5M2 +0 should be zero");
    TEST_ASSERT(float8_e5m2_is_zero(E5M2_ZERO_NEG),
                "E5M2 -0 should be zero");

    /* Infinity tests */
    TEST_ASSERT(float8_e5m2_is_infinity(E5M2_INF_POS),
                "E5M2 0x7c should be +infinity");
    TEST_ASSERT(float8_e5m2_is_infinity(E5M2_INF_NEG),
                "E5M2 0xfc should be -infinity");
    TEST_ASSERT(!float8_e5m2_is_infinity(E5M2_MAX_NORMAL),
                "E5M2 max normal should not be infinity");

    /* NaN tests */
    TEST_ASSERT(float8_e5m2_is_any_nan(E5M2_NAN),
                "E5M2 0x7f should be NaN");
    TEST_ASSERT(float8_e5m2_is_any_nan(0x7d),
                "E5M2 0x7d should be NaN");
    TEST_ASSERT(float8_e5m2_is_any_nan(0x7e),
                "E5M2 0x7e should be NaN");
    TEST_ASSERT(!float8_e5m2_is_any_nan(E5M2_INF_POS),
                "E5M2 infinity should not be NaN");

    /* Normal tests */
    TEST_ASSERT(float8_e5m2_is_normal(E5M2_ONE),
                "E5M2 1.0 should be normal");
    TEST_ASSERT(float8_e5m2_is_normal(E5M2_MAX_NORMAL),
                "E5M2 max normal should be normal");
    TEST_ASSERT(float8_e5m2_is_normal(E5M2_MIN_NORMAL),
                "E5M2 min normal should be normal");

    /* Sign tests */
    TEST_ASSERT(!float8_e5m2_is_neg(E5M2_ONE),
                "E5M2 +1.0 should not be negative");
    TEST_ASSERT(float8_e5m2_is_neg(E5M2_ZERO_NEG),
                "E5M2 -0 should be negative");
    TEST_ASSERT(float8_e5m2_is_neg(E5M2_INF_NEG),
                "E5M2 -inf should be negative");
}

/*
 * ============================================================================
 * E2M1 Format Tests (OCP FP4 with 2-bit exponent, 1-bit mantissa)
 *
 * E2M1 characteristics:
 *   - Exponent: 2 bits, bias = 1
 *   - Mantissa: 1 bit
 *   - No infinity, no NaN
 *   - All 16 encodings (4 bits) are valid numbers
 *   - Max value: 0x7 = 6.0
 *   - Min normal: 0x2 = 1.0
 *   - Min subnormal: 0x1 = 0.5
 * ============================================================================
 */

#define E2M1_ZERO_POS       0x0
#define E2M1_ZERO_NEG       0x8
#define E2M1_ONE            0x2  /* 1.0 */
#define E2M1_MAX            0x7  /* 6.0 */
#define E2M1_MIN_SUBNORMAL  0x1  /* 0.5 */

static void test_e2m1_classification(void)
{
    printf("Testing E2M1 classification functions...\n");

    /* Zero tests */
    TEST_ASSERT(float4_e2m1_is_zero(E2M1_ZERO_POS),
                "E2M1 +0 should be zero");
    TEST_ASSERT(float4_e2m1_is_zero(E2M1_ZERO_NEG),
                "E2M1 -0 should be zero");
    TEST_ASSERT(!float4_e2m1_is_zero(E2M1_ONE),
                "E2M1 1.0 should not be zero");

    /* No NaN in E2M1 */
    TEST_ASSERT(!float4_e2m1_is_any_nan(E2M1_MAX),
                "E2M1 has no NaN");
    TEST_ASSERT(!float4_e2m1_is_any_nan(E2M1_ZERO_POS),
                "E2M1 zero is not NaN");

    /* No infinity in E2M1 */
    TEST_ASSERT(!float4_e2m1_is_infinity(E2M1_MAX),
                "E2M1 has no infinity");

    /* Normal tests */
    TEST_ASSERT(float4_e2m1_is_normal(E2M1_ONE),
                "E2M1 1.0 should be normal");
    TEST_ASSERT(float4_e2m1_is_normal(E2M1_MAX),
                "E2M1 max should be normal");
    TEST_ASSERT(!float4_e2m1_is_normal(E2M1_MIN_SUBNORMAL),
                "E2M1 min subnormal should not be normal");

    /* Sign tests */
    TEST_ASSERT(!float4_e2m1_is_neg(E2M1_ONE),
                "E2M1 +1.0 should not be negative");
    TEST_ASSERT(float4_e2m1_is_neg(E2M1_ZERO_NEG),
                "E2M1 -0 should be negative");
}

/*
 * ============================================================================
 * BFloat16 Classification Tests
 * ============================================================================
 */

#define BF16_ZERO_POS       0x0000
#define BF16_ZERO_NEG       0x8000
#define BF16_INF_POS        0x7f80
#define BF16_INF_NEG        0xff80
#define BF16_NAN            0x7fc0  /* Quiet NaN */
#define BF16_ONE            0x3f80  /* 1.0 */
#define BF16_TWO            0x4000  /* 2.0 */

static void test_bfloat16_classification(void)
{
    printf("Testing BFloat16 classification functions...\n");

    TEST_ASSERT(bfloat16_is_zero(BF16_ZERO_POS),
                "BF16 +0 should be zero");
    TEST_ASSERT(bfloat16_is_zero(BF16_ZERO_NEG),
                "BF16 -0 should be zero");
    TEST_ASSERT(bfloat16_is_infinity(BF16_INF_POS),
                "BF16 0x7f80 should be +infinity");
    TEST_ASSERT(bfloat16_is_infinity(BF16_INF_NEG),
                "BF16 0xff80 should be -infinity");
    TEST_ASSERT(bfloat16_is_any_nan(BF16_NAN),
                "BF16 0x7fc0 should be NaN");
    TEST_ASSERT(!bfloat16_is_any_nan(BF16_ONE),
                "BF16 1.0 should not be NaN");
}

/*
 * ============================================================================
 * Conversion Tests: E4M3 <-> BF16
 * ============================================================================
 */

static void test_e4m3_bf16_conversion(void)
{
    bfloat16 bf;
    float8_e4m3 e4;

    printf("Testing E4M3 <-> BF16 conversions...\n");
    init_status();

    /* E4M3 -> BF16: Zero */
    bf = float8_e4m3_to_bfloat16(E4M3_ZERO_POS, &qsf);
    TEST_ASSERT(bfloat16_is_zero(bf) && !bfloat16_is_neg(bf),
                "E4M3 +0 -> BF16 +0");

    bf = float8_e4m3_to_bfloat16(E4M3_ZERO_NEG, &qsf);
    TEST_ASSERT(bfloat16_is_zero(bf) && bfloat16_is_neg(bf),
                "E4M3 -0 -> BF16 -0");

    /* E4M3 -> BF16: Normal values */
    bf = float8_e4m3_to_bfloat16(E4M3_ONE, &qsf);
    TEST_ASSERT(bf == BF16_ONE,
                "E4M3 1.0 -> BF16 1.0 (got 0x%04x, expected 0x%04x)",
                bf, BF16_ONE);

    bf = float8_e4m3_to_bfloat16(E4M3_TWO, &qsf);
    TEST_ASSERT(bf == BF16_TWO,
                "E4M3 2.0 -> BF16 2.0 (got 0x%04x, expected 0x%04x)",
                bf, BF16_TWO);

    /* E4M3 -> BF16: NaN */
    bf = float8_e4m3_to_bfloat16(E4M3_NAN_POS, &qsf);
    TEST_ASSERT(bfloat16_is_any_nan(bf),
                "E4M3 NaN -> BF16 NaN");

    /* BF16 -> E4M3: Zero */
    e4 = bfloat16_to_float8_e4m3(BF16_ZERO_POS, false, &qsf);
    TEST_ASSERT(float8_e4m3_is_zero(e4) && !float8_e4m3_is_neg(e4),
                "BF16 +0 -> E4M3 +0");

    /* BF16 -> E4M3: Normal values */
    e4 = bfloat16_to_float8_e4m3(BF16_ONE, false, &qsf);
    TEST_ASSERT(e4 == E4M3_ONE,
                "BF16 1.0 -> E4M3 1.0 (got 0x%02x, expected 0x%02x)",
                e4, E4M3_ONE);

    /* BF16 -> E4M3: Infinity -> NaN (E4M3 has no infinity) */
    e4 = bfloat16_to_float8_e4m3(BF16_INF_POS, false, &qsf);
    TEST_ASSERT(float8_e4m3_is_any_nan(e4),
                "BF16 +inf -> E4M3 NaN (no saturation)");

    /* BF16 -> E4M3: Infinity with saturation -> max normal */
    e4 = bfloat16_to_float8_e4m3(BF16_INF_POS, true, &qsf);
    TEST_ASSERT(e4 == E4M3_MAX_NORMAL,
                "BF16 +inf -> E4M3 max normal (with saturation), got 0x%02x",
                e4);

    /*
     * BF16 -> E4M3: NaN conversion
     * E4M3 has limited NaN encoding (only 0x7f/0xff are NaN).
     * For proper NaN handling, we need to configure default_nan_mode.
     */
    {
        float_status nan_status = qsf;
        nan_status.default_nan_pattern = 0x70;  /* Produces 0x7f for E4M3 */
        nan_status.default_nan_mode = true;
        e4 = bfloat16_to_float8_e4m3(BF16_NAN, false, &nan_status);
        TEST_ASSERT(float8_e4m3_is_any_nan(e4),
                    "BF16 NaN -> E4M3 NaN with default_nan_mode (got 0x%02x)", e4);
    }
}

/*
 * ============================================================================
 * Conversion Tests: E5M2 <-> BF16
 * ============================================================================
 */

static void test_e5m2_bf16_conversion(void)
{
    bfloat16 bf;
    float8_e5m2 e5;

    printf("Testing E5M2 <-> BF16 conversions...\n");
    init_status();

    /* E5M2 -> BF16: Zero */
    bf = float8_e5m2_to_bfloat16(E5M2_ZERO_POS, &qsf);
    TEST_ASSERT(bfloat16_is_zero(bf) && !bfloat16_is_neg(bf),
                "E5M2 +0 -> BF16 +0");

    /* E5M2 -> BF16: Normal values */
    bf = float8_e5m2_to_bfloat16(E5M2_ONE, &qsf);
    TEST_ASSERT(bf == BF16_ONE,
                "E5M2 1.0 -> BF16 1.0 (got 0x%04x, expected 0x%04x)",
                bf, BF16_ONE);

    /* E5M2 -> BF16: Infinity */
    bf = float8_e5m2_to_bfloat16(E5M2_INF_POS, &qsf);
    TEST_ASSERT(bfloat16_is_infinity(bf) && !bfloat16_is_neg(bf),
                "E5M2 +inf -> BF16 +inf");

    bf = float8_e5m2_to_bfloat16(E5M2_INF_NEG, &qsf);
    TEST_ASSERT(bfloat16_is_infinity(bf) && bfloat16_is_neg(bf),
                "E5M2 -inf -> BF16 -inf");

    /* E5M2 -> BF16: NaN */
    bf = float8_e5m2_to_bfloat16(E5M2_NAN, &qsf);
    TEST_ASSERT(bfloat16_is_any_nan(bf),
                "E5M2 NaN -> BF16 NaN");

    /* BF16 -> E5M2: Zero */
    e5 = bfloat16_to_float8_e5m2(BF16_ZERO_POS, false, &qsf);
    TEST_ASSERT(float8_e5m2_is_zero(e5) && !float8_e5m2_is_neg(e5),
                "BF16 +0 -> E5M2 +0");

    /* BF16 -> E5M2: Normal values */
    e5 = bfloat16_to_float8_e5m2(BF16_ONE, false, &qsf);
    TEST_ASSERT(e5 == E5M2_ONE,
                "BF16 1.0 -> E5M2 1.0 (got 0x%02x, expected 0x%02x)",
                e5, E5M2_ONE);

    /* BF16 -> E5M2: Infinity */
    e5 = bfloat16_to_float8_e5m2(BF16_INF_POS, false, &qsf);
    TEST_ASSERT(float8_e5m2_is_infinity(e5) && !float8_e5m2_is_neg(e5),
                "BF16 +inf -> E5M2 +inf");

    /* BF16 -> E5M2: Infinity with saturation -> max normal */
    e5 = bfloat16_to_float8_e5m2(BF16_INF_POS, true, &qsf);
    TEST_ASSERT(e5 == E5M2_MAX_NORMAL,
                "BF16 +inf -> E5M2 max normal (with saturation), got 0x%02x",
                e5);

    /* BF16 -> E5M2: NaN */
    e5 = bfloat16_to_float8_e5m2(BF16_NAN, false, &qsf);
    TEST_ASSERT(float8_e5m2_is_any_nan(e5),
                "BF16 NaN -> E5M2 NaN");
}

/*
 * ============================================================================
 * Conversion Tests: F32 -> E4M3/E5M2
 * ============================================================================
 */

/* Float32 bit patterns */
#define F32_ZERO_POS        0x00000000
#define F32_ONE             0x3f800000  /* 1.0 */
#define F32_TWO             0x40000000  /* 2.0 */
#define F32_INF_POS         0x7f800000
#define F32_NAN             0x7fc00000

static void test_f32_to_fp8_conversion(void)
{
    float8_e4m3 e4;
    float8_e5m2 e5;
    float32 f32;

    printf("Testing F32 -> FP8 conversions...\n");
    init_status();

    /* F32 -> E4M3: Zero */
    f32 = F32_ZERO_POS;
    e4 = float32_to_float8_e4m3(f32, false, &qsf);
    TEST_ASSERT(float8_e4m3_is_zero(e4),
                "F32 +0 -> E4M3 +0");

    /* F32 -> E4M3: Normal values */
    f32 = F32_ONE;
    e4 = float32_to_float8_e4m3(f32, false, &qsf);
    TEST_ASSERT(e4 == E4M3_ONE,
                "F32 1.0 -> E4M3 1.0 (got 0x%02x, expected 0x%02x)",
                e4, E4M3_ONE);

    f32 = F32_TWO;
    e4 = float32_to_float8_e4m3(f32, false, &qsf);
    TEST_ASSERT(e4 == E4M3_TWO,
                "F32 2.0 -> E4M3 2.0 (got 0x%02x, expected 0x%02x)",
                e4, E4M3_TWO);

    /* F32 -> E4M3: Infinity -> NaN (no saturation) */
    f32 = F32_INF_POS;
    e4 = float32_to_float8_e4m3(f32, false, &qsf);
    TEST_ASSERT(float8_e4m3_is_any_nan(e4),
                "F32 +inf -> E4M3 NaN (no saturation)");

    /* F32 -> E4M3: Infinity with saturation -> max normal */
    e4 = float32_to_float8_e4m3(f32, true, &qsf);
    TEST_ASSERT(e4 == E4M3_MAX_NORMAL,
                "F32 +inf -> E4M3 max normal (with saturation), got 0x%02x",
                e4);

    /* F32 -> E5M2: Zero */
    f32 = F32_ZERO_POS;
    e5 = float32_to_float8_e5m2(f32, false, &qsf);
    TEST_ASSERT(float8_e5m2_is_zero(e5),
                "F32 +0 -> E5M2 +0");

    /* F32 -> E5M2: Normal values */
    f32 = F32_ONE;
    e5 = float32_to_float8_e5m2(f32, false, &qsf);
    TEST_ASSERT(e5 == E5M2_ONE,
                "F32 1.0 -> E5M2 1.0 (got 0x%02x, expected 0x%02x)",
                e5, E5M2_ONE);

    /* F32 -> E5M2: Infinity */
    f32 = F32_INF_POS;
    e5 = float32_to_float8_e5m2(f32, false, &qsf);
    TEST_ASSERT(float8_e5m2_is_infinity(e5),
                "F32 +inf -> E5M2 +inf");

    /* F32 -> E5M2: Infinity with saturation -> max normal */
    e5 = float32_to_float8_e5m2(f32, true, &qsf);
    TEST_ASSERT(e5 == E5M2_MAX_NORMAL,
                "F32 +inf -> E5M2 max normal (with saturation), got 0x%02x",
                e5);
}

/*
 * ============================================================================
 * Conversion Tests: E2M1 -> E4M3
 * ============================================================================
 */

static void test_e2m1_to_e4m3_conversion(void)
{
    float8_e4m3 e4;

    printf("Testing E2M1 -> E4M3 conversions...\n");
    init_status();

    /* E2M1 -> E4M3: Zero */
    e4 = float4_e2m1_to_float8_e4m3(E2M1_ZERO_POS, &qsf);
    TEST_ASSERT(float8_e4m3_is_zero(e4) && !float8_e4m3_is_neg(e4),
                "E2M1 +0 -> E4M3 +0");

    e4 = float4_e2m1_to_float8_e4m3(E2M1_ZERO_NEG, &qsf);
    TEST_ASSERT(float8_e4m3_is_zero(e4) && float8_e4m3_is_neg(e4),
                "E2M1 -0 -> E4M3 -0");

    /* E2M1 -> E4M3: Normal values */
    e4 = float4_e2m1_to_float8_e4m3(E2M1_ONE, &qsf);
    TEST_ASSERT(e4 == E4M3_ONE,
                "E2M1 1.0 -> E4M3 1.0 (got 0x%02x, expected 0x%02x)",
                e4, E4M3_ONE);

    /* E2M1 -> E4M3: Max value (6.0) */
    e4 = float4_e2m1_to_float8_e4m3(E2M1_MAX, &qsf);
    /* E2M1 max = 6.0 = 1.5 * 2^2, in E4M3: exp=9 (bias 7), frac=4 -> 0x4c */
    TEST_ASSERT(!float8_e4m3_is_any_nan(e4) && !float8_e4m3_is_zero(e4),
                "E2M1 max (6.0) -> E4M3 normal value");
}

/*
 * ============================================================================
 * BFloat16 <-> Float32 Conversion Tests
 * ============================================================================
 */

static void test_bf16_f32_conversion(void)
{
    bfloat16 bf;
    float32 f32;

    printf("Testing BF16 <-> F32 conversions...\n");
    init_status();

    /* BF16 -> F32: Zero */
    f32 = bfloat16_to_float32(BF16_ZERO_POS, &qsf);
    TEST_ASSERT(f32 == F32_ZERO_POS,
                "BF16 +0 -> F32 +0");

    /* BF16 -> F32: Normal values */
    f32 = bfloat16_to_float32(BF16_ONE, &qsf);
    TEST_ASSERT(f32 == F32_ONE,
                "BF16 1.0 -> F32 1.0 (got 0x%08x, expected 0x%08x)",
                f32, F32_ONE);

    /* BF16 -> F32: Infinity */
    f32 = bfloat16_to_float32(BF16_INF_POS, &qsf);
    TEST_ASSERT(f32 == F32_INF_POS,
                "BF16 +inf -> F32 +inf");

    /* BF16 -> F32: NaN */
    f32 = bfloat16_to_float32(BF16_NAN, &qsf);
    TEST_ASSERT(float32_is_any_nan(f32),
                "BF16 NaN -> F32 NaN");

    /* F32 -> BF16: Zero */
    bf = float32_to_bfloat16(F32_ZERO_POS, &qsf);
    TEST_ASSERT(bf == BF16_ZERO_POS,
                "F32 +0 -> BF16 +0");

    /* F32 -> BF16: Normal values */
    bf = float32_to_bfloat16(F32_ONE, &qsf);
    TEST_ASSERT(bf == BF16_ONE,
                "F32 1.0 -> BF16 1.0 (got 0x%04x, expected 0x%04x)",
                bf, BF16_ONE);

    /* F32 -> BF16: Infinity */
    bf = float32_to_bfloat16(F32_INF_POS, &qsf);
    TEST_ASSERT(bf == BF16_INF_POS,
                "F32 +inf -> BF16 +inf");

    /* F32 -> BF16: NaN */
    bf = float32_to_bfloat16(F32_NAN, &qsf);
    TEST_ASSERT(bfloat16_is_any_nan(bf),
                "F32 NaN -> BF16 NaN");
}

/*
 * ============================================================================
 * Rounding Mode Tests
 * ============================================================================
 */

static void test_rounding_modes(void)
{
    float8_e4m3 e4;
    float32 f32;

    printf("Testing rounding modes for FP8 conversions...\n");

    /* Test value that requires rounding: 1.5 in F32 = 0x3fc00000 */
    f32 = 0x3fc00000;  /* 1.5 */

    /* Round to nearest even */
    init_status();
    set_float_rounding_mode(float_round_nearest_even, &qsf);
    e4 = float32_to_float8_e4m3(f32, false, &qsf);
    TEST_ASSERT(!float8_e4m3_is_any_nan(e4),
                "F32 1.5 -> E4M3 with round_nearest_even");

    /* Round toward zero */
    init_status();
    set_float_rounding_mode(float_round_to_zero, &qsf);
    e4 = float32_to_float8_e4m3(f32, false, &qsf);
    TEST_ASSERT(!float8_e4m3_is_any_nan(e4),
                "F32 1.5 -> E4M3 with round_to_zero");

    /* Round up */
    init_status();
    set_float_rounding_mode(float_round_up, &qsf);
    e4 = float32_to_float8_e4m3(f32, false, &qsf);
    TEST_ASSERT(!float8_e4m3_is_any_nan(e4),
                "F32 1.5 -> E4M3 with round_up");

    /* Round down */
    init_status();
    set_float_rounding_mode(float_round_down, &qsf);
    e4 = float32_to_float8_e4m3(f32, false, &qsf);
    TEST_ASSERT(!float8_e4m3_is_any_nan(e4),
                "F32 1.5 -> E4M3 with round_down");
}

/*
 * ============================================================================
 * Canonical NaN Tests (per Zvfofp8min spec: canonical NaN = 0x7f)
 * ============================================================================
 */

static void test_canonical_nan(void)
{
    float8_e4m3 e4;
    float8_e5m2 e5;
    float_status local;

    printf("Testing canonical NaN generation...\n");

    /* Setup for RISC-V Zvfofp8min canonical NaN (0x7f) */
    memset(&local, 0, sizeof(local));
    set_float_2nan_prop_rule(float_2nan_prop_s_ab, &local);
    local.default_nan_pattern = 0x70;  /* Produces 0x7f for both E4M3 and E5M2 */
    local.default_nan_mode = true;
    set_float_rounding_mode(float_round_nearest_even, &local);

    /* E4M3: Infinity -> canonical NaN 0x7f */
    e4 = bfloat16_to_float8_e4m3(BF16_INF_POS, false, &local);
    TEST_ASSERT(e4 == 0x7f,
                "E4M3 canonical NaN should be 0x7f (got 0x%02x)", e4);

    /* E5M2: NaN input -> canonical NaN 0x7f */
    e5 = bfloat16_to_float8_e5m2(BF16_NAN, false, &local);
    TEST_ASSERT(e5 == 0x7f,
                "E5M2 canonical NaN should be 0x7f (got 0x%02x)", e5);
}

/*
 * ============================================================================
 * Main
 * ============================================================================
 */

static void print_usage(const char *prog)
{
    printf("Usage: %s [-v] [-h]\n", prog);
    printf("  -v  Verbose output (show passed tests)\n");
    printf("  -h  Show this help\n");
}

int main(int argc, char *argv[])
{
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    printf("=== QEMU OCP FP8/FP4 and BFloat16 Test Suite ===\n\n");

    /* Classification tests */
    test_e4m3_classification();
    test_e5m2_classification();
    test_e2m1_classification();
    test_bfloat16_classification();

    /* Conversion tests */
    test_e4m3_bf16_conversion();
    test_e5m2_bf16_conversion();
    test_f32_to_fp8_conversion();
    test_e2m1_to_e4m3_conversion();
    test_bf16_f32_conversion();

    /* Rounding and special value tests */
    test_rounding_modes();
    test_canonical_nan();

    printf("\n=== Test Summary ===\n");
    printf("Tests run: %d\n", tests_run);
    printf("Tests passed: %d\n", tests_passed);
    printf("Tests failed: %d\n", errors);

    return errors > 0 ? 1 : 0;
}
