#include "test_signal.h"

#include <math.h>

#include "audio_out.h"

#define BURST_ON_FRAMES (AUDIO_OUT_RATE_HZ / 5)
#define BURST_FRAMES    AUDIO_OUT_RATE_HZ
/* A whole number of bursts, so wrapping the frame counter keeps the burst rhythm */
#define SWEEP_FRAMES    (10 * BURST_FRAMES)
#define LEVEL_MIN_DB    -40.0f
#define LEVEL_MAX_DB    -6.0f
/* The pink filter output peaks a little under 8 for white noise in -1 to 1 */
#define PINK_PEAK       8.0f

static uint32_t frame;
static uint32_t rng = 1;

static float white(void)
{
	/* xorshift32 */
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;

	return (int32_t)rng / 2147483648.0f;
}

/* Paul Kellet's economy filter, white noise shaped to -3 dB per octave */
static float pink(void)
{
	static float b0, b1, b2;
	float w = white();

	b0 = 0.99765f * b0 + w * 0.0990460f;
	b1 = 0.96300f * b1 + w * 0.2965164f;
	b2 = 0.57000f * b2 + w * 1.0526913f;

	return (b0 + b1 + b2 + w * 0.1848f) / PINK_PEAK;
}

/* Triangle in dB, so the sweep sounds even instead of jumping at the quiet end */
static float sweep_gain(void)
{
	float x = 2.0f * frame / SWEEP_FRAMES;
	float tri = x < 1.0f ? x : 2.0f - x;
	float db = LEVEL_MIN_DB + tri * (LEVEL_MAX_DB - LEVEL_MIN_DB);

	return powf(10.0f, db / 20.0f);
}

void test_signal_fill(int16_t *pcm, size_t frames)
{
	float gain = sweep_gain() * INT16_MAX;

	for (size_t i = 0; i < frames; i++) {
		int16_t s = frame % BURST_FRAMES < BURST_ON_FRAMES ? pink() * gain : 0;

		pcm[2 * i] = s;
		pcm[2 * i + 1] = s;
		frame = (frame + 1) % SWEEP_FRAMES;
	}
}
