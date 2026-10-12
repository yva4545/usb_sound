#include <assert.h>
#include <stdio.h>

/* Exercise the production code with hardware replaced by host stubs. Including
 * the capture implementation also lets us populate the simulated ADC DMA buffer.
 */
#include "../Core/Src/audio_capture.c"
#include "../Core/Src/audio_playback.c"
#include "../Middlewares/ST/STM32_USB_Device_Library/Class/AUDIO/Src/usbd_audio.c"
#include "audio_usb_pma.h"

ADC_HandleTypeDef hadc1 = { ADC1 };
TIM_HandleTypeDef htim3 = { TIM3 };
TIM_HandleTypeDef htim1 = { TIM1 };
TestCoreDebug test_core_debug;
TestDWT test_dwt;
static unsigned init_order;
static unsigned pwm_init_order;
static unsigned endpoint_open;
static unsigned playback_endpoint_open;
static unsigned feedback_endpoint_open;
static unsigned transmissions;
static unsigned feedback_transmissions;
static unsigned receive_preparations;
static unsigned received_packets;
static uint8_t fail_open_ep;
static uint8_t fail_transmit_ep;
static uint8_t fail_receive;
static uint8_t *receive_buffer;
static uint32_t receive_length;
static uint8_t transmitted_feedback[AUDIO_FEEDBACK_BYTES];
static unsigned errors;
static uint8_t *control_buffer;
static uint16_t control_length;
static uint16_t transmitted_length;
static uint8_t transmitted[AUDIO_IN_MAX_PACKET];
static uint32_t irq_mask;
static uint16_t usb_frame;
static unsigned capture_closes;
static unsigned feedback_closes;

uint32_t __get_PRIMASK(void) { return irq_mask; }
void __disable_irq(void) { irq_mask = 1U; }
void __set_PRIMASK(uint32_t state) { irq_mask = state; }
void Error_Handler(void) { abort(); }

int HAL_ADCEx_Calibration_Start(ADC_HandleTypeDef *adc)
{
  assert(adc == &hadc1 && init_order++ == 0U);
  return HAL_OK;
}
int HAL_ADC_Start_DMA(ADC_HandleTypeDef *adc, uint32_t *buffer, uint32_t length)
{
  assert(adc == &hadc1 && init_order++ == 1U);
  assert(buffer == (uint32_t *)adc_dma_buffer && length == 192U);
  return HAL_OK;
}
int HAL_TIM_Base_Start(TIM_HandleTypeDef *timer)
{
  assert(timer == &htim3 && init_order++ == 2U);
  return HAL_OK;
}

int HAL_TIM_DMABurst_MultiWriteStart(TIM_HandleTypeDef *timer, uint32_t base,
                                     uint32_t source, const uint32_t *buffer,
                                     uint32_t burst, uint32_t length)
{
  assert(timer == &htim1 && pwm_init_order++ == 0U);
  assert(base == TIM_DMABASE_CCR1 && source == TIM_DMA_UPDATE);
  assert(burst == TIM_DMABURSTLENGTH_2TRANSFERS && length == 768U);
  assert(buffer == (const uint32_t *)pwm_dma_buffer);
  return HAL_OK;
}
int HAL_TIM_PWM_Start(TIM_HandleTypeDef *timer, uint32_t channel)
{
  assert(timer == &htim1);
  assert(channel == ((pwm_init_order++ == 1U) ? TIM_CHANNEL_1 : TIM_CHANNEL_2));
  return HAL_OK;
}

static unsigned *endpoint_state(uint8_t ep)
{
  if (ep == AUDIO_IN_EP) return &endpoint_open;
  if (ep == AUDIO_OUT_EP) return &playback_endpoint_open;
  assert(ep == AUDIO_FEEDBACK_EP);
  return &feedback_endpoint_open;
}

USBD_StatusTypeDef USBD_LL_OpenEP(USBD_HandleTypeDef *dev, uint8_t ep,
                                 uint8_t type, uint16_t size)
{
  UNUSED(dev);
  assert(type == USBD_EP_TYPE_ISOC);
  assert(size == ((ep == AUDIO_FEEDBACK_EP) ? 3U : 196U));
  unsigned *state = endpoint_state(ep);
  assert(*state == 0U);
  if (ep == fail_open_ep) return USBD_FAIL;
  *state = 1U;
  return USBD_OK;
}
USBD_StatusTypeDef USBD_LL_CloseEP(USBD_HandleTypeDef *dev, uint8_t ep)
{
  UNUSED(dev);
  unsigned *state = endpoint_state(ep);
  assert(*state == 1U);
  *state = 0U;
  if (ep == AUDIO_IN_EP) ++capture_closes;
  if (ep == AUDIO_FEEDBACK_EP) ++feedback_closes;
  return USBD_OK;
}
uint16_t USBD_LL_GetFrameNumber(USBD_HandleTypeDef *dev)
{
  UNUSED(dev);
  return usb_frame;
}
static uint8_t test_sof(USBD_HandleTypeDef *dev)
{
  usb_frame = (usb_frame + 1U) & AUDIO_USB_FRAME_MASK;
  test_dwt.CYCCNT += 72000U;
  return USBD_AUDIO.SOF(dev);
}
USBD_StatusTypeDef USBD_LL_Transmit(USBD_HandleTypeDef *dev, uint8_t ep,
                                   uint8_t *buffer, uint16_t size)
{
  UNUSED(dev);
  assert(*endpoint_state(ep) == 1U);
  if (ep == fail_transmit_ep) return USBD_FAIL;
  if (ep == AUDIO_IN_EP)
  {
    assert(size <= sizeof(transmitted));
    memcpy(transmitted, buffer, size);
    transmitted_length = size;
    ++transmissions;
  }
  else
  {
    assert(ep == AUDIO_FEEDBACK_EP && size == AUDIO_FEEDBACK_BYTES);
    memcpy(transmitted_feedback, buffer, size);
    ++feedback_transmissions;
  }
  return USBD_OK;
}
USBD_StatusTypeDef USBD_LL_PrepareReceive(USBD_HandleTypeDef *dev, uint8_t ep,
                                         uint8_t *buffer, uint16_t size)
{
  UNUSED(dev);
  assert(ep == AUDIO_OUT_EP && playback_endpoint_open == 1U && size == 196U);
  if (fail_receive != 0U) return USBD_FAIL;
  receive_buffer = buffer;
  ++receive_preparations;
  return USBD_OK;
}
uint32_t USBD_LL_GetRxDataSize(USBD_HandleTypeDef *dev, uint8_t ep)
{
  UNUSED(dev);
  assert(ep == AUDIO_OUT_EP);
  return receive_length;
}
USBD_StatusTypeDef USBD_CtlSendData(USBD_HandleTypeDef *dev, uint8_t *buffer,
                                   uint16_t length)
{
  UNUSED(dev);
  control_buffer = buffer;
  control_length = length;
  return USBD_OK;
}
USBD_StatusTypeDef USBD_CtlPrepareRx(USBD_HandleTypeDef *dev, uint8_t *buffer,
                                    uint16_t length)
{
  return USBD_CtlSendData(dev, buffer, length);
}
void USBD_CtlError(USBD_HandleTypeDef *dev, USBD_SetupReqTypedef *req)
{
  UNUSED(dev);
  UNUSED(req);
  ++errors;
}

static int8_t test_init(uint32_t freq, uint32_t volume, uint32_t options)
{
  assert(freq == 48000U);
  UNUSED(volume);
  UNUSED(options);
  AudioCapture_SetStreaming(0U);
  AudioCapture_SetMute(0U);
  AudioPlayback_SetStreaming(0U);
  AudioPlayback_SetMute(0U);
  return USBD_OK;
}
static int8_t test_deinit(uint32_t options)
{
  UNUSED(options);
  AudioCapture_SetStreaming(0U);
  AudioPlayback_SetStreaming(0U);
  return USBD_OK;
}
static int8_t test_command(uint8_t *buffer, uint32_t size, uint8_t command)
{
  UNUSED(buffer);
  UNUSED(size);
  assert(command == AUDIO_CMD_START || command == AUDIO_CMD_STOP);
  AudioCapture_SetStreaming(command == AUDIO_CMD_START);
  return USBD_OK;
}
static int8_t test_mute(uint8_t value)
{
  AudioCapture_SetMute(value);
  return USBD_OK;
}
static uint8_t test_put_packet(const uint8_t *packet, uint16_t length)
{
  ++received_packets;
  return AudioPlayback_PushPacket(packet, length);
}
static USBD_AUDIO_ItfTypeDef callbacks =
{
  .Init = test_init,
  .DeInit = test_deinit,
  .AudioCmd = test_command,
  .MuteCtl = test_mute,
  .GetPacket = AudioCapture_GetPacket,
  .PlaybackSetStreaming = AudioPlayback_SetStreaming,
  .PlaybackMuteCtl = AudioPlayback_SetMute,
  .PutPacket = test_put_packet,
  .GetFeedback = AudioPlayback_GetFeedback,
};

static uint16_t le16(const uint8_t *bytes)
{
  return (uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8));
}
static void assert_silence(const uint8_t *bytes, uint16_t length)
{
  for (uint16_t i = 0U; i < length; ++i)
  {
    assert(bytes[i] == 0U);
  }
}
static void fill_dma(uint16_t left, uint16_t right)
{
  for (unsigned i = 0U; i < 2U * ADC_BATCH_SAMPLES; i += 2U)
  {
    adc_dma_buffer[i] = left;
    adc_dma_buffer[i + 1U] = right;
  }
}

static void test_descriptors(void)
{
  uint16_t length;
  uint8_t *desc = USBD_AUDIO.GetFSConfigDescriptor(&length);
  unsigned offset = 0U;
  unsigned interface = 0U;
  unsigned endpoints = 0U;
  unsigned input_seen = 0U, feature_seen = 0U, output_seen = 0U, format_seen = 0U;
  assert(length == 203U && le16(desc + 2U) == length && desc[4] == 3U);
  while (offset < length)
  {
    uint8_t *d = desc + offset;
    assert(d[0] >= 2U && offset + d[0] <= length);
    if (d[1] == USB_DESC_TYPE_INTERFACE)
    {
      interface = d[2];
      assert(d[3] <= 1U);
      assert(d[4] == ((d[3] == 1U) ? interface : 0U));
    }
    else if (d[1] == USB_DESC_TYPE_ENDPOINT)
    {
      assert(d[6] == 1U);
      if (d[2] == AUDIO_IN_EP)
      {
        assert(interface == 1U && d[3] == 0x05U && le16(d + 4U) == 196U);
        assert(d[8] == 0U);
      }
      else if (d[2] == AUDIO_OUT_EP)
      {
        assert(interface == 2U && d[3] == 0x05U && le16(d + 4U) == 196U);
        assert(d[8] == AUDIO_FEEDBACK_EP);
      }
      else
      {
        assert(interface == 2U && d[2] == AUDIO_FEEDBACK_EP && d[3] == 0x11U);
        assert(le16(d + 4U) == 3U && d[7] == 1U && d[8] == 0U);
      }
      ++endpoints;
    }
    else if (d[1] == AUDIO_INTERFACE_DESCRIPTOR_TYPE && interface == 0U)
    {
      switch (d[2])
      {
        case AUDIO_CONTROL_HEADER:
          assert(d[0] == 10U && le16(d + 5U) == 72U && d[7] == 2U);
          assert(d[8] == 1U && d[9] == 2U);
          break;
        case AUDIO_CONTROL_INPUT_TERMINAL:
          assert(d[3] == 1U || d[3] == 4U);
          assert(le16(d + 4U) == ((d[3] == 1U) ? 0x0603U : 0x0101U));
          assert(d[7] == 2U && le16(d + 8U) == 3U);
          ++input_seen;
          break;
        case AUDIO_CONTROL_FEATURE_UNIT:
          assert(d[0] == 10U && (d[3] == 2U || d[3] == 5U) && d[6] == 1U);
          assert(d[4] == d[3] - 1U);
          assert(d[7] == 0U && d[8] == 0U);
          ++feature_seen;
          break;
        case AUDIO_CONTROL_OUTPUT_TERMINAL:
          assert(d[3] == 3U || d[3] == 6U);
          assert(le16(d + 4U) == ((d[3] == 3U) ? 0x0101U : 0x0603U));
          assert(d[7] == d[3] - 1U);
          ++output_seen;
          break;
      }
    }
    else if (d[1] == AUDIO_INTERFACE_DESCRIPTOR_TYPE && interface != 0U)
    {
      if (d[2] == AUDIO_STREAMING_GENERAL)
      {
        assert(d[3] == ((interface == 1U) ? 3U : 4U));
      }
      else if (d[2] == AUDIO_STREAMING_FORMAT_TYPE)
      {
        assert(d[3] == 1U && d[4] == 2U && d[5] == 2U && d[6] == 16U);
        assert(d[7] == 1U && (le16(d + 8U) | ((uint32_t)d[10] << 16)) == 48000U);
        ++format_seen;
      }
    }
    offset += d[0];
  }
  assert(offset == length && endpoints == 3U);
  assert(input_seen == 2U && feature_seen == 2U && output_seen == 2U && format_seen == 2U);
  assert(AUDIO_PMA_END == 488U && AUDIO_PMA_CAPTURE == 0x40U);
  assert(AUDIO_PMA_PLAYBACK == 0x104U && AUDIO_PMA_FEEDBACK == 0x1E4U);
  assert(AUDIO_OUT_BUFFER_BYTES == 224U && USB_MAX_EP0_SIZE == 16U);
  uint32_t slots = AUDIO_PMA_ISO_SLOTS(AUDIO_PMA_PLAYBACK);
  assert((slots & 0xFFFFU) == (slots >> 16));
}

static void test_usb_lifecycle(void)
{
  USBD_HandleTypeDef dev = { .dev_state = USBD_STATE_CONFIGURED };
  USBD_SetupReqTypedef request = { 0x01, USB_REQ_SET_INTERFACE, 1U, 1U, 0U };
  assert(USBD_AUDIO_RegisterInterface(&dev, &callbacks) == USBD_OK);
  assert(USBD_AUDIO.Init(&dev, 0U) == USBD_OK);
  assert(endpoint_open == 0U);
  test_sof(&dev);
  assert(transmissions == 0U);
  assert(USBD_AUDIO.Setup(&dev, &request) == USBD_OK);
  assert(endpoint_open == 1U && dev.ep_in[1].is_used == 1U);
  test_sof(&dev);
  assert(transmissions == 1U && transmitted_length == 192U);
  assert_silence(transmitted, transmitted_length);
  /* No completion: the next SOF must recover rather than deadlock. */
  test_sof(&dev);
  assert(transmissions == 2U);
  USBD_AUDIO.DataIn(&dev, 1U);
  assert(transmissions == 3U); /* Next packet is already armed before SOF. */

  request = (USBD_SetupReqTypedef){ 0xA1, USB_REQ_GET_INTERFACE, 0U, 0U, 1U };
  assert(USBD_AUDIO.Setup(&dev, &request) == USBD_FAIL); /* Wrong request type. */
  request.bmRequest = 0x81;
  assert(USBD_AUDIO.Setup(&dev, &request) == USBD_OK && control_buffer[0] == 0U);
  request.wIndex = 1U;
  assert(USBD_AUDIO.Setup(&dev, &request) == USBD_OK && control_buffer[0] == 1U);

  request = (USBD_SetupReqTypedef){ 0x21, AUDIO_REQ_SET_CUR, 0x0100, 0x0200, 1U };
  assert(USBD_AUDIO.Setup(&dev, &request) == USBD_OK && control_length == 1U);
  control_buffer[0] = 1U;
  USBD_AUDIO.EP0_RxReady(&dev);
  request.bmRequest = 0xA1;
  request.bRequest = AUDIO_REQ_GET_CUR;
  assert(USBD_AUDIO.Setup(&dev, &request) == USBD_OK && control_buffer[0] == 1U);
  request.wLength = 65U; /* Never let an invalid host length overrun EP0 storage. */
  assert(USBD_AUDIO.Setup(&dev, &request) == USBD_FAIL);
  request.wLength = 1U;
  request.wIndex = 0x0300;
  assert(USBD_AUDIO.Setup(&dev, &request) == USBD_FAIL);

  request = (USBD_SetupReqTypedef){ 0x01, USB_REQ_SET_INTERFACE, 2U, 1U, 0U };
  assert(USBD_AUDIO.Setup(&dev, &request) == USBD_FAIL);
  request.wValue = 0U;
  assert(USBD_AUDIO.Setup(&dev, &request) == USBD_OK && endpoint_open == 0U);
  test_sof(&dev);
  assert(transmissions == 3U && dev.ep_in[1].is_used == 0U);
  request.wValue = 1U;
  assert(USBD_AUDIO.Setup(&dev, &request) == USBD_OK);
  assert(USBD_AUDIO.DeInit(&dev, 0U) == USBD_OK);
  assert(endpoint_open == 0U && dev.pClassData == NULL);
  assert(USBD_AUDIO.DeInit(&dev, 0U) == USBD_OK);
  assert(errors == 4U);
}

static void test_capture(void)
{
  uint8_t packet[AUDIO_IN_MAX_PACKET];
  uint16_t size;
  AudioCapture_Init();
  assert(init_order == 3U);
  AudioCapture_SetStreaming(1U);
  AudioCapture_SetMute(0U);
  fill_dma(0U, 4095U);
  HAL_ADC_ConvHalfCpltCallback(&hadc1);
  assert(AudioCapture_GetPacket(packet, sizeof(packet)) == 192U);
  assert_silence(packet, 192U); /* Startup waits for three batches. */
  HAL_ADC_ConvCpltCallback(&hadc1);
  assert(AudioCapture_GetPacket(packet, sizeof(packet)) == 192U);
  assert_silence(packet, 192U);
  HAL_ADC_ConvHalfCpltCallback(&hadc1);
  size = AudioCapture_GetPacket(packet, sizeof(packet));
  for (unsigned i = 0U; i < size; i += 4U)
  {
    assert(le16(packet + i) == 0x8000U && le16(packet + i + 2U) == 0x7FF0U);
  }
  AudioCapture_SetMute(1U);
  HAL_ADC_ConvHalfCpltCallback(&hadc1);
  size = AudioCapture_GetPacket(packet, sizeof(packet));
  assert_silence(packet, size);
  AudioCapture_SetMute(0U);
  AudioCapture_SetStreaming(1U);
  fill_dma(2048U, 2048U);
  HAL_ADC_ConvHalfCpltCallback(&hadc1);
  HAL_ADC_ConvCpltCallback(&hadc1);
  HAL_ADC_ConvHalfCpltCallback(&hadc1);
  assert(AudioCapture_GetPacket(packet, sizeof(packet)) == 192U);
  assert_silence(packet, 192U); /* Mid-supply must be zero in signed PCM. */

  /* Ring wrap, underrun/re-prime, and host pause overflow stay within bounds. */
  for (unsigned i = 0U; i < 1000U; ++i)
  {
    HAL_ADC_ConvHalfCpltCallback(&hadc1);
    assert(AudioCapture_GetPacket(packet, sizeof(packet)) == 192U);
  }
  for (unsigned i = 0U; i < 10U; ++i)
  {
    size = AudioCapture_GetPacket(packet, sizeof(packet));
    assert(size == 188U || size == 192U);
  }
  HAL_ADC_ConvHalfCpltCallback(&hadc1);
  HAL_ADC_ConvCpltCallback(&hadc1);
  assert(AudioCapture_GetPacket(packet, sizeof(packet)) <= sizeof(packet));
  for (unsigned i = 0U; i < 100U; ++i)
  {
    HAL_ADC_ConvHalfCpltCallback(&hadc1);
  }
  assert(AudioCapture_GetPacket(packet, sizeof(packet)) == 196U);
  assert(AudioCapture_GetPacket(packet, 191U) == 0U);
  AudioCapture_SetStreaming(0U);
  assert(AudioCapture_GetPacket(packet, sizeof(packet)) == 192U);
  assert_silence(packet, 192U);
}

static void test_clock_drift(int ppm)
{
  uint8_t packet[AUDIO_IN_MAX_PACKET];
  uint64_t produced = 0U, consumed = 0U;
  uint64_t source_phase = 0U;
  unsigned adjusted = 0U;
  AudioCapture_SetStreaming(1U);
  fill_dma(2200U, 1900U);
  /* Simulate 60 seconds with the source oscillator +/- 1000 ppm from host SOF.
   * Sample accounting checks that rate adjustment preserves every source frame.
   */
  for (unsigned ms = 0U; ms < 60000U; ++ms)
  {
    source_phase += UINT64_C(48000) * (uint32_t)(1000000 + ppm);
    while (source_phase >= UINT64_C(48000000000))
    {
      source_phase -= UINT64_C(48000000000);
      HAL_ADC_ConvHalfCpltCallback(&hadc1);
      produced += 48U;
    }
    uint16_t size = AudioCapture_GetPacket(packet, sizeof(packet));
    assert(size == 188U || size == 192U || size == 196U);
    if (packet[0] != 0U || packet[1] != 0U)
    {
      assert(le16(packet) == (2200U - 2048U) * 16U);
      consumed += size / 4U;
      adjusted += (size != 192U);
    }
    else
    {
      assert(ms < 4U); /* Startup silence only; no steady-state underruns. */
    }
    assert(produced >= consumed && produced - consumed <= 192U);
  }
  assert(adjusted > 1000U);
  /* A second ADC batch can arrive in the same SOF at a clock phase boundary. */
  assert(produced - consumed >= 24U && produced - consumed <= 192U);
}

static void make_playback_packet(uint8_t *packet, unsigned frames,
                                  unsigned left, unsigned right)
{
  for (unsigned frame = 0U; frame < frames; ++frame)
  {
    uint16_t l = (uint16_t)((left << 6) ^ 0x8000U);
    uint16_t r = (uint16_t)((right << 6) ^ 0x8000U);
    packet[4U * frame] = (uint8_t)l;
    packet[4U * frame + 1U] = (uint8_t)(l >> 8);
    packet[4U * frame + 2U] = (uint8_t)r;
    packet[4U * frame + 3U] = (uint8_t)(r >> 8);
  }
}

static void assert_pwm_silence(const uint16_t *half)
{
  for (unsigned frame = 0U; frame < 48U; ++frame)
  {
    for (unsigned channel = 0U; channel < 2U; ++channel)
    {
      unsigned sum = 0U;
      for (unsigned phase = 0U; phase < 4U; ++phase)
      {
        uint16_t duty = half[frame * 8U + phase * 2U + channel];
        assert(duty == 187U || duty == 188U);
        sum += duty;
      }
      assert(sum == 750U); /* Exact 50% average, including accumulator carry. */
    }
  }
}

static void assert_pwm_frame(const uint16_t *frame, unsigned left, unsigned right)
{
  for (unsigned channel = 0U; channel < 2U; ++channel)
  {
    unsigned sum = 0U;
    for (unsigned phase = 0U; phase < 4U; ++phase)
    {
      assert(frame[phase * 2U + channel] <= 375U);
      sum += frame[phase * 2U + channel];
    }
    int32_t error = (int32_t)(sum * 1024U) -
                    (int32_t)(((channel == 0U) ? left : right) * 375U * 4U);
    assert(error > -1024 && error < 1024); /* At most one tick of integrated error. */
  }
}

static void test_playback_precision(void)
{
  uint8_t packet[AUDIO_OUT_MAX_PACKET];
  /* All 1024 output targets, opposite left/right values. Check integrated duty
   * against ideal pulse area rather than duplicating the error-diffusion loop.
   */
  for (unsigned code = 0U; code < 1024U; ++code)
  {
    AudioPlayback_SetStreaming(1U);
    AudioPlayback_SetMute(0U);
    make_playback_packet(packet, 48U, code, 1023U - code);
    for (unsigned batch = 0U; batch < 3U; ++batch)
    {
      assert(AudioPlayback_PushPacket(packet, 192U) == 1U);
    }
    HAL_TIM_PeriodElapsedHalfCpltCallback(&htim1);
    uint32_t area[2] = {0U, 0U};
    for (unsigned word = 0U; word < PWM_HALF_WORDS; ++word)
    {
      assert(pwm_dma_buffer[word] <= 375U);
      area[word % 2U] += pwm_dma_buffer[word];
    }
    for (unsigned channel = 0U; channel < 2U; ++channel)
    {
      int32_t error = (int32_t)(area[channel] * 1024U) -
        (int32_t)(((channel == 0U) ? code : 1023U - code) * 375U * 192U);
      assert(error > -1024 && error < 1024);
    }
  }
}

static void test_playback_order_and_recovery(void)
{
  uint8_t packet[AUDIO_OUT_MAX_PACKET];
  unsigned next = 0U, expected = 0U;
  AudioPlayback_SetStreaming(1U);
  AudioPlayback_SetMute(0U);
  /* Deliberately cross the monotonic counter wrap as well as physical ring wraps. */
  playback_read_frame = UINT32_MAX - 95U;
  playback_write_frame = playback_read_frame;
  for (unsigned batch = 0U; batch < 103U; ++batch)
  {
    for (unsigned frame = 0U; frame < 48U; ++frame)
    {
      make_playback_packet(packet + frame * 4U, 1U, next % 1024U,
                            1023U - next % 1024U);
      ++next;
    }
    assert(AudioPlayback_PushPacket(packet, 192U) == 1U);
    if (batch < 2U) continue;
    uint16_t *half = (batch % 2U == 0U) ? pwm_dma_buffer :
                                          &pwm_dma_buffer[PWM_HALF_WORDS];
    if (batch % 2U == 0U) HAL_TIM_PeriodElapsedHalfCpltCallback(&htim1);
    else HAL_TIM_PeriodElapsedCallback(&htim1);
    for (unsigned frame = 0U; frame < 48U; ++frame)
    {
      assert_pwm_frame(half + frame * 8U, expected % 1024U,
                        1023U - expected % 1024U);
      ++expected;
    }
  }
  assert(next - expected == 96U);
  AudioPlayback_SetMute(1U);
  uint32_t read = playback_read_frame;
  HAL_TIM_PeriodElapsedHalfCpltCallback(&htim1);
  assert(playback_read_frame == read + 48U);
  assert_pwm_silence(pwm_dma_buffer);
  AudioPlayback_SetMute(0U);
  HAL_TIM_PeriodElapsedCallback(&htim1); /* Drain last valid batch. */
  uint32_t underruns = playback_underruns;
  HAL_TIM_PeriodElapsedHalfCpltCallback(&htim1);
  assert(playback_underruns == underruns + 1U && playback_primed == 0U);
  assert_pwm_silence(pwm_dma_buffer);
  make_playback_packet(packet, 49U, 100U, 900U);
  assert(AudioPlayback_PushPacket(NULL, 4U) == 0U);
  assert(AudioPlayback_PushPacket(packet, 194U) == 0U);
  assert(AudioPlayback_PushPacket(packet, 200U) == 0U);
  assert(AudioPlayback_PushPacket(NULL, 0U) == 1U);
  assert(AudioPlayback_PushPacket(packet, 188U) == 1U);
  HAL_TIM_PeriodElapsedHalfCpltCallback(&htim1);
  assert_pwm_silence(pwm_dma_buffer); /* Re-prime, not a partial audio frame. */
  uint32_t overruns = playback_overruns;
  for (unsigned batch = 0U; batch < 20U; ++batch)
  {
    assert(AudioPlayback_PushPacket(packet, 196U) == 1U);
    assert(playback_write_frame - playback_read_frame <= 512U);
  }
  assert(playback_overruns > overruns);
  while (playback_write_frame - playback_read_frame < PLAYBACK_TARGET_FRAMES)
  {
    assert(AudioPlayback_PushPacket(packet, 196U) == 1U);
  }
  HAL_TIM_PeriodElapsedHalfCpltCallback(&htim1);
  for (unsigned frame = 0U; frame < 48U; ++frame)
  {
    assert_pwm_frame(pwm_dma_buffer + frame * 8U, 100U, 900U);
  }
  AudioPlayback_SetStreaming(0U);
  AudioPlayback_PushPacket(packet, 192U);
  assert(playback_write_frame == 0U);
  HAL_TIM_PeriodElapsedHalfCpltCallback(&htim1);
  HAL_TIM_PeriodElapsedCallback(&htim1);
  assert_pwm_silence(pwm_dma_buffer);
  assert_pwm_silence(&pwm_dma_buffer[PWM_HALF_WORDS]);
}

static void test_feedback_clock(void)
{
  for (int ppm = -1000; ppm <= 1000; ppm += 1000)
  {
    AudioPlayback_SetStreaming(1U);
    test_dwt.CYCCNT = UINT32_MAX - 100000U;
    uint32_t cycles = (uint32_t)(72000 + ppm * 72 / 1000);
    uint32_t feedback = AudioPlayback_GetFeedback();
    assert(feedback == 48U << 14);
    for (unsigned frame = 0U; frame < 2000U; ++frame)
    {
      test_dwt.CYCCNT += cycles;
      feedback = AudioPlayback_GetFeedback();
    }
    uint32_t ideal = (uint32_t)((((uint64_t)cycles << 14) + 750U) / 1500U);
    assert(feedback >= ideal - 1U && feedback <= ideal + 1U);
    test_dwt.CYCCNT += 72000000U; /* Resume after a second without SOF. */
    assert(AudioPlayback_GetFeedback() == feedback);
  }
}

static void test_full_duplex_usb(void)
{
  USBD_HandleTypeDef dev = { .dev_state = USBD_STATE_CONFIGURED };
  USBD_SetupReqTypedef request = { 0x01, USB_REQ_SET_INTERFACE, 1U, 2U, 0U };
  unsigned capture_count = transmissions, feedback_count = feedback_transmissions;
  assert(USBD_AUDIO_RegisterInterface(&dev, &callbacks) == USBD_OK);
  assert(USBD_AUDIO.Init(&dev, 0U) == USBD_OK);

  /* Endpoint/open/receive failures roll back playback without disturbing capture. */
  fail_open_ep = AUDIO_FEEDBACK_EP;
  assert(USBD_AUDIO.Setup(&dev, &request) == USBD_FAIL);
  assert(playback_endpoint_open == 0U && feedback_endpoint_open == 0U);
  fail_open_ep = 0U;
  fail_receive = 1U;
  assert(USBD_AUDIO.Setup(&dev, &request) == USBD_FAIL);
  assert(playback_streaming == 0U && playback_endpoint_open == 0U);
  fail_receive = 0U;
  fail_transmit_ep = AUDIO_FEEDBACK_EP;
  assert(USBD_AUDIO.Setup(&dev, &request) == USBD_FAIL);
  assert(playback_streaming == 0U && playback_endpoint_open == 0U &&
         feedback_endpoint_open == 0U && dev.ep_in[3].is_used == 0U);
  fail_transmit_ep = 0U;
  assert(USBD_AUDIO.Setup(&dev, &request) == USBD_OK);
  assert(playback_endpoint_open == 1U && feedback_endpoint_open == 1U);
  assert(endpoint_open == 0U && dev.ep_out[2].is_used == 1U && dev.ep_in[3].is_used == 1U);
  assert(test_sof(&dev) == USBD_OK);
  assert(transmissions == capture_count && feedback_transmissions == feedback_count + 1U);
  assert(transmitted_feedback[0] == 0U && transmitted_feedback[1] == 0U &&
         transmitted_feedback[2] == 12U);
  /* An unpolled feedback packet must also recover on the next SOF. */
  assert(test_sof(&dev) == USBD_OK);
  USBD_AUDIO.DataIn(&dev, 3U);

  unsigned packets = received_packets, preparations = receive_preparations;
  make_playback_packet(receive_buffer, 48U, 100U, 900U);
  for (unsigned batch = 0U; batch < 3U; ++batch)
  {
    receive_length = 192U;
    assert(USBD_AUDIO.DataOut(&dev, 2U) == USBD_OK);
  }
  assert(received_packets == packets + 3U && receive_preparations == preparations + 3U);
  for (unsigned bad = 0U; bad < 2U; ++bad)
  {
    receive_length = (bad == 0U) ? 194U : 224U;
    assert(USBD_AUDIO.DataOut(&dev, 2U) == USBD_OK);
  }
  assert(received_packets == packets + 3U && receive_preparations == preparations + 5U);
  HAL_TIM_PeriodElapsedHalfCpltCallback(&htim1);
  assert_pwm_frame(pwm_dma_buffer, 100U, 900U);

  request.wIndex = 1U;
  uint32_t queued = playback_write_frame - playback_read_frame;
  fail_transmit_ep = AUDIO_IN_EP;
  assert(USBD_AUDIO.Setup(&dev, &request) == USBD_FAIL);
  assert(endpoint_open == 0U && streaming == 0U && dev.ep_in[1].is_used == 0U);
  assert(playback_endpoint_open == 1U && playback_write_frame - playback_read_frame == queued);
  fail_transmit_ep = 0U;
  assert(USBD_AUDIO.Setup(&dev, &request) == USBD_OK);
  assert(playback_write_frame - playback_read_frame == queued);
  fill_dma(2200U, 1900U);
  for (unsigned batch = 0U; batch < 3U; ++batch) HAL_ADC_ConvHalfCpltCallback(&hadc1);
  USBD_AUDIO.DataIn(&dev, 1U); /* Poll startup silence; pre-arm the ADC packet. */
  assert(test_sof(&dev) == USBD_OK);
  assert(transmissions == capture_count + 2U && transmitted_length == 192U);
  assert(le16(transmitted) == (2200U - 2048U) * 16U);
  assert(feedback_transmissions == feedback_count + 3U);
  USBD_AUDIO.DataIn(&dev, 1U);
  USBD_AUDIO.DataIn(&dev, 3U);
  fail_transmit_ep = AUDIO_FEEDBACK_EP;
  assert(USBD_AUDIO.DataIn(&dev, 3U) == USBD_FAIL);
  assert(test_sof(&dev) == USBD_FAIL);
  assert(((USBD_AUDIO_HandleTypeDef *)dev.pClassData)->feedback_busy == 0U);
  fail_transmit_ep = 0U;
  USBD_AUDIO.DataIn(&dev, 1U);

  /* Playback mute does not affect capture mute or its stream lifecycle. */
  request = (USBD_SetupReqTypedef){ 0x21, AUDIO_REQ_SET_CUR, 0x0100, 0x0500, 1U };
  assert(USBD_AUDIO.Setup(&dev, &request) == USBD_OK);
  control_buffer[0] = 1U;
  assert(USBD_AUDIO.EP0_RxReady(&dev) == USBD_OK);
  assert(playback_muted == 1U && muted == 0U);
  HAL_TIM_PeriodElapsedCallback(&htim1);
  assert_pwm_silence(&pwm_dma_buffer[PWM_HALF_WORDS]);
  request = (USBD_SetupReqTypedef){ 0x01, USB_REQ_SET_INTERFACE, 0U, 1U, 0U };
  assert(USBD_AUDIO.Setup(&dev, &request) == USBD_OK);
  assert(endpoint_open == 0U && playback_endpoint_open == 1U);
  assert(test_sof(&dev) == USBD_OK);
  request.wValue = 1U;
  assert(USBD_AUDIO.Setup(&dev, &request) == USBD_OK);
  request.wValue = 0U;
  request.wIndex = 2U;
  assert(USBD_AUDIO.Setup(&dev, &request) == USBD_OK);
  assert(endpoint_open == 1U && playback_endpoint_open == 0U && feedback_endpoint_open == 0U);
  assert(streaming == 1U && playback_streaming == 0U);
  assert(USBD_AUDIO.DeInit(&dev, 0U) == USBD_OK);
  assert(endpoint_open == 0U && dev.pClassData == NULL);
}

static void test_usb_early_poll_timing(void)
{
  USBD_HandleTypeDef dev = { .dev_state = USBD_STATE_CONFIGURED };
  USBD_SetupReqTypedef request = { 0x01, USB_REQ_SET_INTERFACE, 1U, 1U, 0U };
  usb_frame = AUDIO_USB_FRAME_MASK - 1U;
  assert(USBD_AUDIO_RegisterInterface(&dev, &callbacks) == USBD_OK);
  assert(USBD_AUDIO.Init(&dev, 0U) == USBD_OK);
  assert(USBD_AUDIO.Setup(&dev, &request) == USBD_OK);
  request.wIndex = 2U;
  assert(USBD_AUDIO.Setup(&dev, &request) == USBD_OK);
  unsigned capture_close_count = capture_closes;
  unsigned feedback_close_count = feedback_closes;
  unsigned expected = 0U, produced = 0U;

  /* The host may poll before the pending SOF interrupt is serviced. Both IN
   * packets must be ready already. Vary the interrupt order and cross the
   * hardware's 11-bit frame wrap twice, checking real PCM continuity too.
   */
  for (unsigned ms = 0U; ms < 4200U; ++ms)
  {
    usb_frame = (usb_frame + 1U) & AUDIO_USB_FRAME_MASK;
    test_dwt.CYCCNT += 72000U;
    for (unsigned frame = 0U; frame < AUDIO_SAMPLES_PER_PACKET; ++frame)
    {
      unsigned code = (produced + frame) % 512U;
      for (unsigned half = 0U; half < 2U; ++half)
      {
        unsigned index = half * ADC_BATCH_SAMPLES + frame * 2U;
        adc_dma_buffer[index] = (uint16_t)(2048U + code);
        adc_dma_buffer[index + 1U] = (uint16_t)(2000U - code);
      }
    }
    produced += AUDIO_SAMPLES_PER_PACKET;
    HAL_ADC_ConvHalfCpltCallback(&hadc1);
    make_playback_packet(receive_buffer, 48U, 100U, 900U);
    receive_length = 192U;
    assert(USBD_AUDIO.DataOut(&dev, 2U) == USBD_OK);
    HAL_TIM_PeriodElapsedHalfCpltCallback(&htim1);

    unsigned capture_count = transmissions, feedback_count = feedback_transmissions;
    if (ms % 2U == 0U)
    {
      assert(USBD_AUDIO.SOF(&dev) == USBD_OK);
      assert(transmissions == capture_count && feedback_transmissions == feedback_count);
    }
    assert(((USBD_AUDIO_HandleTypeDef *)dev.pClassData)->tx_busy == 1U);
    assert(((USBD_AUDIO_HandleTypeDef *)dev.pClassData)->feedback_busy == 1U);
    if (ms < 3U)
    {
      assert_silence(transmitted, transmitted_length);
    }
    else
    {
      for (unsigned frame = 0U; frame < transmitted_length / AUDIO_FRAME_BYTES; ++frame)
      {
        unsigned code = expected++ % 512U;
        assert(le16(transmitted + frame * 4U) == code * 16U);
        assert(le16(transmitted + frame * 4U + 2U) ==
               (uint16_t)((-48 - (int32_t)code) * 16));
      }
    }
    assert(USBD_AUDIO.DataIn(&dev, 1U) == USBD_OK);
    assert(USBD_AUDIO.DataIn(&dev, 3U) == USBD_OK);
    assert(transmissions == capture_count + 1U && feedback_transmissions == feedback_count + 1U);
    if (ms % 2U != 0U)
    {
      assert(USBD_AUDIO.SOF(&dev) == USBD_OK);
      assert(transmissions == capture_count + 1U && feedback_transmissions == feedback_count + 1U);
    }
    assert(capture_closes == capture_close_count && feedback_closes == feedback_close_count);
  }
  assert(expected > 200000U);

  /* Missing a host poll retires just that packet, on the following frame. */
  unsigned capture_count = transmissions, feedback_count = feedback_transmissions;
  assert(test_sof(&dev) == USBD_OK);
  assert(transmissions == capture_count && feedback_transmissions == feedback_count);
  assert(test_sof(&dev) == USBD_OK);
  assert(transmissions == capture_count + 1U && feedback_transmissions == feedback_count + 1U);
  assert(capture_closes == capture_close_count + 1U &&
         feedback_closes == feedback_close_count + 1U);
  /* A long suspend can advance the hardware counter by more than half its
   * range; recovery must still happen at the first SOF after resume.
   */
  usb_frame = (usb_frame + 1500U) & AUDIO_USB_FRAME_MASK;
  test_dwt.CYCCNT += 108000000U;
  assert(USBD_AUDIO.SOF(&dev) == USBD_OK);
  assert(transmissions == capture_count + 2U && feedback_transmissions == feedback_count + 2U);
  assert(USBD_AUDIO.IsoINIncomplete(&dev, 1U) == USBD_OK);
  assert(USBD_AUDIO.IsoINIncomplete(&dev, 3U) == USBD_OK);
  assert(test_sof(&dev) == USBD_OK);
  assert(transmissions == capture_count + 3U && feedback_transmissions == feedback_count + 3U);
  assert(USBD_AUDIO.DeInit(&dev, 0U) == USBD_OK);
}

static void test_playback_host_feedback(int ppm)
{
  uint8_t packet[AUDIO_OUT_MAX_PACKET];
  uint64_t host_phase = 0U, device_phase = 0U;
  uint32_t feedback = 48U << 14;
  uint32_t underruns = playback_underruns, overruns = playback_overruns;
  AudioPlayback_SetStreaming(1U);
  AudioPlayback_SetMute(0U);
  make_playback_packet(packet, 49U, 100U, 900U);
  /* Model a host delivering the advertised fractional rate and a device clock
   * drifting +/-1000 ppm. Sixty seconds also crosses the DWT cycle-counter wrap.
   */
  for (unsigned ms = 0U; ms < 60000U; ++ms)
  {
    test_dwt.CYCCNT += (uint32_t)(72000 + ppm * 72 / 1000);
    host_phase += feedback;
    uint16_t frames = (uint16_t)(host_phase >> 14);
    host_phase &= 16383U;
    assert(frames >= 47U && frames <= 49U);
    assert(AudioPlayback_PushPacket(packet, frames * 4U) == 1U);
    device_phase += (uint64_t)(1000000 + ppm);
    while (device_phase >= 1000000U)
    {
      device_phase -= 1000000U;
      HAL_TIM_PeriodElapsedHalfCpltCallback(&htim1);
    }
    feedback = AudioPlayback_GetFeedback();
    assert(playback_write_frame - playback_read_frame <= 256U);
    if (ms > 10U) assert(playback_primed == 1U);
  }
  assert(playback_underruns == underruns && playback_overruns == overruns);
}

int main(void)
{
  AudioPlayback_Init();
  assert(pwm_init_order == 3U && test_core_debug.DEMCR != 0U && test_dwt.CTRL != 0U);
  assert_pwm_silence(pwm_dma_buffer);
  test_descriptors();
  test_usb_lifecycle();
  test_capture();
  test_clock_drift(1000);
  test_clock_drift(-1000);
  test_playback_precision();
  test_playback_order_and_recovery();
  test_feedback_clock();
  test_full_duplex_usb();
  test_usb_early_poll_timing();
  test_playback_host_feedback(1000);
  test_playback_host_feedback(-1000);
  assert(irq_mask == 0U);
  puts("Full-duplex USB, independent controls, PCM capture, 10-bit PWM, buffer recovery and clock drift: PASS");
  return 0;
}
