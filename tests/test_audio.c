#include <assert.h>
#include <stdio.h>

/* Exercise the production code with hardware replaced by host stubs. Including
 * the capture implementation also lets us populate the simulated ADC DMA buffer.
 */
#include "../Core/Src/audio_capture.c"
#include "../Middlewares/ST/STM32_USB_Device_Library/Class/AUDIO/Src/usbd_audio.c"

ADC_HandleTypeDef hadc1 = { ADC1 };
TIM_HandleTypeDef htim3;
static unsigned init_order;
static unsigned endpoint_open;
static unsigned transmissions;
static unsigned errors;
static uint8_t *control_buffer;
static uint16_t control_length;
static uint16_t transmitted_length;
static uint8_t transmitted[AUDIO_IN_MAX_PACKET];
static uint32_t irq_mask;

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

USBD_StatusTypeDef USBD_LL_OpenEP(USBD_HandleTypeDef *dev, uint8_t ep,
                                 uint8_t type, uint16_t size)
{
  UNUSED(dev);
  assert(ep == 0x81U && type == USBD_EP_TYPE_ISOC && size == 196U);
  assert(endpoint_open == 0U);
  endpoint_open = 1U;
  return USBD_OK;
}
USBD_StatusTypeDef USBD_LL_CloseEP(USBD_HandleTypeDef *dev, uint8_t ep)
{
  UNUSED(dev);
  assert(ep == 0x81U && endpoint_open == 1U);
  endpoint_open = 0U;
  return USBD_OK;
}
USBD_StatusTypeDef USBD_LL_Transmit(USBD_HandleTypeDef *dev, uint8_t ep,
                                   uint8_t *buffer, uint16_t size)
{
  UNUSED(dev);
  assert(endpoint_open == 1U && ep == 0x81U && size <= sizeof(transmitted));
  memcpy(transmitted, buffer, size);
  transmitted_length = size;
  ++transmissions;
  return USBD_OK;
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
  return USBD_OK;
}
static int8_t test_deinit(uint32_t options)
{
  UNUSED(options);
  AudioCapture_SetStreaming(0U);
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
static USBD_AUDIO_ItfTypeDef callbacks =
{
  .Init = test_init,
  .DeInit = test_deinit,
  .AudioCmd = test_command,
  .MuteCtl = test_mute,
  .GetPacket = AudioCapture_GetPacket,
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
  assert(length == 110U && le16(desc + 2U) == length && desc[4] == 2U);
  while (offset < length)
  {
    uint8_t *d = desc + offset;
    assert(d[0] >= 2U && offset + d[0] <= length);
    if (d[1] == USB_DESC_TYPE_INTERFACE)
    {
      interface = d[2];
      assert(d[3] <= 1U);
      assert(d[4] == ((interface == 1U && d[3] == 1U) ? 1U : 0U));
    }
    else if (d[1] == USB_DESC_TYPE_ENDPOINT)
    {
      assert(interface == 1U && d[2] == 0x81U && d[3] == 0x05U);
      assert(le16(d + 4U) == 196U && d[6] == 1U);
      ++endpoints;
    }
    else if (d[1] == AUDIO_INTERFACE_DESCRIPTOR_TYPE && interface == 0U)
    {
      switch (d[2])
      {
        case AUDIO_CONTROL_HEADER:
          assert(le16(d + 5U) == 40U && d[8] == 1U);
          break;
        case AUDIO_CONTROL_INPUT_TERMINAL:
          assert(d[3] == 1U && le16(d + 4U) == 0x0603U);
          assert(d[7] == 2U && le16(d + 8U) == 3U);
          ++input_seen;
          break;
        case AUDIO_CONTROL_FEATURE_UNIT:
          assert(d[0] == 10U && d[3] == 2U && d[4] == 1U && d[6] == 1U);
          assert(d[7] == 0U && d[8] == 0U);
          ++feature_seen;
          break;
        case AUDIO_CONTROL_OUTPUT_TERMINAL:
          assert(d[3] == 3U && le16(d + 4U) == 0x0101U && d[7] == 2U);
          ++output_seen;
          break;
      }
    }
    else if (d[1] == AUDIO_INTERFACE_DESCRIPTOR_TYPE && interface == 1U)
    {
      if (d[2] == AUDIO_STREAMING_GENERAL)
      {
        assert(d[3] == 3U);
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
  assert(offset == length && endpoints == 1U);
  assert(input_seen == 1U && feature_seen == 1U && output_seen == 1U && format_seen == 1U);
}

static void test_usb_lifecycle(void)
{
  USBD_HandleTypeDef dev = { .dev_state = USBD_STATE_CONFIGURED };
  USBD_SetupReqTypedef request = { 0x01, USB_REQ_SET_INTERFACE, 1U, 1U, 0U };
  assert(USBD_AUDIO_RegisterInterface(&dev, &callbacks) == USBD_OK);
  assert(USBD_AUDIO.Init(&dev, 0U) == USBD_OK);
  assert(endpoint_open == 0U);
  USBD_AUDIO.SOF(&dev);
  assert(transmissions == 0U);
  assert(USBD_AUDIO.Setup(&dev, &request) == USBD_OK);
  assert(endpoint_open == 1U && dev.ep_in[1].is_used == 1U);
  USBD_AUDIO.SOF(&dev);
  assert(transmissions == 1U && transmitted_length == 192U);
  assert_silence(transmitted, transmitted_length);
  /* No completion: the next SOF must recover rather than deadlock. */
  USBD_AUDIO.SOF(&dev);
  assert(transmissions == 2U);
  USBD_AUDIO.DataIn(&dev, 1U);

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
  USBD_AUDIO.SOF(&dev);
  assert(transmissions == 2U && dev.ep_in[1].is_used == 0U);
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

int main(void)
{
  test_descriptors();
  test_usb_lifecycle();
  test_capture();
  test_clock_drift(1000);
  test_clock_drift(-1000);
  assert(irq_mask == 0U);
  puts("USB descriptors, controls, streaming lifecycle, PCM capture and clock drift: PASS");
  return 0;
}
