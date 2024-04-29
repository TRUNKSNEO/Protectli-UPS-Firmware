#include "uart.h"

static bool shutdown = false;

void uart1_out(char *data)
{
	while (*data) {
		usart_send_blocking(USART1, *data++);
	}
}

void usart1_setup(int baud)
{
	SYSCFG_CFGR1 |= SYSCFG_CFGR1_PA11_RMP;
	gpio_mode_setup(GPIOA, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO9);
	gpio_set_af(GPIOA, GPIO_AF1, GPIO9);

	/* Setup USART parameters. */
	usart_set_baudrate(USART1, baud);
	usart_set_databits(USART1, 8);
	usart_set_parity(USART1, USART_PARITY_NONE);
	usart_set_stopbits(USART1, USART_CR2_STOPBITS_1);
	usart_set_mode(USART1, USART_MODE_TX);
	usart_set_flow_control(USART1, USART_FLOWCONTROL_NONE);

	/* Finally enable the USART. */
	usart_enable(USART1);
}

bool uart_get_shutdown(void)
{
	return shutdown;
}

void usart2_lpuart2_isr(void)
{
	static uint8_t data = 0x00;
	data = usart_recv(USART2);
	usart_send(USART1, data);

	// @ is the special shutdown char
	if (data == '@') {
		shutdown = true;
	}
}

void usart2_setup(int baud)
{
	nvic_enable_irq(NVIC_USART2_LPUART2_IRQ);

	gpio_mode_setup(GPIOA, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO3);
	gpio_set_af(GPIOA, GPIO_AF1, GPIO3);

	/* Setup USART parameters. */
	usart_set_baudrate(USART2, baud);
	usart_set_databits(USART2, 8);
	usart_set_parity(USART2, USART_PARITY_NONE);
	usart_set_stopbits(USART2, USART_CR2_STOPBITS_1);
	usart_set_mode(USART2, USART_MODE_RX);
	usart_set_flow_control(USART2, USART_FLOWCONTROL_NONE);

	usart_enable_rx_interrupt(USART2);

	/* Finally enable the USART. */
	usart_enable(USART2);
}
