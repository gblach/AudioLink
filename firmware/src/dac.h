#ifndef DAC_H_
#define DAC_H_

#include <stdbool.h>
#include <stdint.h>

/* Powers the analog rails and configures the PCM5242. Leaves the output muted. */
int dac_init(void);

void dac_mute(bool mute);

/* Bluetooth VCS scale: 0 is silent, 255 is 0 dB */
int dac_set_volume(uint8_t volume);

/* Logs the PCM5242 power state and clock status whenever they change */
void dac_poll_status(void);

#endif /* DAC_H_ */
