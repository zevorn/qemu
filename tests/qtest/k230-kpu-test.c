/*
 * QTest testcase for K230 KPU/GNNE block
 *
 * Copyright (c) 2026 Chao Liu <chao.liu.zevorn@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define K230_FAKE_KPU_OUTPUT_BASE 0x10090000
#define K230_FAKE_KPU_OUTPUT_SIZE 0x00100000
#define K230_KPU_CFG_BASE         0x80400000
#define K230_PLIC_BASE            0xf00000000ULL

#define K230_PLIC_ENABLE_S        0x2080
#define K230_PLIC_CONTEXT_S       0x201000
#define K230_PLIC_THRESHOLD       0x00
#define K230_PLIC_CLAIM           0x04

#define K230_GNNE_COMMAND_START   0x100
#define K230_GNNE_COMMAND_END     0x104
#define K230_GNNE_COMMAND_HI      0x108
#define K230_GNNE_CLEAR           0x128
#define K230_GNNE_STATUS          0x130
#define K230_GNNE_START           0x190

#define K230_GNNE_COMMAND_TEST    0x01000000
#define K230_GNNE_DONE            0x0000000400000004ULL
#define K230_GNNE_DELAY_NS        (100 * 1000)
#define K230_GNNE_IRQ             189
#define K230_KPU_PAGE_SIZE        4096

#define K230_KPU_OUTPUT_END \
    (K230_FAKE_KPU_OUTPUT_BASE + K230_FAKE_KPU_OUTPUT_SIZE)
#define K230_KPU_OUTPUT_TEST0     K230_FAKE_KPU_OUTPUT_BASE
#define K230_KPU_OUTPUT_TEST1     (K230_FAKE_KPU_OUTPUT_BASE + 0x3000)
#define K230_KPU_OUTPUT_LAST \
    (K230_KPU_OUTPUT_END - K230_KPU_PAGE_SIZE)
#define K230_KPU_OUTPUT_UNREFERENCED \
    (K230_FAKE_KPU_OUTPUT_BASE + 0x7000)
#define K230_KPU_OUTSIDE_LOW \
    (K230_FAKE_KPU_OUTPUT_BASE - K230_KPU_PAGE_SIZE)
#define K230_KPU_OUTSIDE_HIGH     K230_KPU_OUTPUT_END

static void k230_plic_enable_irq(QTestState *qts, unsigned int irq)
{
    uint32_t enable;
    uint64_t enable_addr;

    qtest_writel(qts, K230_PLIC_BASE + irq * 4, 1);
    enable_addr = K230_PLIC_BASE + K230_PLIC_ENABLE_S + (irq / 32) * 4;
    enable = qtest_readl(qts, enable_addr);
    qtest_writel(qts, enable_addr, enable | (1u << (irq % 32)));
    qtest_writel(qts, K230_PLIC_BASE + K230_PLIC_CONTEXT_S +
                 K230_PLIC_THRESHOLD, 0);
}

static uint32_t k230_plic_claim(QTestState *qts)
{
    return qtest_readl(qts, K230_PLIC_BASE + K230_PLIC_CONTEXT_S +
                       K230_PLIC_CLAIM);
}

static void k230_plic_complete(QTestState *qts, unsigned int irq)
{
    qtest_writel(qts, K230_PLIC_BASE + K230_PLIC_CONTEXT_S +
                 K230_PLIC_CLAIM, irq);
}

static void k230_assert_page_byte(QTestState *qts, uint64_t addr,
                                  uint8_t expected)
{
    static const uint64_t offsets[] = {
        0,
        K230_KPU_PAGE_SIZE / 2,
        K230_KPU_PAGE_SIZE - 32,
    };
    uint8_t data[32];

    for (size_t i = 0; i < G_N_ELEMENTS(offsets); i++) {
        qtest_memread(qts, addr + offsets[i], data, sizeof(data));
        for (size_t j = 0; j < sizeof(data); j++) {
            g_assert_cmphex(data[j], ==, expected);
        }
    }
}

static QTestState *k230_kpu_init(void)
{
    QTestState *qts = qtest_init("-machine k230");

    k230_plic_enable_irq(qts, K230_GNNE_IRQ);
    g_assert_cmphex(qtest_readq(qts, K230_KPU_CFG_BASE + K230_GNNE_STATUS),
                    ==, 0);
    g_assert_cmphex(k230_plic_claim(qts), ==, 0);

    return qts;
}

static void k230_kpu_set_command_range(QTestState *qts, uint32_t start,
                                       uint32_t end)
{
    qtest_writel(qts, K230_KPU_CFG_BASE + K230_GNNE_COMMAND_START, start);
    qtest_writel(qts, K230_KPU_CFG_BASE + K230_GNNE_COMMAND_END, end);
}

static void k230_kpu_write_commands(QTestState *qts, const uint32_t *commands,
                                    size_t command_count)
{
    for (size_t i = 0; i < command_count; i++) {
        qtest_writel(qts, K230_GNNE_COMMAND_TEST + i * sizeof(commands[0]),
                     commands[i]);
    }
}

static void k230_kpu_start(QTestState *qts)
{
    qtest_writel(qts, K230_KPU_CFG_BASE + K230_GNNE_START, 0x30d40);
}

static void k230_kpu_run_commands(QTestState *qts, const uint32_t *commands,
                                  size_t command_count)
{
    k230_kpu_write_commands(qts, commands, command_count);
    k230_kpu_set_command_range(qts, K230_GNNE_COMMAND_TEST,
                               K230_GNNE_COMMAND_TEST +
                               command_count * sizeof(commands[0]));
    k230_kpu_start(qts);
    qtest_clock_step(qts, K230_GNNE_DELAY_NS);
}

static void k230_kpu_assert_done_irq(QTestState *qts)
{
    g_assert_cmphex(qtest_readq(qts, K230_KPU_CFG_BASE + K230_GNNE_STATUS),
                    ==, K230_GNNE_DONE);
    g_assert_cmphex(k230_plic_claim(qts), ==, K230_GNNE_IRQ);
}

static void k230_kpu_clear_done_irq(QTestState *qts)
{
    qtest_writeq(qts, K230_KPU_CFG_BASE + K230_GNNE_CLEAR, K230_GNNE_DONE);
    g_assert_cmphex(qtest_readq(qts, K230_KPU_CFG_BASE + K230_GNNE_STATUS),
                    ==, 0);
    k230_plic_complete(qts, K230_GNNE_IRQ);
    g_assert_cmphex(k230_plic_claim(qts), ==, 0);
}

static void test_zero_start_does_not_complete(void)
{
    QTestState *qts = k230_kpu_init();
    const uint32_t commands[] = {
        K230_KPU_OUTPUT_TEST0 | 2,
    };

    qtest_memset(qts, K230_KPU_OUTPUT_TEST0, 0xa5, K230_KPU_PAGE_SIZE);
    k230_kpu_write_commands(qts, commands, G_N_ELEMENTS(commands));
    k230_kpu_set_command_range(qts, K230_GNNE_COMMAND_TEST,
                               K230_GNNE_COMMAND_TEST + sizeof(commands));
    qtest_writel(qts, K230_KPU_CFG_BASE + K230_GNNE_COMMAND_HI, 0x12345678);

    qtest_writel(qts, K230_KPU_CFG_BASE + K230_GNNE_START, 0);
    qtest_clock_step(qts, K230_GNNE_DELAY_NS);

    g_assert_cmphex(qtest_readl(qts, K230_KPU_CFG_BASE +
                                K230_GNNE_COMMAND_HI), ==, 0x12345678);
    g_assert_cmphex(qtest_readq(qts, K230_KPU_CFG_BASE + K230_GNNE_STATUS),
                    ==, 0);
    g_assert_cmphex(k230_plic_claim(qts), ==, 0);
    k230_assert_page_byte(qts, K230_KPU_OUTPUT_TEST0, 0xa5);

    qtest_quit(qts);
}

static void test_delayed_completion(void)
{
    QTestState *qts = k230_kpu_init();
    const uint32_t commands[] = {
        K230_KPU_OUTPUT_TEST0 | 2,
    };

    qtest_memset(qts, K230_KPU_OUTPUT_TEST0, 0xa5, K230_KPU_PAGE_SIZE);
    k230_kpu_write_commands(qts, commands, G_N_ELEMENTS(commands));
    k230_kpu_set_command_range(qts, K230_GNNE_COMMAND_TEST,
                               K230_GNNE_COMMAND_TEST + sizeof(commands));
    k230_kpu_start(qts);

    g_assert_cmphex(qtest_readq(qts, K230_KPU_CFG_BASE + K230_GNNE_STATUS),
                    ==, 0);
    g_assert_cmphex(k230_plic_claim(qts), ==, 0);
    k230_assert_page_byte(qts, K230_KPU_OUTPUT_TEST0, 0xa5);

    qtest_clock_step(qts, K230_GNNE_DELAY_NS / 2);
    g_assert_cmphex(qtest_readq(qts, K230_KPU_CFG_BASE + K230_GNNE_STATUS),
                    ==, 0);
    g_assert_cmphex(k230_plic_claim(qts), ==, 0);
    k230_assert_page_byte(qts, K230_KPU_OUTPUT_TEST0, 0xa5);

    qtest_clock_step(qts, K230_GNNE_DELAY_NS - K230_GNNE_DELAY_NS / 2);
    k230_assert_page_byte(qts, K230_KPU_OUTPUT_TEST0, 0);
    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_command_completion_pages(void)
{
    QTestState *qts = k230_kpu_init();
    const uint32_t commands[] = {
        K230_KPU_OUTPUT_TEST0 | 2,
        K230_KPU_OUTSIDE_LOW | 2,
        K230_KPU_OUTPUT_TEST1 + 0x40,
        (K230_KPU_OUTPUT_TEST0 + 0x80) | 2,
    };

    qtest_memset(qts, K230_KPU_OUTPUT_TEST0, 0xa5, K230_KPU_PAGE_SIZE);
    qtest_memset(qts, K230_KPU_OUTPUT_TEST1, 0x5a, K230_KPU_PAGE_SIZE);
    qtest_memset(qts, K230_KPU_OUTPUT_UNREFERENCED, 0xc3,
                 K230_KPU_PAGE_SIZE);
    qtest_memset(qts, K230_KPU_OUTSIDE_LOW, 0x3c, K230_KPU_PAGE_SIZE);

    k230_kpu_run_commands(qts, commands, G_N_ELEMENTS(commands));

    k230_assert_page_byte(qts, K230_KPU_OUTPUT_TEST0, 0);
    k230_assert_page_byte(qts, K230_KPU_OUTPUT_TEST1, 0);
    k230_assert_page_byte(qts, K230_KPU_OUTPUT_UNREFERENCED, 0xc3);
    k230_assert_page_byte(qts, K230_KPU_OUTSIDE_LOW, 0x3c);
    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_same_page_addresses_zero_one_page(void)
{
    QTestState *qts = k230_kpu_init();
    const uint32_t commands[] = {
        K230_KPU_OUTPUT_TEST0 + 0x20,
        K230_KPU_OUTPUT_TEST0 + 0x8f0,
        K230_KPU_OUTPUT_TEST0 + 0xffc,
    };

    qtest_memset(qts, K230_KPU_OUTPUT_TEST0, 0xa5, K230_KPU_PAGE_SIZE);
    qtest_memset(qts, K230_KPU_OUTPUT_TEST0 + K230_KPU_PAGE_SIZE, 0x5a,
                 K230_KPU_PAGE_SIZE);

    k230_kpu_run_commands(qts, commands, G_N_ELEMENTS(commands));

    k230_assert_page_byte(qts, K230_KPU_OUTPUT_TEST0, 0);
    k230_assert_page_byte(qts, K230_KPU_OUTPUT_TEST0 + K230_KPU_PAGE_SIZE,
                          0x5a);
    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_output_window_boundaries(void)
{
    QTestState *qts = k230_kpu_init();
    const uint32_t commands[] = {
        K230_KPU_OUTPUT_TEST0 + 1,
        K230_KPU_OUTPUT_LAST + 0x80,
        K230_KPU_OUTSIDE_LOW + 2,
        K230_KPU_OUTSIDE_HIGH,
    };

    qtest_memset(qts, K230_KPU_OUTPUT_TEST0, 0xa5, K230_KPU_PAGE_SIZE);
    qtest_memset(qts, K230_KPU_OUTPUT_LAST, 0x5a, K230_KPU_PAGE_SIZE);
    qtest_memset(qts, K230_KPU_OUTSIDE_LOW, 0x3c, K230_KPU_PAGE_SIZE);
    qtest_memset(qts, K230_KPU_OUTSIDE_HIGH, 0xc3, K230_KPU_PAGE_SIZE);

    k230_kpu_run_commands(qts, commands, G_N_ELEMENTS(commands));

    k230_assert_page_byte(qts, K230_KPU_OUTPUT_TEST0, 0);
    k230_assert_page_byte(qts, K230_KPU_OUTPUT_LAST, 0);
    k230_assert_page_byte(qts, K230_KPU_OUTSIDE_LOW, 0x3c);
    k230_assert_page_byte(qts, K230_KPU_OUTSIDE_HIGH, 0xc3);
    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_empty_command_range(void)
{
    QTestState *qts = k230_kpu_init();

    qtest_memset(qts, K230_KPU_OUTPUT_TEST0, 0xa5, K230_KPU_PAGE_SIZE);

    k230_kpu_set_command_range(qts, 0, 0);
    k230_kpu_start(qts);
    qtest_clock_step(qts, K230_GNNE_DELAY_NS);

    k230_assert_page_byte(qts, K230_KPU_OUTPUT_TEST0, 0xa5);
    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_reversed_command_range(void)
{
    QTestState *qts = k230_kpu_init();
    const uint32_t commands[] = {
        K230_KPU_OUTPUT_TEST0 | 2,
    };

    qtest_memset(qts, K230_KPU_OUTPUT_TEST0, 0xa5, K230_KPU_PAGE_SIZE);
    k230_kpu_write_commands(qts, commands, G_N_ELEMENTS(commands));
    k230_kpu_set_command_range(qts, K230_GNNE_COMMAND_TEST + sizeof(commands),
                               K230_GNNE_COMMAND_TEST);
    k230_kpu_start(qts);
    qtest_clock_step(qts, K230_GNNE_DELAY_NS);

    k230_assert_page_byte(qts, K230_KPU_OUTPUT_TEST0, 0xa5);
    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_clear_status_allows_second_run(void)
{
    QTestState *qts = k230_kpu_init();
    const uint32_t first_commands[] = {
        K230_KPU_OUTPUT_TEST0 | 2,
    };
    const uint32_t second_commands[] = {
        K230_KPU_OUTPUT_TEST1 | 2,
    };

    qtest_memset(qts, K230_KPU_OUTPUT_TEST0, 0xa5, K230_KPU_PAGE_SIZE);
    qtest_memset(qts, K230_KPU_OUTPUT_TEST1, 0x5a, K230_KPU_PAGE_SIZE);

    k230_kpu_run_commands(qts, first_commands, G_N_ELEMENTS(first_commands));
    k230_assert_page_byte(qts, K230_KPU_OUTPUT_TEST0, 0);
    k230_assert_page_byte(qts, K230_KPU_OUTPUT_TEST1, 0x5a);
    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    g_assert_cmphex(qtest_readq(qts, K230_KPU_CFG_BASE + K230_GNNE_STATUS),
                    ==, 0);
    k230_kpu_run_commands(qts, second_commands, G_N_ELEMENTS(second_commands));
    k230_assert_page_byte(qts, K230_KPU_OUTPUT_TEST1, 0);
    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

int main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/k230-kpu/zero-start-does-not-complete",
                   test_zero_start_does_not_complete);
    qtest_add_func("/k230-kpu/delayed-completion",
                   test_delayed_completion);
    qtest_add_func("/k230-kpu/command-completion-pages",
                   test_command_completion_pages);
    qtest_add_func("/k230-kpu/same-page-addresses-zero-one-page",
                   test_same_page_addresses_zero_one_page);
    qtest_add_func("/k230-kpu/output-window-boundaries",
                   test_output_window_boundaries);
    qtest_add_func("/k230-kpu/empty-command-range",
                   test_empty_command_range);
    qtest_add_func("/k230-kpu/reversed-command-range",
                   test_reversed_command_range);
    qtest_add_func("/k230-kpu/clear-status-allows-second-run",
                   test_clear_status_allows_second_run);

    return g_test_run();
}
