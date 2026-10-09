#include "audio_capture.h"
#include "adc.h"
#include "tim.h"
#include "usbd_audio.h"
#include <string.h>

#define ADC_BATCH_SAMPLES (AUDIO_SAMPLES_PER_PACKET * AUDIO_CHANNELS)
#define CAPTURE_RING_FRAMES 512U
#define CAPTURE_TARGET_FRAMES (3U * AUDIO_SAMPLES_PER_PACKET)
#define CAPTURE_JITTER_FRAMES (AUDIO_SAMPLES_PER_PACKET / 2U)

_Static_assert(USBD_AUDIO_FREQ == 48000U, "TIM3 is configured for 48 kHz capture");
_Static_assert(AUDIO_CHANNELS == 3U, "ADC1 scans three input channels");

/* DMA stores interleaved PA1 (left), PA3 (right), PA2 (center) 12-bit samples
 * in two 1-ms halves, each holding 48 three-channel frames (144 halfwords).
 */
_Alignas(4) static uint16_t adc_dma_buffer[2U * ADC_BATCH_SAMPLES];
static int16_t capture_ring[CAPTURE_RING_FRAMES * AUDIO_CHANNELS];
static uint32_t read_frame;
static uint32_t write_frame;
static uint8_t streaming;
static uint8_t muted;
static uint8_t primed;

void AudioCapture_Init(void)
{
  /* Arm DMA before starting the timer, so the first scan is a complete audio frame. */
  if ((HAL_ADCEx_Calibration_Start(&hadc1) != HAL_OK) ||
      (HAL_ADC_Start_DMA(&hadc1, (uint32_t *)adc_dma_buffer,
                         2U * ADC_BATCH_SAMPLES) != HAL_OK) ||
      (HAL_TIM_Base_Start(&htim3) != HAL_OK))
  {
    Error_Handler();
  }
}

void AudioCapture_SetStreaming(uint8_t enabled)
{
  uint32_t irq_state = __get_PRIMASK();
  __disable_irq();
  streaming = (enabled != 0U);
  read_frame = 0U;
  write_frame = 0U;
  primed = 0U;
  __set_PRIMASK(irq_state);
}

void AudioCapture_SetMute(uint8_t enabled)
{
  muted = (enabled != 0U);
}

static void AudioCapture_PushSamples(const uint16_t *samples)
{
  uint32_t irq_state = __get_PRIMASK();
  __disable_irq();
  if (streaming != 0U)
  {
    /* Discard oldest samples after a long host pause; never overwrite unread data. */
    if ((write_frame - read_frame) > (CAPTURE_RING_FRAMES - AUDIO_SAMPLES_PER_PACKET))
    {
      read_frame = write_frame - (CAPTURE_RING_FRAMES - AUDIO_SAMPLES_PER_PACKET);
    }
    for (uint32_t frame = 0U; frame < AUDIO_SAMPLES_PER_PACKET; ++frame)
    {
      uint32_t index = ((write_frame + frame) % CAPTURE_RING_FRAMES) * AUDIO_CHANNELS;
      for (uint32_t channel = 0U; channel < AUDIO_CHANNELS; ++channel)
      {
        /* A mid-supply input (ADC code 2048) is PCM zero, not -32768.
         * Multiplication avoids left-shifting negative signed integers.
         */
        capture_ring[index + channel] =
          (int16_t)(((int32_t)samples[frame * AUDIO_CHANNELS + channel] - 2048) * 16);
      }
    }
    write_frame += AUDIO_SAMPLES_PER_PACKET;
  }
  __set_PRIMASK(irq_state);
}

uint16_t AudioCapture_GetPacket(uint8_t *packet, uint16_t capacity)
{
  uint32_t irq_state;
  uint32_t available;
  uint32_t frames = AUDIO_SAMPLES_PER_PACKET;

  if (capacity < AUDIO_IN_MAX_PACKET)
  {
    return 0U;
  }
  irq_state = __get_PRIMASK();
  __disable_irq();
  available = write_frame - read_frame;

  /* Keep enough reserve for a SOF with no completed ADC batch when the slower
   * source clock crosses the host frame boundary. ADC DMA and SOF are independent.
   */
  if ((streaming == 0U) || ((primed == 0U) && (available < CAPTURE_TARGET_FRAMES)))
  {
    memset(packet, 0, AUDIO_IN_PACKET);
    __set_PRIMASK(irq_state);
    return AUDIO_IN_PACKET;
  }
  primed = 1U;

  /* Allow 47/48/49 frames per USB millisecond to track crystal/host clock drift.
   * The hysteresis accommodates the 48-frame DMA batch arrival jitter.
   */
  if (available > (CAPTURE_TARGET_FRAMES + CAPTURE_JITTER_FRAMES))
  {
    ++frames;
  }
  else if (available < (CAPTURE_TARGET_FRAMES - CAPTURE_JITTER_FRAMES))
  {
    --frames;
  }
  if (available < frames)
  {
    /* Re-prime after a real underrun rather than sending partial audio frames. */
    primed = 0U;
    memset(packet, 0, AUDIO_IN_PACKET);
    __set_PRIMASK(irq_state);
    return AUDIO_IN_PACKET;
  }
  for (uint32_t frame = 0U; frame < frames; ++frame)
  {
    uint32_t index = ((read_frame + frame) % CAPTURE_RING_FRAMES) * AUDIO_CHANNELS;
    for (uint32_t channel = 0U; channel < AUDIO_CHANNELS; ++channel)
    {
      uint16_t sample = (muted != 0U) ? 0U : (uint16_t)capture_ring[index + channel];
      uint32_t byte = frame * AUDIO_FRAME_BYTES + channel * AUDIO_BYTES_PER_SAMPLE;
      packet[byte] = (uint8_t)sample;
      packet[byte + 1U] = (uint8_t)(sample >> 8);
    }
  }
  read_frame += frames; /* Consume samples even when muted. */
  __set_PRIMASK(irq_state);
  return (uint16_t)(frames * AUDIO_FRAME_BYTES);
}

void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef *hadc)
{
  if (hadc->Instance == ADC1)
  {
    AudioCapture_PushSamples(adc_dma_buffer);
  }
}

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
  if (hadc->Instance == ADC1)
  {
    AudioCapture_PushSamples(&adc_dma_buffer[ADC_BATCH_SAMPLES]);
  }
}
