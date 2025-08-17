/*
 * Gevico TCG system testcase head file
 *
 *  Copyright (c) Zevorn(Chao Liu) chao.liu@yeah.net
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef GEVICO_PL011_H
#define GEVICO_PL011_H

#define PL011_IO_BASE    0x10000000UL
#define UART_DR          0x00 /* data register */
#define UART_RSR_ECR     0x04 /* receive status or error clear */
#define UART_DMAWM       0x08 /* DMA watermark configure */
#define UART_TIMEOUT     0x0C /* Timeout period */

/* reserved space */
#define UART_FR      0x18 /* flag register */
#define UART_ILPR    0x20 /* IrDA low-poer */
#define UART_IBRD    0x24 /* integer baud register */
#define UART_FBRD    0x28 /* fractional baud register */
#define UART_LCR_H   0x2C /* line control register */
#define UART_CR      0x30 /* control register */
#define UART_IFLS    0x34 /* interrupt FIFO level select */
#define UART_IMSC    0x38 /* interrupt mask set/clear */
#define UART_RIS     0x3C /* raw interrupt register */
#define UART_MIS     0x40 /* masked interrupt register */
#define UART_ICR     0x44 /* interrupt clear register */
#define UART_DMACR   0x48 /* DMA control register */

/* flag register bits */
#define UART_FR_RTXDIS    (1 << 13)
#define UART_FR_TERI      (1 << 12)
#define UART_FR_DDCD      (1 << 11)
#define UART_FR_DDSR      (1 << 10)
#define UART_FR_DCTS      (1 << 9)
#define UART_FR_RI        (1 << 8)
#define UART_FR_TXFE      (1 << 7)
#define UART_FR_RXFF      (1 << 6)
#define UART_FR_TXFF      (1 << 5)
#define UART_FR_RXFE      (1 << 4)
#define UART_FR_BUSY      (1 << 3)
#define UART_FR_DCD       (1 << 2)
#define UART_FR_DSR       (1 << 1)
#define UART_FR_CTS       (1 << 0)

#endif
