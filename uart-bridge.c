// SPDX-License-Identifier: MIT
/*
 * Copyright 2021 Álvaro Fernández Rojas <noltari@gmail.com>
 */

#include <hardware/irq.h>
#include <hardware/structs/sio.h>
#include <hardware/uart.h>
#include <hardware/structs/pio.h>
#include <pico/multicore.h>
#include <pico/stdlib.h>
#include <string.h>
#include <tusb.h>
#include <uart_rx.pio.h>
#include <uart_tx.pio.h>
#include <hardware/flash.h>
#include "serial.h"

#if !defined(MIN)
#define MIN(a, b) ((a > b) ? b : a)
#endif /* MIN */

#define LED_PIN 25

// might as well use our RAM
#define BUFFER_SIZE 2560

#define DEF_BIT_RATE 921600
// #define DEF_BIT_RATE 115200
#define DEF_STOP_BITS 1
#define DEF_PARITY 0
#define DEF_DATA_BITS 8

typedef struct
{
	uart_inst_t *const inst;
	uint8_t tx_pin;
	uint8_t rx_pin;
	uint8_t sws_pin;
	uint8_t rst_pin;
	uint sm;
} uart_id_t;

typedef struct
{
	cdc_line_coding_t usb_lc;
	cdc_line_coding_t uart_lc;
	mutex_t lc_mtx;
	uint8_t uart_rx_buffer[BUFFER_SIZE];
	uint32_t uart_rx_pos;
	uint8_t uart_to_usb_buffer[BUFFER_SIZE];
	uint32_t uart_to_usb_pos;
	mutex_t uart_mtx;
	uint8_t usb_to_uart_buffer[BUFFER_SIZE];
	uint32_t usb_to_uart_pos;
	uint32_t usb_to_uart_snd;
	mutex_t usb_mtx;
} uart_data_t;

// Device pin order   SWS - RST - TX  - RX //repeating
// First screen being 2		3	  4  	5

// format	tx,rx
// debug: 	0, 1
// screen1:	4,5
// screen1:	8,9
// screen1:	12,13

// screen2.init(8, 9, 6, 7, &rx2); // Starts at 4
// screen3.init(12, 13, 10, 11, &rx3); // Starts at 4

// old external config

// uart_id_t UART_ID[CFG_TUD_CDC] = {
// 	{
// 		.inst = uart1, // screen 1
// 		.tx_pin = 4,
// 		.rx_pin = 5,
// 		.rst_pin = 3,
// 	},
// 	{
// 		.inst = 0, // screen 2
// 		.tx_pin = 8,
// 		.rx_pin = 9,
// 		.rst_pin = 7,
// 		.sm = 0,
// 	},
// 	{
// 		.inst = uart0, // screen 3
// 		.tx_pin = 12,
// 		.rx_pin = 13,
// 		.rst_pin = 11,
// 	},
// 	{
// 		.inst = 0, // SWS 1
// 		.rst_pin = 3,
// 		.tx_pin = 2,
// 		.sm = 1,
// 	},
// 	{
// 		.inst = 0, // SWS 2
// 		.rst_pin = 7,
// 		.tx_pin = 6,
// 		.sm = 2,
// 	},
// 	{
// 		.inst = 0, // SWS 3
// 		.rst_pin = 11,
// 		.tx_pin = 10,
// 		.sm = 3,
// 	}};

// New internal config
uart_id_t UART_ID[CFG_TUD_CDC] = {
	{
		.inst = uart0, // screen 1
		.tx_pin = 0,
		.rx_pin = 1,
		.rst_pin = 3,
	},
	{
		.inst = uart1, // screen 2
		.tx_pin = 4,
		.rx_pin = 5,
		.rst_pin = 7,
	},
	{
		.inst = 0, // screen 3
		.tx_pin = 8,
		.rx_pin = 9,
		.rst_pin = 11,
		.sm = 0,
	},
	{
		.inst = 0, // SWS 1
		.rst_pin = 3,
		.tx_pin = 2,
		.sm = 1,
	},
	{
		.inst = 0, // SWS 2
		.rst_pin = 7,
		.tx_pin = 6,
		.sm = 2,
	},
	{
		.inst = 0, // SWS 3
		.rst_pin = 11,
		.tx_pin = 10,
		.sm = 3,
	}};

uart_data_t UART_DATA[CFG_TUD_CDC];

uint rx_offset = 0;
uint rxp_offset = 0;

uint tx_offset = 0;
uint txp_offset = 0;

static inline uint databits_usb2uart(uint8_t data_bits)
{
	switch (data_bits)
	{
	case 5:
		return 5;
	case 6:
		return 6;
	case 7:
		return 7;
	default:
		return 8;
	}
}

static inline uart_parity_t parity_usb2uart(uint8_t usb_parity)
{
	switch (usb_parity)
	{
	case 1:
		return UART_PARITY_ODD;
	case 2:
		return UART_PARITY_EVEN;
	default:
		return UART_PARITY_NONE;
	}
}

static inline uint stopbits_usb2uart(uint8_t stop_bits)
{
	switch (stop_bits)
	{
	case 2:
		return 2;
	default:
		return 1;
	}
}

void update_uart_cfg(uint8_t itf)
{
	uart_id_t *ui = &UART_ID[itf];
	uart_data_t *ud = &UART_DATA[itf];

	if (mutex_try_enter(&ud->lc_mtx, NULL))
	{
		if (ui->inst != 0)
		{ // regular uart
			if (ud->usb_lc.bit_rate != ud->uart_lc.bit_rate)
			{
				uart_set_baudrate(ui->inst, ud->usb_lc.bit_rate);
				ud->uart_lc.bit_rate = ud->usb_lc.bit_rate;
			}

			if ((ud->usb_lc.stop_bits != ud->uart_lc.stop_bits) ||
				(ud->usb_lc.parity != ud->uart_lc.parity) ||
				(ud->usb_lc.data_bits != ud->uart_lc.data_bits))
			{
				uart_set_format(ui->inst,
								databits_usb2uart(ud->usb_lc.data_bits),
								stopbits_usb2uart(ud->usb_lc.stop_bits),
								parity_usb2uart(ud->usb_lc.parity));
				ud->uart_lc.data_bits = ud->usb_lc.data_bits;
				ud->uart_lc.parity = ud->usb_lc.parity;
				ud->uart_lc.stop_bits = ud->usb_lc.stop_bits;
			}
		}
		else
		{
			if (ud->usb_lc.bit_rate != ud->uart_lc.bit_rate)
			{
				uart_baud(pio0, ui->sm, ud->usb_lc.bit_rate);
				uart_baud(pio1, ui->sm, ud->usb_lc.bit_rate);
				ud->uart_lc.bit_rate = ud->usb_lc.bit_rate;
			}
			if (ud->usb_lc.parity != ud->uart_lc.parity)
			{
				ud->uart_lc.parity = ud->usb_lc.parity;
				if (ud->usb_lc.parity == UART_PARITY_NONE)
				{
					if (ui->rx_pin)
						uart_rx_program_init(pio0, ui->sm, rx_offset, ui->rx_pin, ud->uart_lc.bit_rate);
					uart_tx_program_init(pio1, ui->sm, tx_offset, ui->tx_pin, ud->uart_lc.bit_rate);
				}
				else
				{
					if (ui->rx_pin)
						uart_rx_program_init(pio0, ui->sm, rxp_offset, ui->rx_pin, ud->uart_lc.bit_rate);
					uart_tx_program_init(pio1, ui->sm, txp_offset, ui->tx_pin, ud->uart_lc.bit_rate);
				}
			}
		}
		mutex_exit(&ud->lc_mtx);
	}
}

char tempData[BUFFER_SIZE] = {};

void usb_read_bytes(uint8_t itf)
{
	uint32_t len = tud_cdc_n_available(itf);

	if (len)
	{
		uart_data_t *ud = &UART_DATA[itf];

		mutex_enter_blocking(&ud->usb_mtx);

		len = MIN(len, BUFFER_SIZE - ud->usb_to_uart_pos);
		if (len)
		{
			// uint32_t count = 0;
			uint32_t count;

			count = tud_cdc_n_read(itf, &ud->usb_to_uart_buffer[ud->usb_to_uart_pos], len);

			// char *start = tempData;

			// len = tud_cdc_n_read(itf, tempData, len);
			// tempData[len] = '\0';

			// char *cmd = strstr(tempData, ":-:"); // pointer to the first instance of :-: or null

			// while (cmd)
			// {
			// 	// copy the bit before the command to the uart buffer
			// 	uint32_t preCmdSize = start - cmd;
			// 	if (preCmdSize > 0)
			// 	{
			// 		// Copy into the uart buffer
			// 		memcpy(&ud->usb_to_uart_buffer[ud->usb_to_uart_pos], start, preCmdSize);
			// 		count += preCmdSize;
			// 	}

			// 	if (strncmp(cmd, ":-:RST", 6)) // compare the first 6 chars
			// 	{
			// 		gpio_put(UART_ID[itf].rst_pin, *(cmd + 6) == '1');
			// 		start = cmd + 9; // :-:RST1\r\n
			// 	}
			// 	else if (strncmp(cmd, ":-:SWS", 6))
			// 	{
			// 		start = cmd + 9; // :-:SWS1\r\n
			// 	}
			// 	else
			// 	{
			// 		start = cmd + 5;
			// 	}

			// 	cmd = strstr(start, ":-:");
			// }

			// // copy the bit after the command to the uart buffer
			// uint32_t postCmdSize = len - (start - tempData);
			// if (postCmdSize > 0)
			// {
			// 	// Copy into the uart buffer
			// 	memcpy(&ud->usb_to_uart_buffer[ud->usb_to_uart_pos], start, postCmdSize);
			// 	count += postCmdSize;
			// }

			ud->usb_to_uart_pos += count;
		}

		mutex_exit(&ud->usb_mtx);
	}
}

void usb_write_bytes(uint8_t itf)
{
	uart_data_t *ud = &UART_DATA[itf];

	if (ud->uart_to_usb_pos && mutex_try_enter(&ud->uart_mtx, NULL))
	{
		uint32_t count;

		count = tud_cdc_n_write(itf, ud->uart_to_usb_buffer, ud->uart_to_usb_pos);
		if (count < ud->uart_to_usb_pos)
			memcpy(ud->uart_to_usb_buffer, &ud->uart_to_usb_buffer[count],
				   ud->uart_to_usb_pos - count);
		ud->uart_to_usb_pos -= count;

		mutex_exit(&ud->uart_mtx);

		if (count)
			tud_cdc_n_write_flush(itf);
	}
}

void usb_cdc_process(uint8_t itf)
{
	uart_data_t *ud = &UART_DATA[itf];

	mutex_enter_blocking(&ud->lc_mtx);
	tud_cdc_n_get_line_coding(itf, &ud->usb_lc);
	mutex_exit(&ud->lc_mtx);

	usb_read_bytes(itf);
	usb_write_bytes(itf);
}

void core1_entry(void)
{
	printf("Starting USB Devices");
	tusb_init();

	while (1)
	{
		int itf;
		int con = 0;

		tud_task();

		for (itf = 0; itf < CFG_TUD_CDC; itf++)
		{
			if (tud_cdc_n_connected(itf))
			{
				if (UART_ID[itf].rst_pin)
				{
					uint8_t state = tud_cdc_n_get_line_state(itf);

					uint8_t rtsMask = 0b00000010; // RTS Mask for line state
					bool rtsState = (state & rtsMask) == 2;
					gpio_put(UART_ID[itf].rst_pin, !rtsState); // write inverted
				}

				usb_cdc_process(itf);
			}
		}

		gpio_put(LED_PIN, con);
	}
}

void uart_read_bytes(uint8_t itf)
{
	const uart_id_t *ui = &UART_ID[itf];
	uart_data_t *ud = &UART_DATA[itf];

	if (ui->inst != 0)
	{
		if (ui->rx_pin && uart_is_readable(ui->inst))
		{
			while (uart_is_readable(ui->inst) &&
				   ud->uart_rx_pos < BUFFER_SIZE)
			{
				ud->uart_rx_buffer[ud->uart_rx_pos] = uart_getc(ui->inst);
				ud->uart_rx_pos++;
			}
		}
	}
	else
	{
		if (ui->rx_pin && !pio_sm_is_rx_fifo_empty(pio0, ui->sm))
		{
			while (!pio_sm_is_rx_fifo_empty(pio0, ui->sm) &&
				   ud->uart_rx_pos < BUFFER_SIZE)
			{
				ud->uart_rx_buffer[ud->uart_rx_pos] = uart_rx_program_getc(pio0, ui->sm);
				ud->uart_rx_pos++;
			}
		}
	}
	// If we can get the uart mutex then copy the UART data to the uart USB sender, otherwise we'll get it next time around
	if (mutex_try_enter(&ud->uart_mtx, NULL))
	{
		// Ensure we don't overflow the uart_to_usb_buffer
		uint32_t len = MIN(ud->uart_rx_pos, BUFFER_SIZE - ud->uart_to_usb_pos);
		memcpy(&ud->uart_to_usb_buffer[ud->uart_to_usb_pos], ud->uart_rx_buffer, len);
		ud->uart_to_usb_pos += len;
		ud->uart_rx_pos = 0;
		mutex_exit(&ud->uart_mtx);
	}
}

void uart_write_bytes(uint8_t itf)
{
	uart_data_t *ud = &UART_DATA[itf];

	// Try to get the usb_mutex and don't block if we cannot get it, we'll TX the data next passs
	if ((ud->usb_to_uart_pos) && (ud->usb_to_uart_snd < ud->usb_to_uart_pos) &&
		mutex_try_enter(&ud->usb_mtx, NULL))
	{
		const uart_id_t *ui = &UART_ID[itf];

		if (ui->inst != 0)
		{
			while (uart_is_writable(ui->inst) && (ud->usb_to_uart_snd < ud->usb_to_uart_pos))
			{
				uart_putc(ui->inst, ud->usb_to_uart_buffer[ud->usb_to_uart_snd++]);
			}
		}
		else
		{
			size_t bufspace = 7 - pio_sm_get_tx_fifo_level(pio1, ui->sm);
			size_t tosend = ud->usb_to_uart_pos - ud->usb_to_uart_snd;
			tosend = MIN(tosend, bufspace);

			for (size_t i = 0; i < tosend; ++i)
			{
				uart_tx_program_putc(pio1, ui->sm, ud->usb_to_uart_buffer[ud->usb_to_uart_snd + i], ud->usb_lc.parity);
			}
			ud->usb_to_uart_snd += tosend;
		}
		// only reset buffers if we've sent everything
		if (ud->usb_to_uart_snd == ud->usb_to_uart_pos)
		{
			ud->usb_to_uart_pos = 0;
			ud->usb_to_uart_snd = 0;
		}
		mutex_exit(&ud->usb_mtx);
	}
}

static inline void init_usb_cdc_serial_num()
{
	uint8_t id[8];
	flash_get_unique_id(id);
	for (int i = 0; i < 8; ++i)
	{
		sprintf(serial + 2 * i, "%X", id[i]);
	}
	serial[16] = '\0';
}

void init_uart_data(uint8_t itf)
{
	uart_id_t *ui = &UART_ID[itf];
	uart_data_t *ud = &UART_DATA[itf];

	if (ui->inst != 0)
	{
		/* Pinmux */
		gpio_set_function(ui->tx_pin, GPIO_FUNC_UART);
		if (ui->rx_pin)
			gpio_set_function(ui->rx_pin, GPIO_FUNC_UART);
	}

	if (ui->rst_pin)
	{
		gpio_init(ui->rst_pin);
		gpio_set_dir(ui->rst_pin, true);
		gpio_put(ui->rst_pin, true);
	}

	/* USB CDC LC */
	ud->usb_lc.bit_rate = DEF_BIT_RATE;
	ud->usb_lc.data_bits = DEF_DATA_BITS;
	ud->usb_lc.parity = DEF_PARITY;
	ud->usb_lc.stop_bits = DEF_STOP_BITS;

	/* UART LC */
	ud->uart_lc.bit_rate = DEF_BIT_RATE;
	ud->uart_lc.data_bits = DEF_DATA_BITS;
	ud->uart_lc.parity = DEF_PARITY;
	ud->uart_lc.stop_bits = DEF_STOP_BITS;

	/* Buffer */
	ud->uart_rx_pos = 0;
	ud->uart_to_usb_pos = 0;
	ud->usb_to_uart_pos = 0;
	ud->usb_to_uart_snd = 0;

	/* Mutex */
	mutex_init(&ud->lc_mtx);
	mutex_init(&ud->uart_mtx);
	mutex_init(&ud->usb_mtx);

	if (ui->inst != 0)
	{
		/* UART start */
		uart_init(ui->inst, ud->usb_lc.bit_rate);
		uart_set_hw_flow(ui->inst, false, false);
		uart_set_format(ui->inst, databits_usb2uart(ud->usb_lc.data_bits),
						stopbits_usb2uart(ud->usb_lc.stop_bits),
						parity_usb2uart(ud->usb_lc.parity));
	}
	else
	{
		// Set up the state machine we're going to use to for rx/tx
		if (ui->rx_pin)
			uart_rx_program_init(pio0, ui->sm, rx_offset, ui->rx_pin, ud->uart_lc.bit_rate);
		uart_tx_program_init(pio1, ui->sm, tx_offset, ui->tx_pin, ud->uart_lc.bit_rate);
	}
}

int main(void)
{
	int itf;

	// store our PIO programs in tbe instruction registers
	// we'll use pio0 for RX and pio1 for tx so only one copy of each is needed
	// however we'll use a different program to send/receive with parity
	rx_offset = pio_add_program(pio0, &uart_rx_program);
	tx_offset = pio_add_program(pio1, &uart_tx_program);
	rxp_offset = pio_add_program(pio0, &uart_rxp_program);
	txp_offset = pio_add_program(pio1, &uart_txp_program);

	init_usb_cdc_serial_num();

	printf("Starting UART Devices");

	for (itf = 0; itf < CFG_TUD_CDC; itf++)
		init_uart_data(itf);

	gpio_init(LED_PIN);
	gpio_set_dir(LED_PIN, GPIO_OUT);

	multicore_launch_core1(core1_entry);

	while (1)
	{
		for (itf = 0; itf < CFG_TUD_CDC; itf++)
		{
			update_uart_cfg(itf);
			uart_read_bytes(itf);
			uart_write_bytes(itf);
		}
	}

	return 0;
}
