#include <libopencm3/stm32/usart.h>
#include <libopencm3/stm32/syscfg.h>
#include <libopencm3/stm32/gpio.h>

void usart1_setup(int baud);
void uart1_out(char *data);
void usart2_setup(int baud);
