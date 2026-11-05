#ifndef AUDIO_OUT_H_
#define AUDIO_OUT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AUDIO_OUT_RATE_HZ 48000

/* Lowest priority first, a higher value wins when several sources are active */
enum audio_src {
	AUDIO_SRC_NONE,
	AUDIO_SRC_BLE,
	AUDIO_SRC_USB,
	/* Generated on the board, overrides the others while the app has test mode on */
	AUDIO_SRC_TEST,
};

int audio_out_init(void);

/* Mark a source as wanting (or no longer wanting) the output */
void audio_out_request(enum audio_src src, bool active);

enum audio_src audio_out_source(void);

/*
 * Queue interleaved 16-bit stereo frames. Frames from a source that is not the
 * active one are dropped. Returns the number of frames queued.
 */
size_t audio_out_write(enum audio_src src, const int16_t *frames, size_t count);

/* Highest absolute sample since the last call, 0 to 32768 */
uint32_t audio_out_take_peak(void);

/* Frames currently queued */
size_t audio_out_fill(void);

/* Fill level a source starts playing at, and the level clock tracking aims for */
size_t audio_out_start_level(enum audio_src src);

/* Negate the right channel, used in balanced mode where J2 is wired tip=OUTRN */
void audio_out_set_invert_right(bool invert);

#endif /* AUDIO_OUT_H_ */
