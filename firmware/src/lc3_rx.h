#ifndef LC3_RX_H_
#define LC3_RX_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/bluetooth/audio/audio.h>
#include <zephyr/bluetooth/iso.h>
#include <zephyr/net_buf.h>

/* One received LE Audio stream, unicast CIS or broadcast BIS */
struct lc3_rx_stream {
	bool broadcast;
	bool started;
	uint8_t chan_cnt;
	/* Output channels fed by each LC3 channel in the SDU, bit 0 = left, bit 1 = right */
	uint8_t out[2];
	uint8_t blocks;
	uint16_t octets;
	uint32_t frame_us;
	int freq_hz;
};

int lc3_rx_init(void);

int lc3_rx_start(struct lc3_rx_stream *s, const struct bt_audio_codec_cfg *cfg);

void lc3_rx_stop(struct lc3_rx_stream *s);

/* Called from the ISO receive callback, decoding happens on a separate thread */
void lc3_rx_recv(struct lc3_rx_stream *s, const struct bt_iso_recv_info *info,
		 struct net_buf *buf);

#endif /* LC3_RX_H_ */
