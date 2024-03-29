#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include "hw_errors.h"

/* LEDs */
static const struct gpio_dt_spec load_oc_led =
	GPIO_DT_SPEC_GET(DT_ALIAS(load_overcurrent_led), gpios);

static const struct gpio_dt_spec batt_oc_led =
	GPIO_DT_SPEC_GET(DT_ALIAS(batt_overcurrent_led), gpios);

static const struct gpio_dt_spec batt_ov_led =
	GPIO_DT_SPEC_GET(DT_ALIAS(batt_overvoltage_led), gpios);

/* Input Signals from HW */
static const struct gpio_dt_spec load_oc =
	GPIO_DT_SPEC_GET(DT_NODELABEL(load_overcurrent), gpios);

static const struct gpio_dt_spec batt_oc =
	GPIO_DT_SPEC_GET(DT_NODELABEL(batt_overcurrent), gpios);

static const struct gpio_dt_spec batt_ov =
	GPIO_DT_SPEC_GET(DT_NODELABEL(batt_overvoltage), gpios);

HwErrors::HwErrors() : error_code(0), error_count(100), cur_error_count(0), rst_ctr(1000)
{
	gpio_pin_configure_dt(&load_oc_led, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&batt_oc_led, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&batt_ov_led, GPIO_OUTPUT_INACTIVE);

	gpio_pin_configure_dt(&load_oc, GPIO_INPUT);
	gpio_pin_configure_dt(&batt_oc, GPIO_INPUT);
	gpio_pin_configure_dt(&batt_ov, GPIO_INPUT);
}

uint8_t HwErrors::check(void)
{
	// Every 1000 calls, if there is no errors zero the error counter
	// we're looking for errors happening in a row
	if(!rst_ctr-- && !error_code ) {
		cur_error_count = 0x00;
		rst_ctr = 1000;
	}

	/* Load Overcurrent */
	if (gpio_pin_get_dt(&load_oc)) {
		cur_error_count++;
		gpio_pin_set_dt(&load_oc_led, true);
		if(cur_error_count > error_count) {
			printk("Load Overcurrent! \n");
			error_code |= (1 << Errors::LOAD_OC);
		}
	}

	/* Battery Overcurrent */
	if (gpio_pin_get_dt(&batt_oc)) {
		cur_error_count++;
		gpio_pin_set_dt(&batt_oc_led, true);
		if(cur_error_count > error_count) {
			printk("Battery Overcurrent! \n");
			error_code |= (1 << Errors::BATT_OC);
		}
	}

	/* Battery Overvoltage */
	if (gpio_pin_get_dt(&batt_ov)) {
		cur_error_count++;
		gpio_pin_set_dt(&batt_ov_led, true);
		if(cur_error_count > error_count) {
			printk("Battery Overvoltage! \n");
			error_code |= (1 << Errors::BATT_OV);
		}
	}

	return error_code;
}


void HwErrors::clear(void)
{
	error_code = 0x00;
	cur_error_count = 0x00;
}
