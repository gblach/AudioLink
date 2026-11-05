#ifndef TEST_SIGNAL_H_
#define TEST_SIGNAL_H_

#include <stddef.h>
#include <stdint.h>

/*
 * Write interleaved 16-bit stereo frames of pink noise bursts whose level sweeps slowly up and
 * down, to check the speakers and the LEDs without a source.
 */
void test_signal_fill(int16_t *pcm, size_t frames);

#endif /* TEST_SIGNAL_H_ */
