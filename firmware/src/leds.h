#ifndef LEDS_H_
#define LEDS_H_

#include <stdint.h>

/* Chain order. LED_J1 and LED_J2 sit next to their jacks. */
enum led {
	LED_SOURCE,
	LED_J1,
	LED_SOURCE_2,
	LED_J2,
	LED_COUNT,
};

int leds_init(void);

/* 0 amber/magenta, 1 blue/cyan, 2 green/lime, 3 amber/yellow */
#define LEDS_THEME_COUNT 4

void leds_set_theme(uint8_t theme);

uint8_t leds_theme(void);

/* The LED's color in the current theme, 0xRRGGBB at full scale, dimmed when shown */
uint32_t leds_color(enum led led);

#define LEDS_MAX_PERCENT_DEFAULT 50

/* Upper limit for leds_set_brightness(), 0 to 100 % of full drive */
void leds_set_max_percent(uint8_t percent);

/* The limit as a drive level, 255 is full */
uint8_t leds_max_level(void);

/* WS2812 drive level, 255 is full */
void leds_set_brightness(uint8_t level);

/* Only writes the chain when a color actually changed */
void leds_set(enum led led, uint32_t rgb);

#endif /* LEDS_H_ */
