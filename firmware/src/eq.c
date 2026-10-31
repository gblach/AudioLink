#include "eq.h"

#include <math.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "audio_out.h"

LOG_MODULE_REGISTER(eq, LOG_LEVEL_INF);

#define CHANNELS    2
#define Q           0.707f
/* M_PI is not defined in strict C17 */
#define PI          3.14159265f
/* Largest float below 2^31, INT32_MAX itself rounds up and overflows */
#define SAMPLE_MAX  2147483520.0f

enum band_type {
	LOW_SHELF,
	PEAK,
	HIGH_SHELF,
};

static const struct {
	enum band_type type;
	float freq;
} bands[EQ_BANDS] = {
	{LOW_SHELF, 60}, {PEAK, 250}, {PEAK, 1000}, {PEAK, 4000}, {HIGH_SHELF, 12000},
};

struct coefs {
	float b0, b1, b2, a1, a2;
};

struct eq_cfg {
	struct coefs band[EQ_BANDS];
	bool active;
};

static struct eq_cfg pending;
static bool pending_set;
static struct k_spinlock lock;

/* Owned by the audio_out feeder thread */
static struct eq_cfg cfg;
static float state[CHANNELS][EQ_BANDS][2];

/* Audio EQ Cookbook (R. Bristow-Johnson) */
static struct coefs band_coefs(enum band_type type, float freq, int gain_db)
{
	float a = powf(10.0f, gain_db / 40.0f);
	float w0 = 2.0f * PI * freq / AUDIO_OUT_RATE_HZ;
	float cw = cosf(w0);
	float alpha = sinf(w0) / (2.0f * Q);
	float sa = 2.0f * sqrtf(a) * alpha;
	float b0, b1, b2, a0, a1, a2;

	switch (type) {
	case LOW_SHELF:
		b0 = a * ((a + 1) - (a - 1) * cw + sa);
		b1 = 2 * a * ((a - 1) - (a + 1) * cw);
		b2 = a * ((a + 1) - (a - 1) * cw - sa);
		a0 = (a + 1) + (a - 1) * cw + sa;
		a1 = -2 * ((a - 1) + (a + 1) * cw);
		a2 = (a + 1) + (a - 1) * cw - sa;
		break;
	case HIGH_SHELF:
		b0 = a * ((a + 1) + (a - 1) * cw + sa);
		b1 = -2 * a * ((a - 1) + (a + 1) * cw);
		b2 = a * ((a + 1) + (a - 1) * cw - sa);
		a0 = (a + 1) - (a - 1) * cw + sa;
		a1 = 2 * ((a - 1) - (a + 1) * cw);
		a2 = (a + 1) - (a - 1) * cw - sa;
		break;
	default:
		b0 = 1 + alpha * a;
		b1 = -2 * cw;
		b2 = 1 - alpha * a;
		a0 = 1 + alpha / a;
		a1 = -2 * cw;
		a2 = 1 - alpha / a;
		break;
	}

	return (struct coefs){b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

void eq_set_gains(const int8_t *gains)
{
	struct eq_cfg next = {0};

	for (int i = 0; i < EQ_BANDS; i++) {
		next.band[i] = band_coefs(bands[i].type, bands[i].freq, gains[i]);
		next.active |= gains[i] != 0;
	}

	K_SPINLOCK(&lock) {
		pending = next;
		pending_set = true;
	}

	LOG_INF("Gains %d %d %d %d %d dB", gains[0], gains[1], gains[2], gains[3], gains[4]);
}

void eq_process(int32_t *samples, size_t frames)
{
	K_SPINLOCK(&lock) {
		if (pending_set) {
			/* State left over from before a bypass would play as a click */
			if (!cfg.active) {
				memset(state, 0, sizeof(state));
			}
			cfg = pending;
			pending_set = false;
		}
	}

	if (!cfg.active) {
		return;
	}

	for (size_t i = 0; i < frames * CHANNELS; i++) {
		float (*z)[2] = state[i % CHANNELS];
		float y = samples[i];

		for (int b = 0; b < EQ_BANDS; b++) {
			const struct coefs *c = &cfg.band[b];
			float x = y;

			y = c->b0 * x + z[b][0];
			z[b][0] = c->b1 * x - c->a1 * y + z[b][1];
			z[b][1] = c->b2 * x - c->a2 * y;
		}

		samples[i] = (int32_t)CLAMP(y, (float)INT32_MIN, SAMPLE_MAX);
	}
}
