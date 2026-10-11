#include "audio_playback.h"
#include "tim.h"
#include "usbd_audio.h"

#define PLAYBACK_RING_FRAMES 512U
#define PLAYBACK_TARGET_FRAMES (3U * AUDIO_SAMPLES_PER_PACKET)
#define PWM_HALF_WORDS (AUDIO_SAMPLES_PER_PACKET * AUDIO_PWM_OVERSAMPLE * AUDIO_CHANNELS)
#define PLAYBACK_CLOCK_HZ 72000000U
#define PLAYBACK_CYCLES_PER_FRAME (PLAYBACK_CLOCK_HZ / USBD_AUDIO_FREQ)

_Static_assert(AUDIO_CHANNELS == 2U, "TIM1 drives stereo output");
_Static_assert(USBD_AUDIO_FREQ * AUDIO_PWM_OVERSAMPLE * AUDIO_PWM_PERIOD_TICKS ==
               PLAYBACK_CLOCK_HZ, "TIM1 PWM must be exactly four times the PCM rate");

/* Each DMA burst writes the left and right CCR preloads at one PWM update.
 * Two 1-ms halves; halfword DMA even though the HAL burst API takes uint32_t *.
 */
_Alignas(4) static uint16_t pwm_dma_buffer[2U * PWM_HALF_WORDS];
static int16_t playback_ring[PLAYBACK_RING_FRAMES * AUDIO_CHANNELS];
static uint32_t playback_read_frame;
static uint32_t playback_write_frame;
static uint32_t duty_error[AUDIO_CHANNELS];
static uint8_t playback_streaming;
static uint8_t playback_muted;
static uint8_t playback_primed;
/* Useful in the debugger; recovery always returns to a bounded, primed stream. */
static uint32_t playback_underruns;
static uint32_t playback_overruns;
static uint32_t feedback_last_cycle;
static uint32_t feedback_rate_q20;
static int32_t feedback_level_q8;
static uint8_t feedback_clock_valid;

static void AudioPlayback_FillHalf(uint16_t *destination)
{
  int16_t samples[AUDIO_SAMPLES_PER_PACKET * AUDIO_CHANNELS] = {0};
  uint32_t irq_state = __get_PRIMASK();
  __disable_irq();
  uint32_t available = playback_write_frame - playback_read_frame;
  if ((playback_streaming != 0U) &&
      ((playback_primed != 0U) || (available >= PLAYBACK_TARGET_FRAMES)))
  {
    if (available >= AUDIO_SAMPLES_PER_PACKET)
    {
      playback_primed = 1U;
      for (uint32_t frame = 0U; frame < AUDIO_SAMPLES_PER_PACKET; ++frame)
      {
        uint32_t index = ((playback_read_frame + frame) % PLAYBACK_RING_FRAMES) *
                         AUDIO_CHANNELS;
        if (playback_muted == 0U)
        {
          samples[frame * AUDIO_CHANNELS] = playback_ring[index];
          samples[frame * AUDIO_CHANNELS + 1U] = playback_ring[index + 1U];
        }
      }
      playback_read_frame += AUDIO_SAMPLES_PER_PACKET;
    }
    else
    {
      playback_primed = 0U;
      ++playback_underruns;
    }
  }
  __set_PRIMASK(irq_state);

  /* Convert S16_LE to a 10-bit unsigned target, then first-order error-diffuse
   * its fractional PWM duty at 192 kHz. Persistent, separate accumulators keep
   * sub-tick precision and stereo order across DMA halves. PCM zero alternates
   * 187/188 ticks, producing an exact average 50% duty. No MHz/sample IRQs.
   */
  for (uint32_t frame = 0U; frame < AUDIO_SAMPLES_PER_PACKET; ++frame)
  {
    uint32_t target[AUDIO_CHANNELS];
    for (uint32_t channel = 0U; channel < AUDIO_CHANNELS; ++channel)
    {
      uint32_t code = (uint32_t)((int32_t)samples[frame * AUDIO_CHANNELS + channel] +
                                 32768) >> 6;
      target[channel] = code * AUDIO_PWM_PERIOD_TICKS;
    }
    for (uint32_t phase = 0U; phase < AUDIO_PWM_OVERSAMPLE; ++phase)
    {
      for (uint32_t channel = 0U; channel < AUDIO_CHANNELS; ++channel)
      {
        uint32_t value = target[channel] + duty_error[channel];
        *destination++ = (uint16_t)(value >> 10);
        duty_error[channel] = value & 1023U;
      }
    }
  }
}

void AudioPlayback_Init(void)
{
  /* DWT measures the same 72 MHz clock as TIM1, without reserving another timer.
   * Unsigned SOF-to-SOF subtraction handles its approximately 60-second wrap.
   */
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
  AudioPlayback_SetStreaming(0U);
  AudioPlayback_SetMute(0U);
  AudioPlayback_FillHalf(pwm_dma_buffer);
  AudioPlayback_FillHalf(&pwm_dma_buffer[PWM_HALF_WORDS]);
  if ((HAL_TIM_DMABurst_MultiWriteStart(&htim1, TIM_DMABASE_CCR1, TIM_DMA_UPDATE,
         (const uint32_t *)pwm_dma_buffer, TIM_DMABURSTLENGTH_2TRANSFERS,
         2U * PWM_HALF_WORDS) != HAL_OK) ||
      (HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1) != HAL_OK) ||
      (HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_2) != HAL_OK))
  {
    Error_Handler();
  }
}

void AudioPlayback_SetStreaming(uint8_t enabled)
{
  uint32_t irq_state = __get_PRIMASK();
  __disable_irq();
  playback_streaming = (enabled != 0U);
  playback_read_frame = 0U;
  playback_write_frame = 0U;
  playback_primed = 0U;
  feedback_clock_valid = 0U;
  feedback_rate_q20 = AUDIO_SAMPLES_PER_PACKET << 20;
  feedback_level_q8 = (int32_t)(PLAYBACK_TARGET_FRAMES << 8);
  __set_PRIMASK(irq_state);
}

void AudioPlayback_SetMute(uint8_t enabled)
{
  playback_muted = (enabled != 0U);
}

uint8_t AudioPlayback_PushPacket(const uint8_t *packet, uint16_t length)
{
  if ((length > AUDIO_OUT_MAX_PACKET) || ((length % AUDIO_FRAME_BYTES) != 0U) ||
      ((packet == NULL) && (length != 0U)))
  {
    return 0U;
  }
  uint32_t irq_state = __get_PRIMASK();
  __disable_irq();
  if (playback_streaming != 0U)
  {
    uint32_t frames = length / AUDIO_FRAME_BYTES;
    if ((playback_write_frame - playback_read_frame + frames) > PLAYBACK_RING_FRAMES)
    {
      playback_read_frame = playback_write_frame;
      playback_primed = 0U;
      ++playback_overruns;
    }
    for (uint32_t frame = 0U; frame < frames; ++frame)
    {
      uint32_t index = ((playback_write_frame + frame) % PLAYBACK_RING_FRAMES) *
                       AUDIO_CHANNELS;
      for (uint32_t channel = 0U; channel < AUDIO_CHANNELS; ++channel)
      {
        uint32_t byte = (frame * AUDIO_CHANNELS + channel) * AUDIO_BYTES_PER_SAMPLE;
        uint16_t value = (uint16_t)(packet[byte] | ((uint16_t)packet[byte + 1U] << 8));
        playback_ring[index + channel] = (int16_t)((int32_t)(value ^ 0x8000U) - 32768);
      }
    }
    playback_write_frame += frames;
  }
  __set_PRIMASK(irq_state);
  return 1U;
}

uint32_t AudioPlayback_GetFeedback(void)
{
  uint32_t now = DWT->CYCCNT;
  uint32_t elapsed = now - feedback_last_cycle;
  feedback_last_cycle = now;
  if ((feedback_clock_valid != 0U) &&
      (elapsed >= PLAYBACK_CLOCK_HZ / 2000U) &&
      (elapsed <= 3U * PLAYBACK_CLOCK_HZ / 2000U))
  {
    /* One PCM frame is 1500 clock cycles. Filter in Q20 to avoid a Q14 dead zone. */
    int32_t measured = (int32_t)(((uint64_t)elapsed << 20) / PLAYBACK_CYCLES_PER_FRAME);
    feedback_rate_q20 = (uint32_t)((int32_t)feedback_rate_q20 +
                                  (measured - (int32_t)feedback_rate_q20) / 64);
  }
  feedback_clock_valid = 1U;

  uint32_t irq_state = __get_PRIMASK();
  __disable_irq();
  uint32_t available = playback_write_frame - playback_read_frame;
  uint8_t primed = playback_primed;
  __set_PRIMASK(irq_state);
  int32_t correction = 0;
  if (primed != 0U)
  {
    feedback_level_q8 += ((int32_t)(available << 8) - feedback_level_q8) / 16;
    int32_t error = ((int32_t)(PLAYBACK_TARGET_FRAMES << 8) - feedback_level_q8) / 256;
    /* Ignore ordinary 1-ms DMA/USB packet arrival jitter. Outside that band,
     * gently steer host delivery towards the ring target, at most +/-0.25 frame/ms.
     */
    if (error > (int32_t)(AUDIO_SAMPLES_PER_PACKET / 2U))
    {
      correction = (error - (int32_t)(AUDIO_SAMPLES_PER_PACKET / 2U)) * 32;
    }
    else if (error < -(int32_t)(AUDIO_SAMPLES_PER_PACKET / 2U))
    {
      correction = (error + (int32_t)(AUDIO_SAMPLES_PER_PACKET / 2U)) * 32;
    }
    if (correction > 4096)
    {
      correction = 4096;
    }
    if (correction < -4096)
    {
      correction = -4096;
    }
  }
  int32_t feedback = (int32_t)((feedback_rate_q20 + 32U) >> 6) + correction;
  if (feedback < (47 << 14))
  {
    feedback = 47 << 14;
  }
  if (feedback > (49 << 14))
  {
    feedback = 49 << 14;
  }
  return (uint32_t)feedback;
}

void HAL_TIM_PeriodElapsedHalfCpltCallback(TIM_HandleTypeDef *timer)
{
  if (timer->Instance == TIM1)
  {
    AudioPlayback_FillHalf(pwm_dma_buffer);
  }
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *timer)
{
  if (timer->Instance == TIM1)
  {
    AudioPlayback_FillHalf(&pwm_dma_buffer[PWM_HALF_WORDS]);
  }
}

void HAL_TIM_ErrorCallback(TIM_HandleTypeDef *timer)
{
  if (timer->Instance == TIM1)
  {
    __HAL_TIM_MOE_DISABLE(timer);
    Error_Handler();
  }
}
