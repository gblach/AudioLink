#include <math.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <nrfx_clock.h>

#include "audio_out.h"
#include "dac.h"
#include "le_audio.h"
#include "leds.h"
#include "usb_audio.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

#define POLL_MS         20
#define DEBOUNCE_POLLS  3
/* PCM5242 soft mute ramp, so the DG419 switches on silence */
#define MUTE_RAMP_MS    20

/* LED brightness follows the output level in dB, so quiet passages still show */
#define LEVEL_FLOOR_DB  -48.0f
/* Per poll, about 50 dB/s, so the LEDs fade like a VU meter instead of flickering */
#define LEVEL_FALL_DB   1.0f

#define ANALOG DT_NODELABEL(analog)

static const struct gpio_dt_spec mode = GPIO_DT_SPEC_GET(ANALOG, mode_gpios);
static const struct gpio_dt_spec j1_det = GPIO_DT_SPEC_GET(ANALOG, j1_det_gpios);
static const struct gpio_dt_spec j2_det = GPIO_DT_SPEC_GET(ANALOG, j2_det_gpios);

struct jack {
	const struct gpio_dt_spec *det;
	bool inserted;
	int stable_polls;
};

static struct jack j1 = {.det = &j1_det};
static struct jack j2 = {.det = &j2_det};

/* Returns true when the debounced state changed */
static bool jack_poll(struct jack *j)
{
	bool now = gpio_pin_get_dt(j->det) > 0;

	if (now == j->inserted) {
		j->stable_polls = 0;
		return false;
	}

	if (++j->stable_polls < DEBOUNCE_POLLS) {
		return false;
	}

	j->inserted = now;
	j->stable_polls = 0;

	return true;
}

/*
 * With J2 plugged both jacks are balanced outputs, J1 = left, J2 = right.
 * Otherwise J1 carries stereo: tip = left, ring = right.
 */
static void apply_mode(bool balanced)
{
	LOG_INF("Output mode: %s", balanced ? "balanced L/R" : "stereo on J1");

	dac_mute(true);
	k_sleep(K_MSEC(MUTE_RAMP_MS));

	gpio_pin_set_dt(&mode, !balanced);
	/* J2 is wired tip = OUTRN, so the right channel is inverted to keep tip hot */
	audio_out_set_invert_right(balanced);

	dac_mute(false);
}

static uint8_t level_brightness(void)
{
	static float level_db = LEVEL_FLOOR_DB;
	uint32_t peak = audio_out_take_peak();
	float db = peak > 0 ? 20.0f * log10f(peak / 32768.0f) : LEVEL_FLOOR_DB;

	level_db = MAX(MAX(db, LEVEL_FLOOR_DB), level_db - LEVEL_FALL_DB);

	/*
	 * Exponential, since the eye sees equal brightness ratios as equal steps. Offset by one so
	 * the curve can start at zero, which a pure ratio cannot.
	 */
	return powf(leds_max_level() + 1, (level_db - LEVEL_FLOOR_DB) / -LEVEL_FLOOR_DB) - 0.5f;
}

/* USB has no volume control in Zephyr's UAC2 class, the host scales the samples */
static void update_volume(void)
{
	static int applied = -1;
	int vol = audio_out_source() == AUDIO_SRC_BLE ? le_audio_volume() : 255;

	if (vol != applied && dac_set_volume(vol) == 0) {
		applied = vol;
	}
}

static int io_init(void)
{
	int err;

	if (!gpio_is_ready_dt(&mode) || !gpio_is_ready_dt(&j1_det) ||
	    !gpio_is_ready_dt(&j2_det)) {
		return -ENODEV;
	}

	err = gpio_pin_configure_dt(&mode, GPIO_OUTPUT_INACTIVE);
	if (err) {
		return err;
	}

	err = gpio_pin_configure_dt(&j1_det, GPIO_INPUT);
	if (err) {
		return err;
	}

	err = gpio_pin_configure_dt(&j2_det, GPIO_INPUT);
	if (err) {
		return err;
	}

	j1.inserted = gpio_pin_get_dt(&j1_det) > 0;
	j2.inserted = gpio_pin_get_dt(&j2_det) > 0;

	return 0;
}

int main(void)
{
	int err;

	/* LC3 decoding needs the application core at 128 MHz */
	err = nrfx_clock_divider_set(NRF_CLOCK_DOMAIN_HFCLK, NRF_CLOCK_HFCLK_DIV_1);
	if (err) {
		LOG_ERR("HFCLK divider failed (%d)", err);
	}

	err = leds_init();
	if (err) {
		LOG_ERR("LED init failed (%d)", err);
	}

	err = io_init();
	if (err) {
		LOG_ERR("IO init failed (%d)", err);
		return 0;
	}

	/* I2S has to be running first, the PCM5242 PLL locks to BCK */
	err = audio_out_init();
	if (err) {
		LOG_ERR("Audio out init failed (%d)", err);
		return 0;
	}

	err = dac_init();
	if (err) {
		LOG_ERR("DAC init failed (%d)", err);
		return 0;
	}

	apply_mode(j2.inserted);

	err = usb_audio_init();
	if (err) {
		LOG_ERR("USB init failed (%d)", err);
	}

	err = le_audio_init();
	if (err) {
		LOG_ERR("LE Audio init failed (%d)", err);
	}

	for (;;) {
		bool changed = jack_poll(&j1);

		if (jack_poll(&j2)) {
			apply_mode(j2.inserted);
			changed = true;
		}

		if (changed) {
			LOG_INF("J1 %s, J2 %s", j1.inserted ? "in" : "out",
				j2.inserted ? "in" : "out");
		}

		update_volume();
		dac_poll_status();

		leds_set_brightness(level_brightness());
		for (enum led led = 0; led < LED_COUNT; led++) {
			leds_set(led, leds_color(led));
		}

		k_sleep(K_MSEC(POLL_MS));
	}

	return 0;
}
