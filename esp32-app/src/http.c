#include <zephyr/net/net_ip.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>

#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/conn_mgr.h>

#include <stdio.h>

#include "http.h"

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

static void process_tcp4(void);

K_THREAD_DEFINE(tcp4_thread_id, STACK_SIZE, process_tcp4, NULL, NULL, NULL,
		THREAD_PRIORITY, 0, -1);

static const char index_content[] = {
#include "index.html.bin.inc"
};

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
		printk("Network connected\n");

		connected = true;
		k_sem_give(&run_app);

		return;
	}

	if (mgmt_event == NET_EVENT_L4_DISCONNECTED) {
		if (connected == false) {
			printk("Waiting network to be connected");
		} else {
			printk("Network disconnected");
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

static int setup(int *sock, struct sockaddr *bind_addr, socklen_t bind_addrlen)
{
	int ret;

	*sock = socket(bind_addr->sa_family, SOCK_STREAM, IPPROTO_TCP);
	if (*sock < 0) {
		printk("Failed to create TCP socket: %d", errno);
		return -errno;
	}

	ret = bind(*sock, bind_addr, bind_addrlen);
	if (ret < 0) {
		printk("Failed to bind TCP socket %d", errno);
		return -errno;
	}

	ret = listen(*sock, MAX_CLIENT_QUEUE);
	if (ret < 0) {
		printk("Failed to listen on TCP socket %d", errno);
		ret = -errno;
	}

	return ret;
}

void send_response(int socket, const char *header, const char *content_type,
		   const char *body)
{
	char response[1024];

	int len = sprintf(response, "%sContent-Type: %s\n\n%s", header, content_type,
		body);

	if(len) {
		sendall(socket, response, len);
	} else {
		printk("sprintf Returned: %d", len);
	}
}

void handle_get_request(int client_fd, const char *path)
{
	size_t fileSize;
	if (strcmp(path, "/") == 0) {
		printk("Got / request");
		sendall(client_fd, index_content, sizeof(index_content));
	} else if (strcmp(path, "/hello") == 0) {
		send_response(client_fd, "HTTP/1.1 200 OK\n", "text/html",
			      "<html><body><h1>Hello, World from "
			      "/hello!</h1></body></html>");
	} else {
		send_response(
			client_fd, "HTTP/1.1 404 Not Found\n", "text/html",
			"<html><body><h1>404 Not Found</h1></body></html>");
	}
}

void handle_post_request(int client_fd, const char *path, const char *body)
{
	if (strcmp(path, "/submit") == 0) {
		printk("Got /submit POST: %s", body);
		send_response(client_fd, "HTTP/1.1 200 OK\n", "text/plain",
			      body);
	} else {
		send_response(
			client_fd, "HTTP/1.1 404 Not Found\n", "text/html",
			"<html><body><h1>404 Not Found</h1></body></html>");
	}
}

static void client_conn_handler(void *ptr1, void *ptr2, void *ptr3)
{
	ARG_UNUSED(ptr1);
	int *sock = ptr2;
	k_tid_t *in_use = ptr3;
	int client;
	int received;
	int ret;
	char buf[256];
	char method[16], path[256], *body;

	client = *sock;
	received = recv(client, buf, sizeof(buf), 0);
	sscanf(buf, "%s %s", method, path);

	if (received == 0) {
		/* Connection closed */
		printk("[%d] Connection closed by peer", client);
	} else if (received < 0) {
		/* Socket error */
		ret = -errno;
		printk("[%d] Connection error %d", client, ret);
	}

	body = strstr(buf, "\r\n\r\n");

	/* We received status from the client */
	if (strstr(buf, "\r\n\r\nOK")) {
		running_status = true;
		want_to_quit = true;
		k_sem_give(&quit_lock);
	} else if (strstr(buf, "\r\n\r\nFAIL")) {
		running_status = false;
		want_to_quit = true;
		k_sem_give(&quit_lock);
	} else if (strcmp(method, "GET") == 0) {
		handle_get_request(client, path);
	} else if (strcmp(method, "POST") == 0) {
		handle_post_request(client, path, body);
	} else {
		send_response(client, "HTTP/1.1 405 Method Not Allowed\n",
			      "text/html",
			      "<html><body><h1>405 Method Not "
			      "Allowed</h1></body></html>");
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
		printk("Error in accept %d, stopping server", -errno);
		return -errno;
	}

	slot = get_free_slot(accepted);
	if (slot < 0 || slot >= CONFIG_NET_SAMPLE_NUM_HANDLERS) {
		printk("Cannot accept more connections");
		close(client);
		return 0;
	}

	accepted[slot] = client;

	if (client_addr.sin6_family == AF_INET) {
		tcp4_handler_tid[slot] = k_thread_create(
			&tcp4_handler_thread[slot], tcp4_handler_stack[slot],
			K_THREAD_STACK_SIZEOF(tcp4_handler_stack[slot]),
			(k_thread_entry_t)client_conn_handler,
			INT_TO_POINTER(slot), &accepted[slot],
			&tcp4_handler_tid[slot], THREAD_PRIORITY, 0, K_NO_WAIT);
	}

	char addr_str[INET6_ADDRSTRLEN];

	net_addr_ntop(client_addr.sin6_family, &client_addr.sin6_addr, addr_str,
		      sizeof(addr_str));

	printk("[%d] Connection #%d from %s", client, ++counter, addr_str);

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

	printk("Waiting for IPv4 HTTP connections on port %d, sock %d\n", MY_PORT,
	       tcp4_listen_sock);

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

void net_init(void)
{
	if (IS_ENABLED(CONFIG_NET_CONNECTION_MANAGER)) {
		net_mgmt_init_event_callback(&mgmt_cb, event_handler,
					     EVENT_MASK);
		net_mgmt_add_event_callback(&mgmt_cb);

		conn_mgr_resend_status();
	}

	/* Wait for the connection. */
	k_sem_take(&run_app, K_FOREVER);
	/* I think this might block waiting for net
	we probably do _not_ want this */
}
