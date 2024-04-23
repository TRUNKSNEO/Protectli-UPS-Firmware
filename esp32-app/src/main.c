#include <stdio.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>

#include "msg.h"
#include "screen.h"
#include "http.h"

#define SLEEP_TIME_MS 5000
#define LED0_NODE     DT_ALIAS(led0)

#define MSG_SIZE 32

K_MSGQ_DEFINE(uart_msgq, MSG_SIZE, 10, 4);

static struct k_poll_signal spi_slave_done_sig =
	K_POLL_SIGNAL_INITIALIZER(spi_slave_done_sig);

static const struct gpio_dt_spec led =
	GPIO_DT_SPEC_GET(DT_NODELABEL(led0), gpios);

static const struct gpio_dt_spec pb =
	GPIO_DT_SPEC_GET(DT_NODELABEL(button), gpios);

static const struct gpio_dt_spec relay1 =
	GPIO_DT_SPEC_GET(DT_NODELABEL(relay1), gpios);

static const struct gpio_dt_spec relay2 =
	GPIO_DT_SPEC_GET(DT_NODELABEL(relay2), gpios);

static const struct device *const uart_dev =
	DEVICE_DT_GET(DT_CHOSEN(zephyr_mcu_uart));

/* receive buffer used in UART ISR callback */
static char rx_buf[MSG_SIZE];
static int rx_buf_pos = 0;

K_THREAD_STACK_DEFINE(screen_thd_stack_area, SCREEN_THD_STACK_SIZE);
struct k_thread screen_thd_data;

uint8_t pb_re = false;

void print_uart(char *buf, uint8_t msg_len)
{
	for (int i = 0; i < msg_len; i++) {
		uart_poll_out(uart_dev, buf[i]);
	}
}

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
			while (k_msgq_put(&uart_msgq, &msg, K_NO_WAIT) != 0) {
				k_msgq_purge(&uart_msgq);
			}
			rx_buf_pos = 0;
		} else if (c) {
			rx_buf[rx_buf_pos++] = c;
		}
	}
}

int main(void)
{
	int ret;
	char tx_buf[MSG_SIZE];
	char uartbuf[64] = {};
	struct Msg msg;

	printf("Starting Up!\n");

	if (!gpio_is_ready_dt(&led)) {
		return 0;
	}

	gpio_pin_configure_dt(&pb, GPIO_INPUT);
	gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
	gpio_pin_configure_dt(&relay1, GPIO_OUTPUT_ACTIVE);
	gpio_pin_configure_dt(&relay2, GPIO_OUTPUT_ACTIVE);

	if (!device_is_ready(uart_dev)) {
		printk("UART device not found!");
		return 0;
	}

	screen_init();

	/* configure interrupt and callback to receive data */
	ret = uart_irq_callback_user_data_set(uart_dev, serial_cb, NULL);
	uart_irq_rx_enable(uart_dev);

	k_tid_t my_tid = k_thread_create(
		&screen_thd_data, screen_thd_stack_area,
		K_THREAD_STACK_SIZEOF(screen_thd_stack_area), screen_thread,
		NULL, NULL, NULL, SCREEN_THD_PRIORITY, 0, K_NO_WAIT);

	// net_init();
	// start_listener();
	// k_sem_take(&quit_lock, K_FOREVER);

	printk("Init Complete\n");
	int pwr_dwn = 10;
	bool prev_pb_state = gpio_pin_get_dt(&pb);
	while (1) {
		if (gpio_pin_get_dt(&pb)) {
			if (!pwr_dwn--) {
				printk("Power Down Requested");
				memset(&msg, 0x00, sizeof(msg));
				msg.power_dwn = true;
				ret = msg_cobs_encode(msg, uartbuf);
				print_uart(uartbuf, ret);
				pwr_dwn = 10;
			}
		}

		if (gpio_pin_get_dt(&pb) && (prev_pb_state == false)) {
			// Rising Edge
			prev_pb_state = gpio_pin_get_dt(&pb);
			pb_re = true;
			gpio_pin_toggle_dt(&led);
		} else if (!gpio_pin_get_dt(&pb) && (prev_pb_state == true)) {
			// Falling Edge
			prev_pb_state = gpio_pin_get_dt(&pb);
		}

		k_sleep(K_MSEC(100U));
	}
	return 0;
}
