#ifndef AUDIO_USB_PMA_H
#define AUDIO_USB_PMA_H

#include "usbd_audio.h"

/* Only endpoint registers 0..3 are used. Addresses are PMA byte offsets, not
 * CPU addresses (the F103 maps 16-bit PMA words onto 32-bit bus locations).
 */
#define AUDIO_PMA_BTABLE_BYTES (4U * 8U)
#define AUDIO_PMA_EP0_OUT AUDIO_PMA_BTABLE_BYTES
#define AUDIO_PMA_EP0_IN (AUDIO_PMA_EP0_OUT + USB_MAX_EP0_SIZE)
#define AUDIO_PMA_CAPTURE (AUDIO_PMA_EP0_IN + USB_MAX_EP0_SIZE)
#define AUDIO_PMA_PLAYBACK (AUDIO_PMA_CAPTURE + AUDIO_IN_MAX_PACKET)
#define AUDIO_PMA_FEEDBACK (AUDIO_PMA_PLAYBACK + AUDIO_OUT_BUFFER_BYTES)
#define AUDIO_PMA_END (AUDIO_PMA_FEEDBACK + ((AUDIO_FEEDBACK_BYTES + 1U) & ~1U))
#define AUDIO_PMA_ISO_SLOTS(address) ((uint32_t)(address) | ((uint32_t)(address) << 16))

_Static_assert(USB_MAX_EP0_SIZE == 16U, "Stereo full duplex needs 16-byte EP0 buffers");
_Static_assert(AUDIO_PMA_END <= 512U, "USB audio exceeds STM32F103 packet memory");
_Static_assert((AUDIO_PMA_CAPTURE & 1U) == 0U && (AUDIO_PMA_PLAYBACK & 1U) == 0U &&
               (AUDIO_PMA_FEEDBACK & 1U) == 0U, "PMA buffers must be word aligned");
_Static_assert(AUDIO_IN_EP == 0x81U && AUDIO_OUT_EP == 0x02U &&
               AUDIO_FEEDBACK_EP == 0x83U, "PMA table covers only endpoints 0..3");

#endif
