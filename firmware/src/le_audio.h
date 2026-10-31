#ifndef LE_AUDIO_H_
#define LE_AUDIO_H_

#include <stdint.h>

int le_audio_init(void);

/* VCS volume, 0 when muted */
uint8_t le_audio_volume(void);

#endif /* LE_AUDIO_H_ */
