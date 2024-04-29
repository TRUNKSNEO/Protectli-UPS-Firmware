#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/device.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/uart.h>
#include <string.h>
#include <stdio.h>

#include "hw_errors.h"
#include "pid.h"
#include "adc.h"
#include "battery.h"

extern "C" {
#include <msg.h>
}

#define PERIOD    PWM_NSEC(2500) // 500Khz
#define STACKSIZE 32768

#define MSG_SIZE 32
K_MSGQ_DEFINE(uart_msgq, MSG_SIZE, 10, 4);

static const struct pwm_dt_spec pwm = PWM_DT_SPEC_GET(DT_ALIAS(pwm_0));

static const struct gpio_dt_spec pwm_en =
	GPIO_DT_SPEC_GET(DT_ALIAS(pwm_en), gpios);

static const struct gpio_dt_spec pwm_skip =
	GPIO_DT_SPEC_GET(DT_ALIAS(pwm_skip), gpios);

static const struct gpio_dt_spec vin_detect =
	GPIO_DT_SPEC_GET(DT_ALIAS(vin_detect), gpios);

static const struct gpio_dt_spec pack_boot =
	GPIO_DT_SPEC_GET(DT_ALIAS(pack_boot), gpios);

static const struct gpio_dt_spec gpio2 =
	GPIO_DT_SPEC_GET(DT_ALIAS(gpio_2), gpios);

static const struct gpio_dt_spec gpio3 =
	GPIO_DT_SPEC_GET(DT_ALIAS(gpio_3), gpios);

static const struct device *const uart_dev =
	DEVICE_DT_GET(DT_CHOSEN(zephyr_mcu_uart));

void print_voltages(Adc adc, float drive)
{
	printk("Vout : %d mV\t", adc.get_vout());
	printk("Vbat : %d mV\t", adc.get_vbat());
	printk("Iout : %d mA\t", adc.get_iout());
	printk("Ibat : %d mA\t", adc.get_ibat());
	printk("Drve : %f\n", drive);
}

/* receive buffer used in UART ISR callback */
static char rx_buf[MSG_SIZE];
static int rx_buf_pos;

struct k_msgq msgq;
char msgq_buffer[sizeof(struct Msg) * 2];

/*
 * Read characters from UART until line end is detected. Afterwards push the
 * data to the message queue.
 */
void serial_cb(const struct device *dev, void *user_data)
{
	uint8_t c;

	if (!uart_irq_update(uart_dev)) {
		return;
	}

	if (!uart_irq_rx_ready(uart_dev)) {
		return;
	}

	while (uart_fifo_read(uart_dev, &c, 1) == 1) {
		if (!c && rx_buf_pos > sizeof(struct Msg)) {
			// We have a complete packet
			struct Msg msg = {};
			msg_cobs_decode(rx_buf, &msg);
			while (k_msgq_put(&msgq, &msg, K_NO_WAIT) != 0) {
				k_msgq_purge(&msgq);
			}
			rx_buf_pos = 0;
		} else if (c) {
			rx_buf[rx_buf_pos++] = c;
		}
	}
}

void print_uart(char *buf, uint8_t msg_len)
{
	for (int i = 0; i < msg_len; i++) {
		uart_poll_out(uart_dev, buf[i]);
	}
}

enum State {
	NONE,
	BUCK,
	BOOST,
	ERROR
} state;

void buckboost(void)
{
	int ret = 0;
	float drive = 0.8;
	float vout = 0;
	char uartbuf[64] = {};
	bool powered = false;
	uint8_t errors = 0;
	int countdown = 0;
	Msg msg_out = {.vout = 0, .vbat = 0};
	Msg msg_in = {0};

	k_msgq_init(&msgq, msgq_buffer, sizeof(struct Msg), 2);
	printk("~~~ Protectli UPS ~~~\n");

	HwErrors hw_errors;
	Pid buck_pid(12.0, 0.03, 0.0001, 0.0);

	Battery battery = Battery().set_voltage(16.8).set_current(1000.0);

	gpio_pin_configure_dt(&pwm_en, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&pack_boot, GPIO_OUTPUT);
	gpio_pin_configure_dt(&pwm_skip, GPIO_OUTPUT_ACTIVE);
	gpio_pin_configure_dt(&vin_detect, GPIO_INPUT);

	gpio_pin_configure_dt(&gpio2, GPIO_DISCONNECTED);
	gpio_pin_configure_dt(&gpio3, GPIO_DISCONNECTED);

	k_sleep(K_MSEC(1000));

	Adc adc;
	adc.sample_all();

	if (!device_is_ready(pwm.dev)) {
		printk("Error: PWM device %s is not ready\n", pwm.dev->name);
	}

	if (!device_is_ready(uart_dev)) {
		printk("UART device not found!");
	}

	/* configure interrupt and callback to receive data */
	uart_irq_callback_user_data_set(uart_dev, serial_cb, NULL);
	uart_irq_rx_enable(uart_dev);

	state = NONE;

	adc.sample_all();
	drive = (float)adc.get_vout() / (float)adc.get_vbat(); 
	printk("Initial Drive: %f, %d, %d!\n", drive, adc.get_vout(), adc.get_vbat());

	while (true) {
		adc.sample_all();
		powered = gpio_pin_get_dt(&vin_detect);
		errors = hw_errors.check();

		if (!countdown--) {
			countdown = 1000;
			print_voltages(adc, drive);

			msg_out.vout = adc.get_vout();
			msg_out.vbat = adc.get_vbat();
			msg_out.iout = adc.get_iout();
			msg_out.ibat = adc.get_ibat();
			msg_out.gas = (adc.get_vbat() - 12000) / 48;
			ret = msg_cobs_encode(msg_out, uartbuf);
			print_uart(uartbuf, ret);

			ret = k_msgq_get(&msgq, &msg_in, K_MSEC(10));
			if (ret != 0) {
				if (msg_in.power_dwn) {
					printk("Power Down Requested @ \n");
					msg_in.power_dwn = false;
				}
			}
		}

		// Buck State
		if (!errors && !powered) {
			if (state != BUCK) {
				state = BUCK;
				printk("Entering Buck State\n");
				k_sleep(K_MSEC(5U));
				msg_out.state = MSG_STATE_DISCHARGING;
			}
			vout = adc.get_vout();
			vout = vout / 1000;
			buck_pid.compute(vout);
			drive = buck_pid.get_duc();

			gpio_pin_set_dt(&pwm_en, true);
			pwm_set_dt(&pwm, PERIOD, PERIOD * drive);
		}

		// Boost State (Charging)
		else if (!errors && powered) {
			if (state != BOOST) {
				state = BOOST;
				printk("Entering Boost State\n");
				k_sleep(K_MSEC(5U));
				msg_out.state = MSG_STATE_CHARGING;
#if defined(CONFIG_FORCE_PACK)
				gpio_pin_set_dt(&pack_boot, true);
				k_sleep(K_MSEC(100U));
				gpio_pin_set_dt(&pack_boot, false);
#endif
			}

			// Reduce charge current near the end of CC
			// cycle.
			if (adc.get_vbat() > 16500) {
				battery.set_scaling(0.3);
			} else if (adc.get_vbat() > 16600) {
				battery.set_scaling(0.1);
			} else {
				battery.set_scaling(1);
			}

			drive = battery.compute_drive(adc.get_vbat(),
						      adc.get_ibat(), drive);

			pwm_set_dt(&pwm, PERIOD, PERIOD * drive);
			gpio_pin_set_dt(&pwm_en, true);
		}
		// Error State
		else {
			if (state != ERROR) {
				state = ERROR;
				msg_out.state = MSG_STATE_ERROR;
				printk("Entering Error State\n");
			}
			gpio_pin_set_dt(&pwm_en, false);
			hw_errors.clear();
			state = NONE;
		}
	}
}

K_THREAD_DEFINE(buckboost_id, STACKSIZE, buckboost, NULL, NULL, NULL, 0, 0, 0);
