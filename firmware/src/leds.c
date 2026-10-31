#include "leds.h"

#include <zephyr/drivers/led_strip.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

LOG_MODULE_REGISTER(leds, LOG_LEVEL_INF);

static const struct device *const strip = DEVICE_DT_GET(DT_ALIAS(led_strip));
static struct led_rgb pixels[LED_COUNT];
static atomic_t brightness;
static atomic_t max_level = ATOMIC_INIT((LEDS_MAX_PERCENT_DEFAULT * 255 + 50) / 100);
static atomic_t theme;

/* Color of LED_SOURCE and LED_SOURCE_2, then of LED_J1 and LED_J2 */
static const uint32_t themes[LEDS_THEME_COUNT][2] = {
	{0xff8000, 0xff00ff},
	{0x0000ff, 0x00ffff},
	{0x00ff00, 0x80ff00},
	{0xff8000, 0xfff000},
};

void leds_set_theme(uint8_t t)
{
	if (t < LEDS_THEME_COUNT) {
		atomic_set(&theme, t);
	}
}

uint8_t leds_theme(void)
{
	return atomic_get(&theme);
}

uint32_t leds_color(enum led led)
{
	bool source = led == LED_SOURCE || led == LED_SOURCE_2;

	return themes[atomic_get(&theme)][source ? 0 : 1];
}

void leds_set_max_percent(uint8_t percent)
{
	atomic_set(&max_level, (percent * 255 + 50) / 100);
}

uint8_t leds_max_level(void)
{
	return atomic_get(&max_level);
}

/* Picked up by the next leds_set(), which the main loop calls every 20 ms */
void leds_set_brightness(uint8_t level)
{
	atomic_set(&brightness, level);
}

void leds_set(enum led led, uint32_t rgb)
{
	uint32_t level = atomic_get(&brightness);
	struct led_rgb px = {
		.r = (rgb >> 16 & 0xff) * level / 255,
		.g = (rgb >> 8 & 0xff) * level / 255,
		.b = (rgb & 0xff) * level / 255,
	};
	int err;

	if (px.r == pixels[led].r && px.g == pixels[led].g && px.b == pixels[led].b) {
		return;
	}

	pixels[led] = px;

	err = led_strip_update_rgb(strip, pixels, LED_COUNT);
	if (err) {
		LOG_WRN("Update failed (%d)", err);
	}
}

int leds_init(void)
{
	if (!device_is_ready(strip)) {
		return -ENODEV;
	}

	return led_strip_update_rgb(strip, pixels, LED_COUNT);
}
