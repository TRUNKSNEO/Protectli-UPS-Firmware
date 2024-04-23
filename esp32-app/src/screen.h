#pragma once

#define SCREEN_THD_STACK_SIZE 500
#define SCREEN_THD_PRIORITY   5

#define DELAY_LOOPS 10

void screen_thread(void *, void *, void *);
int screen_init(void);

enum screen_state {
	INTRO = 0,
	ERR,
	VBAT,
	VOUT
};
