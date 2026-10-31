#ifndef EQ_H_
#define EQ_H_

#include <stddef.h>
#include <stdint.h>

#define EQ_BANDS       5
#define EQ_GAIN_MAX_DB 6

/* Gains in dB for 60 Hz, 250 Hz, 1 kHz, 4 kHz and 12 kHz. All zero bypasses the EQ. */
void eq_set_gains(const int8_t *gains);

/* Filters interleaved 32-bit stereo samples in place */
void eq_process(int32_t *samples, size_t frames);

#endif /* EQ_H_ */
