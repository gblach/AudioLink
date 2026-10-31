#include "lc3_rx.h"

#include <stdlib.h>
#include <string.h>

#include <lc3.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "audio_out.h"

LOG_MODULE_REGISTER(lc3_rx, LOG_LEVEL_INF);

#define CH_L    BIT(0)
#define CH_R    BIT(1)
#define CH_BOTH (CH_L | CH_R)

#define MAX_STREAMS 4
/* 10 ms is the longest LC3 frame */
#define MAX_FRAMES  (AUDIO_OUT_RATE_HZ / 100)

struct rx_item {
	struct lc3_rx_stream *s;
	struct net_buf *buf;
	uint32_t ts;
	bool valid;
};

K_MSGQ_DEFINE(rx_queue, sizeof(struct rx_item), CONFIG_BT_ISO_RX_BUF_COUNT, 4);
K_MUTEX_DEFINE(lock);

/* One decoder per output channel, so each keeps the history PLC relies on */
static lc3_decoder_mem_48k_t dec_mem[2];
static lc3_decoder_t dec[2];

static struct lc3_rx_stream *streams[MAX_STREAMS];
static uint8_t expected;

/* Stereo frame being assembled from the streams that share a timestamp */
static struct {
	int16_t pcm[MAX_FRAMES * 2];
	size_t frames;
	uint32_t ts;
	uint8_t have;
} frame;

static bool unicast_active(void)
{
	for (size_t i = 0; i < MAX_STREAMS; i++) {
		if (streams[i] != NULL && !streams[i]->broadcast) {
			return true;
		}
	}

	return false;
}

/* Broadcast audio is ignored while a phone streams to us directly */
static bool routed(const struct lc3_rx_stream *s)
{
	return s->started && (!s->broadcast || !unicast_active());
}

/*
 * Rebuilds the decoders for the streams that are routed now. This also resets
 * the decoders of streams already playing, which costs one frame of history.
 */
static void update_routing(void)
{
	expected = 0;

	for (size_t i = 0; i < MAX_STREAMS; i++) {
		struct lc3_rx_stream *s = streams[i];

		if (s == NULL || !routed(s)) {
			continue;
		}

		for (uint8_t c = 0; c < s->chan_cnt; c++) {
			int ch = (s->out[c] & CH_L) ? 0 : 1;

			dec[ch] = lc3_setup_decoder(s->frame_us, s->freq_hz, AUDIO_OUT_RATE_HZ,
						    &dec_mem[ch]);
			if (dec[ch] == NULL) {
				LOG_ERR("Decoder setup failed");
				continue;
			}

			expected |= s->out[c];
		}
	}

	frame.have = 0;
	audio_out_request(AUDIO_SRC_BLE, expected != 0);
}

static void copy_channel(int from, int to)
{
	for (size_t i = 0; i < frame.frames; i++) {
		frame.pcm[2 * i + to] = frame.pcm[2 * i + from];
	}
}

static void flush(void)
{
	if (frame.have == 0) {
		return;
	}

	/* A stream may carry only one side, play it on both */
	if (!(frame.have & CH_R)) {
		copy_channel(0, 1);
	} else if (!(frame.have & CH_L)) {
		copy_channel(1, 0);
	}

	audio_out_write(AUDIO_SRC_BLE, frame.pcm, frame.frames);
	frame.have = 0;
}

static void decode(const struct rx_item *item)
{
	const struct lc3_rx_stream *s = item->s;

	if (!routed(s)) {
		return;
	}

	for (uint8_t b = 0; b < s->blocks; b++) {
		uint32_t ts = item->ts + b * s->frame_us;

		if (frame.have && abs((int32_t)(ts - frame.ts)) > (int32_t)s->frame_us / 2) {
			flush();
		}

		frame.ts = ts;
		frame.frames = (size_t)AUDIO_OUT_RATE_HZ * s->frame_us / USEC_PER_SEC;

		for (uint8_t c = 0; c < s->chan_cnt; c++) {
			size_t offset = (b * s->chan_cnt + c) * s->octets;
			int ch = (s->out[c] & CH_L) ? 0 : 1;
			const void *data = NULL;

			if (item->valid && item->buf->len >= offset + s->octets) {
				data = item->buf->data + offset;
			}

			/* NULL data makes the decoder conceal the lost frame */
			lc3_decode(dec[ch], data, s->octets, LC3_PCM_FORMAT_S16, &frame.pcm[ch], 2);

			if (s->out[c] == CH_BOTH) {
				copy_channel(0, 1);
			}

			frame.have |= s->out[c];
		}

		if ((frame.have & expected) == expected) {
			flush();
		}
	}
}

static void decode_thread(void *p1, void *p2, void *p3)
{
	struct rx_item item;

	for (;;) {
		k_msgq_get(&rx_queue, &item, K_FOREVER);

		k_mutex_lock(&lock, K_FOREVER);
		decode(&item);
		k_mutex_unlock(&lock);

		net_buf_unref(item.buf);
	}
}

static uint8_t location_to_channel(uint32_t bit)
{
	switch (bit) {
	case BT_AUDIO_LOCATION_FRONT_LEFT:
		return CH_L;
	case BT_AUDIO_LOCATION_FRONT_RIGHT:
		return CH_R;
	default:
		return CH_BOTH;
	}
}

static int parse_config(struct lc3_rx_stream *s, const struct bt_audio_codec_cfg *cfg)
{
	enum bt_audio_location alloc;
	int ret;

	ret = bt_audio_codec_cfg_get_freq(cfg);
	if (ret < 0) {
		return ret;
	}
	s->freq_hz = bt_audio_codec_cfg_freq_to_freq_hz(ret);

	ret = bt_audio_codec_cfg_get_frame_dur(cfg);
	if (ret < 0) {
		return ret;
	}
	s->frame_us = bt_audio_codec_cfg_frame_dur_to_frame_dur_us(ret);

	ret = bt_audio_codec_cfg_get_octets_per_frame(cfg);
	if (ret < 0) {
		return ret;
	}
	s->octets = ret;

	ret = bt_audio_codec_cfg_get_frame_blocks_per_sdu(cfg, true);
	if (ret < 0) {
		return ret;
	}
	s->blocks = ret;

	ret = bt_audio_codec_cfg_get_chan_allocation(cfg, &alloc, true);
	if (ret < 0) {
		return ret;
	}

	if (alloc == BT_AUDIO_LOCATION_MONO_AUDIO) {
		s->chan_cnt = 1;
		s->out[0] = CH_BOTH;
		return 0;
	}

	if (__builtin_popcount(alloc) > ARRAY_SIZE(s->out)) {
		return -ENOTSUP;
	}

	/* LC3 channels are ordered by ascending location bit */
	s->chan_cnt = 0;
	for (uint32_t bit = BIT(0); bit != 0; bit <<= 1) {
		if (alloc & bit) {
			s->out[s->chan_cnt++] = location_to_channel(bit);
		}
	}

	return 0;
}

int lc3_rx_start(struct lc3_rx_stream *s, const struct bt_audio_codec_cfg *cfg)
{
	int slot = -1;
	int err;

	err = parse_config(s, cfg);
	if (err) {
		LOG_ERR("Unsupported codec config (%d)", err);
		return err;
	}

	LOG_INF("%s stream: %d Hz, %u us, %u octets, %u channel(s)",
		s->broadcast ? "Broadcast" : "Unicast", s->freq_hz, s->frame_us, s->octets,
		s->chan_cnt);

	k_mutex_lock(&lock, K_FOREVER);

	for (int i = 0; i < MAX_STREAMS; i++) {
		if (streams[i] == s) {
			slot = i;
			break;
		}

		if (streams[i] == NULL && slot < 0) {
			slot = i;
		}
	}

	if (slot >= 0) {
		streams[slot] = s;
		s->started = true;
		update_routing();
	}

	k_mutex_unlock(&lock);

	return slot >= 0 ? 0 : -ENOMEM;
}

void lc3_rx_stop(struct lc3_rx_stream *s)
{
	k_mutex_lock(&lock, K_FOREVER);

	for (int i = 0; i < MAX_STREAMS; i++) {
		if (streams[i] == s) {
			streams[i] = NULL;
		}
	}

	s->started = false;
	update_routing();

	k_mutex_unlock(&lock);
}

void lc3_rx_recv(struct lc3_rx_stream *s, const struct bt_iso_recv_info *info,
		 struct net_buf *buf)
{
	struct rx_item item = {
		.s = s,
		.valid = (info->flags & BT_ISO_FLAGS_VALID) != 0,
		.buf = net_buf_ref(buf),
	};

	/* Without a controller timestamp, derive one from the SDU counter */
	if (info->flags & BT_ISO_FLAGS_TS) {
		item.ts = info->ts;
	} else {
		item.ts = info->seq_num * s->frame_us * s->blocks;
	}

	if (k_msgq_put(&rx_queue, &item, K_NO_WAIT) != 0) {
		LOG_WRN("Decoder queue full");
		net_buf_unref(buf);
	}
}

K_THREAD_STACK_DEFINE(decode_stack, 4096);
static struct k_thread decoder;

int lc3_rx_init(void)
{
	k_thread_create(&decoder, decode_stack, K_THREAD_STACK_SIZEOF(decode_stack),
			decode_thread, NULL, NULL, NULL, K_PRIO_PREEMPT(2), K_FP_REGS, K_NO_WAIT);
	k_thread_name_set(&decoder, "lc3_rx");

	return 0;
}
