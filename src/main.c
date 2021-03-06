// vim: tabstop=8 softtabstop=8 shiftwidth=8 noexpandtab
/*
 * This file is part of the libopencm3 project.
 *
 * Copyright (C) 2010 Gareth McMullin <gareth@blacksphere.co.nz>
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <libopencm3/cm3/nvic.h>
#include <libopencm3/cm3/systick.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/usb/usbd.h>
#include <libopencm3/usb/cdc.h>
#include "../libopencm3/include/libopencmsis/core_cm3.h"

#include "fake6502.h"
#include "acia6850.h"
#include "rom.h"
#include "cdcacm.h"
#include "version.h"

usbd_device *usbd_dev;

const struct usb_device_descriptor dev_descr = {
	.bLength = USB_DT_DEVICE_SIZE,
	.bDescriptorType = USB_DT_DEVICE,
	.bcdUSB = 0x0200,
	.bDeviceClass = 0,
	.bDeviceSubClass = 0,
	.bDeviceProtocol = 0,
	.bMaxPacketSize0 = 64,
	.idVendor = 0x05ac,
	.idProduct = 0x2227,
	.bcdDevice = 0x0200,
	.iManufacturer = 1,
	.iProduct = 2,
	.iSerialNumber = 3,
	.bNumConfigurations = 1,
};


const struct usb_interface ifaces[] = {{
	.num_altsetting = 1,
	.iface_assoc = &uart_assoc,
	.altsetting = uart_comm_iface,
}, {
	.num_altsetting = 1,
	.altsetting = uart_data_iface,
}};

const struct usb_config_descriptor config = {
	.bLength = USB_DT_CONFIGURATION_SIZE,
	.bDescriptorType = USB_DT_CONFIGURATION,
	.wTotalLength = 0,
	.bNumInterfaces = sizeof(ifaces)/sizeof(ifaces[0]),
	.bConfigurationValue = 1,
	.iConfiguration = 0,
	.bmAttributes = 0xC0,
	.bMaxPower = 0x32,

	.interface = ifaces,
};

static const char *usb_strings[] = {
	"satoshinm",
	"Pill 6502",
	"6502",
	"Pill 6502 UART Port",
};

static bool paused = true;
volatile uint32_t millis = 0;

void dowork(void)
{
	if (paused) return;
	++millis;
	/* Should we single step (1khz clock) */
	//step6502();
	/* Or do the Fandango (1Mhz clock) */
	exec6502(1000);
	/* Toggle LED once per cycle? Nah, save the clocks */
	// gpio_toggle(GPIOC, GPIO13);
}

void sys_tick_handler(void)
{
	if (!paused) {
		++millis;
    }
}


static void usb_set_config(usbd_device *dev, uint16_t wValue)
{
	cdcacm_set_config(dev, wValue);
}

static void usb_reset()
{
	reset6502();
	paused = false;
}

extern bool local_echo;
char *process_serial_command(char b) {
	if (b == '\x16') { // ^V
		return "Pill 6502 version " FIRMWARE_VERSION;
	} else if (b == '\x10') { // ^P
		paused = !paused;
		return paused ? "paused" : "resumed";
	} else if (b == '\x06') { // ^F
	} else if (b == '\x12') { // ^R
		reset6502();
		paused = false;
		return "reset";
	} else if (b == '\x05') { // ^E
		local_echo = !local_echo;
		return local_echo ? "local echo enabled" : "local echo disabled";
	} else if (b == '\x14') { // ^T
		static uint32_t last_ticks = 0;
		static uint32_t last_millis = 0;
		uint32_t hz = 0;
		if (last_ticks != 0) {
			uint32_t elapsed_ticks = clockticks6502 - last_ticks;
			uint32_t elapsed_millis = millis - last_millis;

			if (elapsed_millis != 0) hz = elapsed_ticks * 1000 / elapsed_millis;
		}

		last_ticks = clockticks6502;
		last_millis = millis;

		static char buf[64];
		snprintf(buf, sizeof(buf), "%ld ticks\r\n%ld instructions\r\n%ld Hz", clockticks6502, instructions, hz);
		return buf;
	} else if (b == '\x07') { // ^G
		return "^V=version ^R=reset ^E=echo ^P=pause ^T=timing ^G=help";
	}

	return NULL;
}

static void setup_clock(void) {
	rcc_clock_setup_in_hsi_out_48mhz();
	rcc_periph_clock_enable(RCC_GPIOC);

	systick_set_clocksource(STK_CSR_CLKSOURCE_AHB_DIV8);
	/* SysTick interrupt every N clock pulses: set reload to N-1
	 * Period: N / (72 MHz / 8 )
	 * */
	systick_set_reload(8999); // 1 ms
	systick_interrupt_enable();
	systick_counter_enable();
}

static void setup_gpio(void) {
	// Built-in LED on blue pill board, PC13
	gpio_set_mode(GPIOC, GPIO_MODE_OUTPUT_2_MHZ,
		GPIO_CNF_OUTPUT_PUSHPULL, GPIO13);
	gpio_set(GPIOC, GPIO13);

}

/* Buffer to be used for control requests. */
uint8_t usbd_control_buffer[128];


// 6502 processor memory, 16KB (< 20KB)
uint8_t ram[0x4000];
uint8_t read6502(uint16_t address) {
	// RAM
	if (address < sizeof(ram)) {
		return ram[address];
	}

	// ROM
	if (address >= 0xc000) {
		const uint8_t *rom = &_binary____osi_bas_ROM_o_bin_start;

		return rom[address - 0xc000];
	}

	// ACIA
	if (address >= 0xa000 && address <= 0xbfff) {
		return read6850(address);
	}

	return 0xff;
}

void write6502(uint16_t address, uint8_t value) {
	// RAM
	if (address < sizeof(ram)) {
		ram[address] = value;
	}

	// ACIA
	if (address >= 0xa000 && address <= 0xbfff) {
		write6850(address, value);
	}

}

int main(void)
{
	setup_clock();
	setup_gpio();

	rcc_periph_clock_enable(RCC_GPIOA);
	// Re-enumeration hack from libopencm3 example
	/*
	 * Vile hack to reenumerate, physically _drag_ d+ low.
	 * do NOT do this if you're board has proper usb pull up control!
	 * (need at least 2.5us to trigger usb disconnect)
	 */
	gpio_set_mode(GPIOA, GPIO_MODE_OUTPUT_2_MHZ,
		GPIO_CNF_OUTPUT_PUSHPULL, GPIO12);
	gpio_clear(GPIOA, GPIO12);
	for (unsigned int i = 0; i < 800000; i++) {
		__asm__("nop");
	}

	rcc_periph_clock_enable(RCC_OTGFS);

	usbd_dev = usbd_init(&st_usbfs_v1_usb_driver, &dev_descr, &config, usb_strings,
		sizeof(usb_strings)/sizeof(char *),
		usbd_control_buffer, sizeof(usbd_control_buffer));
	usbd_register_set_config_callback(usbd_dev, usb_set_config);
	usbd_register_reset_callback(usbd_dev, usb_reset);

	reset6502();
	nvic_enable_irq(NVIC_USB_LP_CAN_RX0_IRQ);
	nvic_enable_irq(NVIC_USB_WAKEUP_IRQ);

	// TODO: put CPU to sleep when we have nothing to do
	uint32_t cached_millis=millis;
	while (1){
		while (cached_millis != millis)
		{
			dowork();
			++cached_millis;
		}
	}
}

void usb_wakeup_isr(void)
{
  usbd_poll(usbd_dev);
}

void usb_lp_can_rx0_isr(void)
{
  usbd_poll(usbd_dev);
}