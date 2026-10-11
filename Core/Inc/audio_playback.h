#ifndef AUDIO_PLAYBACK_H
#define AUDIO_PLAYBACK_H

#include <stdint.h>

#define AUDIO_PWM_PERIOD_TICKS 375U
#define AUDIO_PWM_OVERSAMPLE 4U

void AudioPlayback_Init(void);
void AudioPlayback_SetStreaming(uint8_t enabled);
void AudioPlayback_SetMute(uint8_t enabled);
/* Returns zero for malformed packets; a zero-length USB packet is valid. */
uint8_t AudioPlayback_PushPacket(const uint8_t *packet, uint16_t length);
/* Call once per USB SOF while playback is active. UAC1 samples/frame, Q10.14. */
uint32_t AudioPlayback_GetFeedback(void);

#endif
