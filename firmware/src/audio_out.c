#include "audio_out.h"

#include <stdlib.h>

#include <zephyr/drivers/i2s.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/util.h>

#include <hal/nrf_clock.h>

#include "eq.h"

LOG_MODULE_REGISTER(audio_out, LOG_LEVEL_INF);

#define CHANNELS     2
#define BLOCK_FRAMES (AUDIO_OUT_RATE_HZ / 1000)
/*
 * 32-bit I2S words give BCK = 64 fs, a ratio the PCM5242 PLL locks to
 * reliably when BCK is its only clock.
 */
#define BLOCK_BYTES  (BLOCK_FRAMES * CHANNELS * sizeof(int32_t))
#define FRAME_BYTES  (CHANNELS * sizeof(int16_t))
#define RING_FRAMES  4096
#define MS_TO_FRAMES(ms) ((ms) * BLOCK_FRAMES)

/* HFCLKAUDIO FREQ_VALUE for 12.288 MHz, one step is about 3.3 ppm */
#define ACLK_CENTER    0x9BA6
#define ACLK_MAX_STEPS 150
#define DRIFT_PERIOD_BLOCKS 100

K_MEM_SLAB_DEFINE_STATIC(tx_slab, BLOCK_BYTES, CONFIG_I2S_NRFX_TX_BLOCK_COUNT, 4);
RING_BUF_DECLARE(ring, RING_FRAMES * FRAME_BYTES);
static struct k_spinlock ring_lock;

static const struct device *const i2s = DEVICE_DT_GET(DT_NODELABEL(i2s0));

static atomic_t requested;
static atomic_t active_src;
static atomic_t invert_right;
static atomic_t peak;
static bool playing;

/* Fill level (frames) the stream starts at and drift compensation aims for */
static const size_t start_level[] = {
	/* Matches the 3 SOF delay of the Zephyr UAC2 explicit feedback sample */
	[AUDIO_SRC_USB] = MS_TO_FRAMES(3),
	/* Two 10 ms LC3 frames plus scheduling headroom */
	[AUDIO_SRC_BLE] = MS_TO_FRAMES(30),
};

struct drift {
	int64_t fill_sum;
	int32_t integral;
	int blocks;
};

static struct drift drift;

static void aclk_set(int32_t steps)
{
	steps = CLAMP(steps, -ACLK_MAX_STEPS, ACLK_MAX_STEPS);
	nrf_clock_hfclkaudio_config_set(NRF_CLOCK, ACLK_CENTER + steps);
}

static void ring_flush(void)
{
	K_SPINLOCK(&ring_lock) {
		ring_buf_reset(&ring);
	}
}

size_t audio_out_fill(void)
{
	size_t bytes;

	K_SPINLOCK(&ring_lock) {
		bytes = ring_buf_size_get(&ring);
	}

	return bytes / FRAME_BYTES;
}

size_t audio_out_start_level(enum audio_src src)
{
	return start_level[src];
}

static void select_source(void)
{
	enum audio_src src = AUDIO_SRC_NONE;
	atomic_val_t req = atomic_get(&requested);

	for (int i = AUDIO_SRC_USB; i > AUDIO_SRC_NONE; i--) {
		if (req & BIT(i)) {
			src = i;
			break;
		}
	}

	atomic_set(&active_src, src);
}

void audio_out_request(enum audio_src src, bool active)
{
	if (active) {
		atomic_or(&requested, BIT(src));
	} else {
		atomic_and(&requested, ~BIT(src));
	}

	select_source();
}

enum audio_src audio_out_source(void)
{
	return atomic_get(&active_src);
}

void audio_out_set_invert_right(bool invert)
{
	atomic_set(&invert_right, invert);
}

size_t audio_out_write(enum audio_src src, const int16_t *frames, size_t count)
{
	size_t bytes;

	if (src != audio_out_source()) {
		return 0;
	}

	K_SPINLOCK(&ring_lock) {
		bytes = ring_buf_put(&ring, (const uint8_t *)frames, count * FRAME_BYTES);
	}

	if (bytes < count * FRAME_BYTES) {
		LOG_WRN("Overflow, dropped %u frames", count - bytes / FRAME_BYTES);
	}

	return bytes / FRAME_BYTES;
}

/*
 * The BLE source runs on the remote clock, so the local audio clock is steered
 * to keep the ring fill at its start level. A PI loop on the 100 ms average
 * absorbs the 10 ms bursts of LC3 frames.
 */
static void drift_update(size_t fill)
{
	int32_t err;

	drift.fill_sum += fill;
	if (++drift.blocks < DRIFT_PERIOD_BLOCKS) {
		return;
	}

	err = (int32_t)(drift.fill_sum / drift.blocks) - start_level[AUDIO_SRC_BLE];
	drift.fill_sum = 0;
	drift.blocks = 0;
	drift.integral = CLAMP(drift.integral + err, -ACLK_MAX_STEPS * 32,
			       ACLK_MAX_STEPS * 32);

	/* More queued audio than wanted means the local clock is slow, speed it up */
	aclk_set(err / 2 + drift.integral / 32);
}

uint32_t audio_out_take_peak(void)
{
	return atomic_clear(&peak);
}

static void peak_update(const int32_t *samples)
{
	uint32_t max = 0;

	for (size_t i = 0; i < BLOCK_FRAMES * CHANNELS; i++) {
		max = MAX(max, (uint32_t)abs(samples[i] / 65536));
	}

	/* Only this thread raises it, a clear in between just loses one block */
	if (max > (uint32_t)atomic_get(&peak)) {
		atomic_set(&peak, max);
	}
}

static int16_t negate(int16_t s)
{
	return s == INT16_MIN ? INT16_MAX : -s;
}

static void fill_block(int32_t *out)
{
	int16_t pcm[BLOCK_FRAMES * CHANNELS] = {0};
	static enum audio_src last_src;
	enum audio_src src = audio_out_source();
	bool invert = atomic_get(&invert_right);
	size_t fill;

	if (src != last_src) {
		LOG_INF("Source %d", src);
		last_src = src;
		ring_flush();
		playing = false;
		drift = (struct drift){0};
		aclk_set(0);
	}

	fill = audio_out_fill();

	if (src == AUDIO_SRC_NONE) {
		playing = false;
	} else if (!playing && fill >= start_level[src]) {
		playing = true;
	}

	if (playing) {
		size_t bytes;

		K_SPINLOCK(&ring_lock) {
			bytes = ring_buf_get(&ring, (uint8_t *)pcm, sizeof(pcm));
		}

		if (bytes < sizeof(pcm)) {
			LOG_DBG("Underrun");
			playing = false;
			drift.fill_sum = 0;
			drift.blocks = 0;
		} else if (src == AUDIO_SRC_BLE) {
			drift_update(fill);
		}
	}

	for (size_t i = 0; i < BLOCK_FRAMES; i++) {
		int16_t right = pcm[2 * i + 1];

		out[2 * i] = (int32_t)pcm[2 * i] * 65536;
		out[2 * i + 1] = (int32_t)(invert ? negate(right) : right) * 65536;
	}

	eq_process(out, BLOCK_FRAMES);
	peak_update(out);
}

static int queue_block(void)
{
	void *block;
	int err;

	err = k_mem_slab_alloc(&tx_slab, &block, K_FOREVER);
	if (err) {
		return err;
	}

	fill_block(block);

	err = i2s_write(i2s, block, BLOCK_BYTES);
	if (err) {
		k_mem_slab_free(&tx_slab, block);
	}

	return err;
}

static int i2s_start(void)
{
	int err;

	/* The driver needs two blocks queued before it can start */
	for (int i = 0; i < 2; i++) {
		err = queue_block();
		if (err) {
			return err;
		}
	}

	return i2s_trigger(i2s, I2S_DIR_TX, I2S_TRIGGER_START);
}

static void feeder_thread(void *p1, void *p2, void *p3)
{
	for (;;) {
		int err = queue_block();

		if (err) {
			LOG_WRN("I2S write failed (%d), restarting", err);
			/*
			 * After an underrun the driver has stopped the transfer itself. DROP
			 * would stop it a second time and trip an assertion in nrfx.
			 */
			i2s_trigger(i2s, I2S_DIR_TX, I2S_TRIGGER_PREPARE);
			err = i2s_start();
			if (err) {
				LOG_ERR("I2S restart failed (%d)", err);
				/* Not running, so this only frees the blocks queued for the start */
				i2s_trigger(i2s, I2S_DIR_TX, I2S_TRIGGER_DROP);
				k_sleep(K_MSEC(100));
			}
		}
	}
}

K_THREAD_STACK_DEFINE(feeder_stack, 1024);
static struct k_thread feeder;

int audio_out_init(void)
{
	struct i2s_config cfg = {
		.word_size = 32,
		.channels = CHANNELS,
		.format = I2S_FMT_DATA_FORMAT_I2S,
		.options = I2S_OPT_BIT_CLK_CONTROLLER | I2S_OPT_FRAME_CLK_CONTROLLER,
		.frame_clk_freq = AUDIO_OUT_RATE_HZ,
		.mem_slab = &tx_slab,
		.block_size = BLOCK_BYTES,
		.timeout = SYS_FOREVER_MS,
	};
	int err;

	if (!device_is_ready(i2s)) {
		return -ENODEV;
	}

	err = i2s_configure(i2s, I2S_DIR_TX, &cfg);
	if (err) {
		LOG_ERR("I2S configure failed (%d)", err);
		return err;
	}

	err = i2s_start();
	if (err) {
		LOG_ERR("I2S start failed (%d)", err);
		return err;
	}

	k_thread_create(&feeder, feeder_stack, K_THREAD_STACK_SIZEOF(feeder_stack),
			feeder_thread, NULL, NULL, NULL, K_PRIO_COOP(2), 0, K_NO_WAIT);
	k_thread_name_set(&feeder, "audio_out");

	return 0;
}
