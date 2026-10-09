#ifndef AUDIO_CAPTURE_H
#define AUDIO_CAPTURE_H

#include <stdint.h>

void AudioCapture_Init(void);
void AudioCapture_SetStreaming(uint8_t enabled);
void AudioCapture_SetMute(uint8_t enabled);
uint16_t AudioCapture_GetPacket(uint8_t *packet, uint16_t capacity);

#endif
