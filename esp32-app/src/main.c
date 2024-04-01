#include <zephyr/logging/log.h>
#define LOG_LEVEL LOG_LEVEL_DBG
LOG_MODULE_REGISTER(net_dumb_http_srv_mt_sample);

#include <stdio.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>

#include <zephyr/net/net_ip.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>

#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/conn_mgr.h>

#include "msg.h"
#include "screen.h"

#define SLEEP_TIME_MS 5000
#define LED0_NODE     DT_ALIAS(led0)

#define MSG_SIZE 32
#define MY_PORT 8080

static const char content[] = {
#include "response.html.bin.inc"
};

#define CONFIG_NET_SAMPLE_NUM_HANDLERS 2 
#define STACK_SIZE 4096
#define THREAD_PRIORITY K_PRIO_PREEMPT(8)

#define EVENT_MASK (NET_EVENT_L4_CONNECTED | \
		    NET_EVENT_L4_DISCONNECTED)


#define MAX_CLIENT_QUEUE CONFIG_NET_SAMPLE_NUM_HANDLERS


K_THREAD_STACK_ARRAY_DEFINE(tcp4_handler_stack, CONFIG_NET_SAMPLE_NUM_HANDLERS,
			    STACK_SIZE);
static struct k_thread tcp4_handler_thread[CONFIG_NET_SAMPLE_NUM_HANDLERS];
static k_tid_t tcp4_handler_tid[CONFIG_NET_SAMPLE_NUM_HANDLERS];

static struct net_mgmt_event_callback mgmt_cb;
static bool connected;
K_SEM_DEFINE(run_app, 0, 1);
K_SEM_DEFINE(quit_lock, 0, 1);
static bool running_status;
static bool want_to_quit;
static int tcp4_listen_sock;
static int tcp4_accepted[CONFIG_NET_SAMPLE_NUM_HANDLERS];
static int tcp6_listen_sock;
static int tcp6_accepted[CONFIG_NET_SAMPLE_NUM_HANDLERS];

static void process_tcp4(void);

K_THREAD_DEFINE(tcp4_thread_id, STACK_SIZE,
		process_tcp4, NULL, NULL, NULL,
		THREAD_PRIORITY, 0, -1);

K_MSGQ_DEFINE(uart_msgq, MSG_SIZE, 10, 4);

static struct k_poll_signal spi_slave_done_sig =
	K_POLL_SIGNAL_INITIALIZER(spi_slave_done_sig);

static const struct gpio_dt_spec led =
	GPIO_DT_SPEC_GET(DT_NODELABEL(led0), gpios);

static const struct gpio_dt_spec relay1 =
	GPIO_DT_SPEC_GET(DT_NODELABEL(relay1), gpios);

static const struct gpio_dt_spec relay2 =
	GPIO_DT_SPEC_GET(DT_NODELABEL(relay2), gpios);

static const struct device *const uart_dev =
	DEVICE_DT_GET(DT_CHOSEN(zephyr_mcu_uart));

/* receive buffer used in UART ISR callback */
static char rx_buf[MSG_SIZE];
static int rx_buf_pos = 0;

struct k_msgq msgq;
char msgq_buffer[sizeof(struct Msg) * 2];

K_THREAD_STACK_DEFINE(screen_thd_stack_area, SCREEN_THD_STACK_SIZE);
struct k_thread screen_thd_data;

static void event_handler(struct net_mgmt_event_callback *cb,
			  uint32_t mgmt_event, struct net_if *iface)
{
	if ((mgmt_event & EVENT_MASK) != mgmt_event) {
		return;
	}

	if (want_to_quit) {
		k_sem_give(&run_app);
		want_to_quit = false;
	}

	if (mgmt_event == NET_EVENT_L4_CONNECTED) {
		LOG_INF("Network connected");

		connected = true;
		k_sem_give(&run_app);

		return;
	}

	if (mgmt_event == NET_EVENT_L4_DISCONNECTED) {
		if (connected == false) {
			LOG_INF("Waiting network to be connected");
		} else {
			LOG_INF("Network disconnected");
			connected = false;
		}

		k_sem_reset(&run_app);

		return;
	}
}

static ssize_t sendall(int sock, const void *buf, size_t len)
{
	while (len) {
		ssize_t out_len = send(sock, buf, len, 0);

		if (out_len < 0) {
			return out_len;
		}

		buf = (const char *)buf + out_len;
		len -= out_len;
	}

	return 0;
}

static int setup(int *sock, struct sockaddr *bind_addr,
		 socklen_t bind_addrlen)
{
	int ret;

#if defined(CONFIG_NET_SOCKETS_SOCKOPT_TLS)
	*sock = socket(bind_addr->sa_family, SOCK_STREAM, IPPROTO_TLS_1_2);
#else
	*sock = socket(bind_addr->sa_family, SOCK_STREAM, IPPROTO_TCP);
#endif
	if (*sock < 0) {
		LOG_ERR("Failed to create TCP socket: %d", errno);
		return -errno;
	}

#if defined(CONFIG_NET_SOCKETS_SOCKOPT_TLS)
	sec_tag_t sec_tag_list[] = {
		SERVER_CERTIFICATE_TAG,
	};

	ret = setsockopt(*sock, SOL_TLS, TLS_SEC_TAG_LIST,
			 sec_tag_list, sizeof(sec_tag_list));
	if (ret < 0) {
		LOG_ERR("Failed to set TCP secure option %d", errno);
		ret = -errno;
	}
#endif

	ret = bind(*sock, bind_addr, bind_addrlen);
	if (ret < 0) {
		LOG_ERR("Failed to bind TCP socket %d", errno);
		return -errno;
	}

	ret = listen(*sock, MAX_CLIENT_QUEUE);
	if (ret < 0) {
		LOG_ERR("Failed to listen on TCP socket %d", errno);
		ret = -errno;
	}

	return ret;
}

static void client_conn_handler(void *ptr1, void *ptr2, void *ptr3)
{
	ARG_UNUSED(ptr1);
	int *sock = ptr2;
	k_tid_t *in_use = ptr3;
	int client;
	int received;
	int ret;
	char buf[100];

	client = *sock;

	/* Discard HTTP request (or otherwise client will get
	 * connection reset error).
	 */
	do {
		received = recv(client, buf, sizeof(buf), 0);
		if (received == 0) {
			/* Connection closed */
			LOG_DBG("[%d] Connection closed by peer", client);
			break;
		} else if (received < 0) {
			/* Socket error */
			ret = -errno;
			LOG_ERR("[%d] Connection error %d", client, ret);
			break;
		}

		/* Note that something like this strstr() check should *NOT*
		 * be used in production code. This is done like this just
		 * for this sample application to keep things simple.
		 *
		 * We are assuming here that the full HTTP request is received
		 * in one TCP segment which in real life might not.
		 */
		if (strstr(buf, "\r\n\r\n")) {
			break;
		}
	} while (true);

	/* We received status from the client */
	if (strstr(buf, "\r\n\r\nOK")) {
		running_status = true;
		want_to_quit = true;
		k_sem_give(&quit_lock);
	} else if (strstr(buf, "\r\n\r\nFAIL")) {
		running_status = false;
		want_to_quit = true;
		k_sem_give(&quit_lock);
	} else {
		(void)sendall(client, content, sizeof(content));
	}

	(void)close(client);

	*sock = -1;
	*in_use = NULL;
}

static int get_free_slot(int *accepted)
{
	int i;

	for (i = 0; i < CONFIG_NET_SAMPLE_NUM_HANDLERS; i++) {
		if (accepted[i] < 0) {
			return i;
		}
	}

	return -1;
}

static int process_tcp(int *sock, int *accepted)
{
	static int counter;
	int client;
	int slot;
	struct sockaddr_in6 client_addr;
	socklen_t client_addr_len = sizeof(client_addr);

	client = accept(*sock, (struct sockaddr *)&client_addr,
			&client_addr_len);
	if (client < 0) {
		LOG_ERR("Error in accept %d, stopping server", -errno);
		return -errno;
	}

	slot = get_free_slot(accepted);
	if (slot < 0 || slot >= CONFIG_NET_SAMPLE_NUM_HANDLERS) {
		LOG_ERR("Cannot accept more connections");
		close(client);
		return 0;
	}

	accepted[slot] = client;

	if (client_addr.sin6_family == AF_INET) {
		tcp4_handler_tid[slot] = k_thread_create(
			&tcp4_handler_thread[slot],
			tcp4_handler_stack[slot],
			K_THREAD_STACK_SIZEOF(tcp4_handler_stack[slot]),
			(k_thread_entry_t)client_conn_handler,
			INT_TO_POINTER(slot),
			&accepted[slot],
			&tcp4_handler_tid[slot],
			THREAD_PRIORITY,
			0, K_NO_WAIT);
	}

	if (LOG_LEVEL >= LOG_LEVEL_DBG) {
		char addr_str[INET6_ADDRSTRLEN];

		net_addr_ntop(client_addr.sin6_family,
			      &client_addr.sin6_addr,
			      addr_str, sizeof(addr_str));

		LOG_DBG("[%d] Connection #%d from %s",
			client, ++counter,
			addr_str);
	}

	return 0;
}

static void process_tcp4(void)
{
	struct sockaddr_in addr4;
	int ret;

	(void)memset(&addr4, 0, sizeof(addr4));
	addr4.sin_family = AF_INET;
	addr4.sin_port = htons(MY_PORT);

	ret = setup(&tcp4_listen_sock, (struct sockaddr *)&addr4,
		    sizeof(addr4));
	if (ret < 0) {
		return;
	}

	LOG_DBG("Waiting for IPv4 HTTP connections on port %d, sock %d",
		MY_PORT, tcp4_listen_sock);

	while (ret == 0 || !want_to_quit) {
		ret = process_tcp(&tcp4_listen_sock, tcp4_accepted);
		if (ret < 0) {
			return;
		}
	}
}

void start_listener(void)
{
	int i;

	for (i = 0; i < CONFIG_NET_SAMPLE_NUM_HANDLERS; i++) {
		tcp4_accepted[i] = -1;
		tcp4_listen_sock = -1;
	}

	if (IS_ENABLED(CONFIG_NET_IPV4)) {
		k_thread_start(tcp4_thread_id);
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
			while (k_msgq_put(&msgq, &msg, K_NO_WAIT) != 0) {
				k_msgq_purge(&msgq);
			}
			rx_buf_pos = 0;
		} else if (c) {
			rx_buf[rx_buf_pos++] = c;
		}
	}
}

void print_uart(char *buf)
{
	int msg_len = strlen(buf);

	for (int i = 0; i < msg_len; i++) {
		uart_poll_out(uart_dev, buf[i]);
	}
}

int main(void)
{
	int ret;
	char tx_buf[MSG_SIZE];

	printf("Starting Up!\n");

	if (!gpio_is_ready_dt(&led)) {
		return 0;
	}

	k_msgq_init(&msgq, msgq_buffer, sizeof(struct Msg), 2);

	gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&relay1, GPIO_OUTPUT_ACTIVE);

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


	if (IS_ENABLED(CONFIG_NET_CONNECTION_MANAGER)) {
		net_mgmt_init_event_callback(&mgmt_cb,
					     event_handler, EVENT_MASK);
		net_mgmt_add_event_callback(&mgmt_cb);

		conn_mgr_resend_status();
	}

	if (!IS_ENABLED(CONFIG_NET_CONNECTION_MANAGER)) {
		/* If the config library has not been configured to start the
		 * app only after we have a connection, then we can start
		 * it right away.
		 */
		k_sem_give(&run_app);
	}

	/* Wait for the connection. */
	k_sem_take(&run_app, K_FOREVER);

	start_listener();

	k_sem_take(&quit_lock, K_FOREVER);

	printk("Init Complete");
	while (1) {
		gpio_pin_toggle_dt(&led);
		k_sleep(K_MSEC(100U));
	}
	return 0;
}
