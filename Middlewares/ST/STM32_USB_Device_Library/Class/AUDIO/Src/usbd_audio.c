/**
  ******************************************************************************
  * @file    usbd_audio.c
  * @author  MCD Application Team
  * @brief   USB Audio Class 1.0, stereo ADC capture and asynchronous PWM playback.
  ******************************************************************************
  * @attention
  *
  * <h2><center>&copy; Copyright (c) 2015 STMicroelectronics.
  * All rights reserved.</center></h2>
  *
  * This software component is licensed by ST under Ultimate Liberty license
  * SLA0044, the "License"; You may not use this file except in compliance with
  * the License. You may obtain a copy of the License at:
  *                      www.st.com/SLA0044
  *
  ******************************************************************************
  */

#include "usbd_audio.h"
#include "usbd_ctlreq.h"

#define AUDIO_SAMPLE_FREQ(frq) (uint8_t)(frq), (uint8_t)((frq) >> 8), (uint8_t)((frq) >> 16)

static uint8_t USBD_AUDIO_Init(USBD_HandleTypeDef *pdev, uint8_t cfgidx);
static uint8_t USBD_AUDIO_DeInit(USBD_HandleTypeDef *pdev, uint8_t cfgidx);
static uint8_t USBD_AUDIO_Setup(USBD_HandleTypeDef *pdev, USBD_SetupReqTypedef *req);
static uint8_t USBD_AUDIO_EP0_RxReady(USBD_HandleTypeDef *pdev);
static uint8_t USBD_AUDIO_DataIn(USBD_HandleTypeDef *pdev, uint8_t epnum);
static uint8_t USBD_AUDIO_DataOut(USBD_HandleTypeDef *pdev, uint8_t epnum);
static uint8_t USBD_AUDIO_SOF(USBD_HandleTypeDef *pdev);
static uint8_t USBD_AUDIO_IsoINIncomplete(USBD_HandleTypeDef *pdev, uint8_t epnum);
static uint8_t *USBD_AUDIO_GetCfgDesc(uint16_t *length);
static uint8_t *USBD_AUDIO_GetDeviceQualifierDesc(uint16_t *length);

USBD_ClassTypeDef USBD_AUDIO =
{
  USBD_AUDIO_Init,
  USBD_AUDIO_DeInit,
  USBD_AUDIO_Setup,
  NULL,
  USBD_AUDIO_EP0_RxReady,
  USBD_AUDIO_DataIn,
  USBD_AUDIO_DataOut,
  USBD_AUDIO_SOF,
  USBD_AUDIO_IsoINIncomplete,
  NULL,
  USBD_AUDIO_GetCfgDesc,
  USBD_AUDIO_GetCfgDesc,
  USBD_AUDIO_GetCfgDesc,
  USBD_AUDIO_GetDeviceQualifierDesc,
};

/* Capture: line input (1) -> mute (2) -> USB (3).
 * Playback: USB (4) -> mute (5) -> line output (6).
 * AudioControl interface 0 owns independently selectable streaming interfaces 1/2.
 */
__ALIGN_BEGIN static uint8_t USBD_AUDIO_CfgDesc[] __ALIGN_END =
{
  /* Configuration: three interfaces, bus powered, 100 mA. */
  0x09, USB_DESC_TYPE_CONFIGURATION,
  LOBYTE(USB_AUDIO_CONFIG_DESC_SIZ), HIBYTE(USB_AUDIO_CONFIG_DESC_SIZ),
  0x03, 0x01, 0x00, 0x80, 0x32,

  /* Standard AudioControl interface. */
  0x09, USB_DESC_TYPE_INTERFACE, 0x00, 0x00, 0x00,
  USB_DEVICE_CLASS_AUDIO, AUDIO_SUBCLASS_AUDIOCONTROL, AUDIO_PROTOCOL_UNDEFINED, 0x00,

  /* UAC 1.0 header: 10 + 2 * (12 + 10 + 9) = 72 AC bytes. */
  0x0A, AUDIO_INTERFACE_DESCRIPTOR_TYPE, AUDIO_CONTROL_HEADER,
  0x00, 0x01, 0x48, 0x00, 0x02, AUDIO_CAPTURE_INTERFACE, AUDIO_PLAYBACK_INTERFACE,

  /* Physical input terminal: line connector (0x0603), left/right stereo. */
  0x0C, AUDIO_INTERFACE_DESCRIPTOR_TYPE, AUDIO_CONTROL_INPUT_TERMINAL,
  0x01, 0x03, 0x06, 0x00, AUDIO_CHANNELS, 0x03, 0x00, 0x00, 0x00,

  /* Feature unit: master mute, no individual channel controls. */
  0x0A, AUDIO_INTERFACE_DESCRIPTOR_TYPE, AUDIO_CONTROL_FEATURE_UNIT,
  AUDIO_FEATURE_UNIT_ID, 0x01, 0x01, AUDIO_CONTROL_MUTE, 0x00, 0x00, 0x00,

  /* Output terminal: USB streaming (0x0101), sourced from feature unit 2. */
  0x09, AUDIO_INTERFACE_DESCRIPTOR_TYPE, AUDIO_CONTROL_OUTPUT_TERMINAL,
  0x03, 0x01, 0x01, 0x00, AUDIO_FEATURE_UNIT_ID, 0x00,

  /* USB playback input terminal: stereo. */
  0x0C, AUDIO_INTERFACE_DESCRIPTOR_TYPE, AUDIO_CONTROL_INPUT_TERMINAL,
  0x04, 0x01, 0x01, 0x00, AUDIO_CHANNELS, 0x03, 0x00, 0x00, 0x00,

  /* Independent playback master mute. */
  0x0A, AUDIO_INTERFACE_DESCRIPTOR_TYPE, AUDIO_CONTROL_FEATURE_UNIT,
  AUDIO_PLAYBACK_FEATURE_UNIT_ID, 0x04, 0x01, AUDIO_CONTROL_MUTE, 0x00, 0x00, 0x00,

  /* Line output terminal, sourced from playback mute. */
  0x09, AUDIO_INTERFACE_DESCRIPTOR_TYPE, AUDIO_CONTROL_OUTPUT_TERMINAL,
  0x06, 0x03, 0x06, 0x00, AUDIO_PLAYBACK_FEATURE_UNIT_ID, 0x00,

  /* AudioStreaming alternate 0: zero bandwidth. */
  0x09, USB_DESC_TYPE_INTERFACE, 0x01, 0x00, 0x00,
  USB_DEVICE_CLASS_AUDIO, AUDIO_SUBCLASS_AUDIOSTREAMING, AUDIO_PROTOCOL_UNDEFINED, 0x00,

  /* AudioStreaming alternate 1: one capture endpoint. */
  0x09, USB_DESC_TYPE_INTERFACE, 0x01, 0x01, 0x01,
  USB_DEVICE_CLASS_AUDIO, AUDIO_SUBCLASS_AUDIOSTREAMING, AUDIO_PROTOCOL_UNDEFINED, 0x00,

  /* Link to USB output terminal 3; PCM format. */
  0x07, AUDIO_INTERFACE_DESCRIPTOR_TYPE, AUDIO_STREAMING_GENERAL,
  0x03, 0x01, 0x01, 0x00,

  /* Type I: stereo, signed 16-bit little-endian PCM at a fixed 48 kHz. */
  0x0B, AUDIO_INTERFACE_DESCRIPTOR_TYPE, AUDIO_STREAMING_FORMAT_TYPE,
  AUDIO_FORMAT_TYPE_I, AUDIO_CHANNELS, AUDIO_BYTES_PER_SAMPLE, 16, 0x01,
  AUDIO_SAMPLE_FREQ(USBD_AUDIO_FREQ),

  /* Asynchronous isochronous IN, up to 49 stereo frames every 1 ms.
   * Capture packet lengths report the source clock rate; no feedback EP is needed.
   */
  0x09, USB_DESC_TYPE_ENDPOINT, AUDIO_IN_EP, 0x05,
  LOBYTE(AUDIO_IN_MAX_PACKET), HIBYTE(AUDIO_IN_MAX_PACKET), 0x01, 0x00, 0x00,

  /* No sampling-frequency or pitch controls; fixed sampling rate. */
  0x07, AUDIO_ENDPOINT_DESCRIPTOR_TYPE, AUDIO_ENDPOINT_GENERAL,
  0x00, 0x00, 0x00, 0x00,

  /* Playback alternate 0: idle; alternate 1: data OUT plus explicit feedback IN. */
  0x09, USB_DESC_TYPE_INTERFACE, AUDIO_PLAYBACK_INTERFACE, 0x00, 0x00,
  USB_DEVICE_CLASS_AUDIO, AUDIO_SUBCLASS_AUDIOSTREAMING, AUDIO_PROTOCOL_UNDEFINED, 0x00,
  0x09, USB_DESC_TYPE_INTERFACE, AUDIO_PLAYBACK_INTERFACE, 0x01, 0x02,
  USB_DEVICE_CLASS_AUDIO, AUDIO_SUBCLASS_AUDIOSTREAMING, AUDIO_PROTOCOL_UNDEFINED, 0x00,
  0x07, AUDIO_INTERFACE_DESCRIPTOR_TYPE, AUDIO_STREAMING_GENERAL,
  0x04, 0x01, 0x01, 0x00,
  0x0B, AUDIO_INTERFACE_DESCRIPTOR_TYPE, AUDIO_STREAMING_FORMAT_TYPE,
  AUDIO_FORMAT_TYPE_I, AUDIO_CHANNELS, AUDIO_BYTES_PER_SAMPLE, 16, 0x01,
  AUDIO_SAMPLE_FREQ(USBD_AUDIO_FREQ),
  0x09, USB_DESC_TYPE_ENDPOINT, AUDIO_OUT_EP, 0x05,
  LOBYTE(AUDIO_OUT_MAX_PACKET), HIBYTE(AUDIO_OUT_MAX_PACKET), 0x01, 0x00, AUDIO_FEEDBACK_EP,
  0x07, AUDIO_ENDPOINT_DESCRIPTOR_TYPE, AUDIO_ENDPOINT_GENERAL,
  0x00, 0x00, 0x00, 0x00,
  /* UAC1 feedback is three bytes, Q10.14 samples per USB frame. */
  0x09, USB_DESC_TYPE_ENDPOINT, AUDIO_FEEDBACK_EP, 0x11,
  AUDIO_FEEDBACK_BYTES, 0x00, 0x01, 0x01, 0x00,
};

_Static_assert(sizeof(USBD_AUDIO_CfgDesc) == USB_AUDIO_CONFIG_DESC_SIZ,
               "USB audio configuration descriptor length mismatch");

__ALIGN_BEGIN static uint8_t USBD_AUDIO_DeviceQualifierDesc[] __ALIGN_END =
{
  USB_LEN_DEV_QUALIFIER_DESC, USB_DESC_TYPE_DEVICE_QUALIFIER,
  0x00, 0x02, 0x00, 0x00, 0x00, USB_MAX_EP0_SIZE, 0x01, 0x00,
};

static uint8_t USBD_AUDIO_Init(USBD_HandleTypeDef *pdev, uint8_t cfgidx)
{
  USBD_AUDIO_ItfTypeDef *fops = (USBD_AUDIO_ItfTypeDef *)pdev->pUserData;
  UNUSED(cfgidx);

  if ((fops == NULL) || (fops->Init == NULL) || (fops->DeInit == NULL) ||
      (fops->AudioCmd == NULL) || (fops->MuteCtl == NULL) ||
      (fops->GetPacket == NULL) || (fops->PlaybackSetStreaming == NULL) ||
      (fops->PlaybackMuteCtl == NULL) || (fops->PutPacket == NULL) ||
      (fops->GetFeedback == NULL))
  {
    return USBD_FAIL;
  }
  pdev->pClassData = USBD_malloc(sizeof(USBD_AUDIO_HandleTypeDef));
  if (pdev->pClassData == NULL)
  {
    return USBD_FAIL;
  }
  memset(pdev->pClassData, 0, sizeof(USBD_AUDIO_HandleTypeDef));
  if (fops->Init(USBD_AUDIO_FREQ, AUDIO_DEFAULT_VOLUME, 0U) != USBD_OK)
  {
    USBD_free(pdev->pClassData);
    pdev->pClassData = NULL;
    return USBD_FAIL;
  }
  /* Endpoint stays closed until the host selects alternate setting 1. */
  return USBD_OK;
}

static void USBD_AUDIO_StopCapture(USBD_HandleTypeDef *pdev)
{
  USBD_AUDIO_HandleTypeDef *haudio = (USBD_AUDIO_HandleTypeDef *)pdev->pClassData;
  if (haudio->alt_setting != 0U)
  {
    ((USBD_AUDIO_ItfTypeDef *)pdev->pUserData)->AudioCmd(NULL, 0U, AUDIO_CMD_STOP);
    USBD_LL_CloseEP(pdev, AUDIO_IN_EP);
    pdev->ep_in[AUDIO_IN_EP & 0x0FU].is_used = 0U;
  }
  haudio->alt_setting = 0U;
  haudio->tx_busy = 0U;
}

static void USBD_AUDIO_StopPlayback(USBD_HandleTypeDef *pdev)
{
  USBD_AUDIO_HandleTypeDef *haudio = (USBD_AUDIO_HandleTypeDef *)pdev->pClassData;
  if (haudio->playback_alt_setting != 0U)
  {
    ((USBD_AUDIO_ItfTypeDef *)pdev->pUserData)->PlaybackSetStreaming(0U);
    USBD_LL_CloseEP(pdev, AUDIO_OUT_EP);
    USBD_LL_CloseEP(pdev, AUDIO_FEEDBACK_EP);
    pdev->ep_out[AUDIO_OUT_EP & 0x0FU].is_used = 0U;
    pdev->ep_in[AUDIO_FEEDBACK_EP & 0x0FU].is_used = 0U;
  }
  haudio->playback_alt_setting = 0U;
  haudio->feedback_busy = 0U;
}

static uint8_t USBD_AUDIO_StartPlayback(USBD_HandleTypeDef *pdev)
{
  USBD_AUDIO_HandleTypeDef *haudio = (USBD_AUDIO_HandleTypeDef *)pdev->pClassData;
  USBD_AUDIO_ItfTypeDef *fops = (USBD_AUDIO_ItfTypeDef *)pdev->pUserData;
  if (USBD_LL_OpenEP(pdev, AUDIO_OUT_EP, USBD_EP_TYPE_ISOC,
                    AUDIO_OUT_MAX_PACKET) != USBD_OK)
  {
    return USBD_FAIL;
  }
  if (USBD_LL_OpenEP(pdev, AUDIO_FEEDBACK_EP, USBD_EP_TYPE_ISOC,
                    AUDIO_FEEDBACK_BYTES) != USBD_OK)
  {
    USBD_LL_CloseEP(pdev, AUDIO_OUT_EP);
    return USBD_FAIL;
  }
  fops->PlaybackSetStreaming(1U);
  if (USBD_LL_PrepareReceive(pdev, AUDIO_OUT_EP, haudio->out_buffer,
                             AUDIO_OUT_MAX_PACKET) != USBD_OK)
  {
    fops->PlaybackSetStreaming(0U);
    USBD_LL_CloseEP(pdev, AUDIO_OUT_EP);
    USBD_LL_CloseEP(pdev, AUDIO_FEEDBACK_EP);
    return USBD_FAIL;
  }
  pdev->ep_out[AUDIO_OUT_EP & 0x0FU].is_used = 1U;
  pdev->ep_out[AUDIO_OUT_EP & 0x0FU].maxpacket = AUDIO_OUT_MAX_PACKET;
  pdev->ep_in[AUDIO_FEEDBACK_EP & 0x0FU].is_used = 1U;
  pdev->ep_in[AUDIO_FEEDBACK_EP & 0x0FU].maxpacket = AUDIO_FEEDBACK_BYTES;
  haudio->playback_alt_setting = 1U;
  return USBD_OK;
}

static uint8_t USBD_AUDIO_DeInit(USBD_HandleTypeDef *pdev, uint8_t cfgidx)
{
  UNUSED(cfgidx);
  if (pdev->pClassData != NULL)
  {
    USBD_AUDIO_StopCapture(pdev);
    USBD_AUDIO_StopPlayback(pdev);
    ((USBD_AUDIO_ItfTypeDef *)pdev->pUserData)->DeInit(0U);
    USBD_free(pdev->pClassData);
    pdev->pClassData = NULL;
  }
  return USBD_OK;
}

static uint8_t USBD_AUDIO_Setup(USBD_HandleTypeDef *pdev, USBD_SetupReqTypedef *req)
{
  USBD_AUDIO_HandleTypeDef *haudio = (USBD_AUDIO_HandleTypeDef *)pdev->pClassData;
  USBD_AUDIO_ItfTypeDef *fops = (USBD_AUDIO_ItfTypeDef *)pdev->pUserData;

  if ((haudio == NULL) || (pdev->dev_state != USBD_STATE_CONFIGURED))
  {
    USBD_CtlError(pdev, req);
    return USBD_FAIL;
  }

  if ((req->bmRequest & USB_REQ_TYPE_MASK) == USB_REQ_TYPE_CLASS)
  {
    /* Independent capture/playback master mute on AudioControl interface 0. */
    if (((req->bmRequest & USB_REQ_RECIPIENT_MASK) == USB_REQ_RECIPIENT_INTERFACE) &&
        ((req->wIndex == (AUDIO_FEATURE_UNIT_ID << 8)) ||
         (req->wIndex == (AUDIO_PLAYBACK_FEATURE_UNIT_ID << 8))) &&
        (req->wValue == (AUDIO_CONTROL_MUTE << 8)) && (req->wLength == 1U))
    {
      if ((req->bRequest == AUDIO_REQ_GET_CUR) && ((req->bmRequest & 0x80U) != 0U))
      {
        haudio->control.data[0] = (req->wIndex == (AUDIO_FEATURE_UNIT_ID << 8)) ?
                                 haudio->mute : haudio->playback_mute;
        return USBD_CtlSendData(pdev, haudio->control.data, 1U);
      }
      if ((req->bRequest == AUDIO_REQ_SET_CUR) && ((req->bmRequest & 0x80U) == 0U))
      {
        haudio->control.cmd = AUDIO_REQ_SET_CUR;
        haudio->control.unit = (uint8_t)(req->wIndex >> 8);
        return USBD_CtlPrepareRx(pdev, haudio->control.data, 1U);
      }
    }
  }
  else if (((req->bmRequest & USB_REQ_TYPE_MASK) == USB_REQ_TYPE_STANDARD) &&
           ((req->bmRequest & USB_REQ_RECIPIENT_MASK) == USB_REQ_RECIPIENT_INTERFACE) &&
            (req->wIndex < USBD_MAX_NUM_INTERFACES))
  {
    switch (req->bRequest)
    {
      case USB_REQ_GET_STATUS:
        if ((req->bmRequest & 0x80U) == 0U) break;
        haudio->control.data[0] = 0U;
        haudio->control.data[1] = 0U;
        return USBD_CtlSendData(pdev, haudio->control.data, MIN(req->wLength, 2U));

      case USB_REQ_GET_INTERFACE:
        if ((req->bmRequest & 0x80U) == 0U) break;
        haudio->control.data[0] = (req->wIndex == AUDIO_CAPTURE_INTERFACE) ?
          (uint8_t)haudio->alt_setting : ((req->wIndex == AUDIO_PLAYBACK_INTERFACE) ?
          (uint8_t)haudio->playback_alt_setting : 0U);
        return USBD_CtlSendData(pdev, haudio->control.data, MIN(req->wLength, 1U));

      case USB_REQ_SET_INTERFACE:
        if (((req->bmRequest & 0x80U) != 0U) || (req->wLength != 0U) || (req->wValue > 1U) ||
            ((req->wIndex == 0U) && (req->wValue != 0U)))
        {
          break;
        }
        if (req->wIndex == 0U)
        {
          return USBD_OK;
        }
        if (req->wIndex == AUDIO_PLAYBACK_INTERFACE)
        {
          USBD_AUDIO_StopPlayback(pdev);
          if ((req->wValue == 1U) && (USBD_AUDIO_StartPlayback(pdev) != USBD_OK))
          {
            break;
          }
          return USBD_OK;
        }
        USBD_AUDIO_StopCapture(pdev);
        if (req->wValue == 1U)
        {
          if (USBD_LL_OpenEP(pdev, AUDIO_IN_EP, USBD_EP_TYPE_ISOC,
                             AUDIO_IN_MAX_PACKET) != USBD_OK)
          {
            break;
          }
          if (fops->AudioCmd(NULL, 0U, AUDIO_CMD_START) != USBD_OK)
          {
            USBD_LL_CloseEP(pdev, AUDIO_IN_EP);
            break;
          }
          pdev->ep_in[AUDIO_IN_EP & 0x0FU].is_used = 1U;
          haudio->alt_setting = 1U;
        }
        return USBD_OK;

      default:
        break;
    }
  }
  USBD_CtlError(pdev, req);
  return USBD_FAIL;
}

static uint8_t USBD_AUDIO_EP0_RxReady(USBD_HandleTypeDef *pdev)
{
  USBD_AUDIO_HandleTypeDef *haudio = (USBD_AUDIO_HandleTypeDef *)pdev->pClassData;
  if ((haudio != NULL) && (haudio->control.cmd == AUDIO_REQ_SET_CUR))
  {
    uint8_t mute = (haudio->control.data[0] != 0U) ? 1U : 0U;
    USBD_AUDIO_ItfTypeDef *fops = (USBD_AUDIO_ItfTypeDef *)pdev->pUserData;
    if (haudio->control.unit == AUDIO_FEATURE_UNIT_ID)
    {
      haudio->mute = mute;
      fops->MuteCtl(mute);
    }
    else if (haudio->control.unit == AUDIO_PLAYBACK_FEATURE_UNIT_ID)
    {
      haudio->playback_mute = mute;
      fops->PlaybackMuteCtl(mute);
    }
    haudio->control.cmd = 0U;
  }
  return USBD_OK;
}

static uint8_t USBD_AUDIO_DataIn(USBD_HandleTypeDef *pdev, uint8_t epnum)
{
  USBD_AUDIO_HandleTypeDef *haudio = (USBD_AUDIO_HandleTypeDef *)pdev->pClassData;
  if ((haudio != NULL) && (epnum == (AUDIO_IN_EP & 0x0FU)))
  {
    haudio->tx_busy = 0U;
  }
  else if ((haudio != NULL) && (epnum == (AUDIO_FEEDBACK_EP & 0x0FU)))
  {
    haudio->feedback_busy = 0U;
  }
  return USBD_OK;
}

static uint8_t USBD_AUDIO_DataOut(USBD_HandleTypeDef *pdev, uint8_t epnum)
{
  USBD_AUDIO_HandleTypeDef *haudio = (USBD_AUDIO_HandleTypeDef *)pdev->pClassData;
  if ((haudio != NULL) && (haudio->playback_alt_setting == 1U) &&
      (epnum == (AUDIO_OUT_EP & 0x0FU)))
  {
    uint32_t size = USBD_LL_GetRxDataSize(pdev, AUDIO_OUT_EP);
    if ((size <= AUDIO_OUT_MAX_PACKET) && ((size % AUDIO_FRAME_BYTES) == 0U))
    {
      ((USBD_AUDIO_ItfTypeDef *)pdev->pUserData)->PutPacket(haudio->out_buffer,
                                                        (uint16_t)size);
    }
    /* Invalid or empty packets never prevent the next host frame being received. */
    return USBD_LL_PrepareReceive(pdev, AUDIO_OUT_EP, haudio->out_buffer,
                                  AUDIO_OUT_MAX_PACKET);
  }
  return USBD_OK;
}

static uint8_t USBD_AUDIO_SendCapture(USBD_HandleTypeDef *pdev)
{
  USBD_AUDIO_HandleTypeDef *haudio = (USBD_AUDIO_HandleTypeDef *)pdev->pClassData;
  uint16_t size;
  if (haudio->alt_setting != 1U)
  {
    return USBD_OK;
  }
  if (haudio->tx_busy != 0U)
  {
    /* Retire an unpolled frame before touching its shared logical ISO slots. */
    USBD_LL_CloseEP(pdev, AUDIO_IN_EP);
    if (USBD_LL_OpenEP(pdev, AUDIO_IN_EP, USBD_EP_TYPE_ISOC,
                       AUDIO_IN_MAX_PACKET) != USBD_OK)
    {
      return USBD_FAIL;
    }
    haudio->tx_busy = 0U;
  }
  size = ((USBD_AUDIO_ItfTypeDef *)pdev->pUserData)->GetPacket(haudio->buffer,
                                                            sizeof(haudio->buffer));
  if ((size > sizeof(haudio->buffer)) || ((size % AUDIO_FRAME_BYTES) != 0U))
  {
    return USBD_FAIL;
  }
  haudio->tx_busy = 1U;
  if (USBD_LL_Transmit(pdev, AUDIO_IN_EP, haudio->buffer, size) != USBD_OK)
  {
    haudio->tx_busy = 0U;
    return USBD_FAIL;
  }
  return USBD_OK;
}

static uint8_t USBD_AUDIO_SOF(USBD_HandleTypeDef *pdev)
{
  USBD_AUDIO_HandleTypeDef *haudio = (USBD_AUDIO_HandleTypeDef *)pdev->pClassData;
  if (haudio == NULL)
  {
    return USBD_OK;
  }
  uint8_t status = USBD_AUDIO_SendCapture(pdev);
  if (haudio->playback_alt_setting != 1U)
  {
    return status;
  }
  uint32_t feedback = ((USBD_AUDIO_ItfTypeDef *)pdev->pUserData)->GetFeedback();
  if (haudio->feedback_busy != 0U)
  {
    USBD_LL_CloseEP(pdev, AUDIO_FEEDBACK_EP);
    if (USBD_LL_OpenEP(pdev, AUDIO_FEEDBACK_EP, USBD_EP_TYPE_ISOC,
                       AUDIO_FEEDBACK_BYTES) != USBD_OK)
    {
      return USBD_FAIL;
    }
    haudio->feedback_busy = 0U;
  }
  for (uint32_t byte = 0U; byte < AUDIO_FEEDBACK_BYTES; ++byte)
  {
    haudio->feedback[byte] = (uint8_t)(feedback >> (8U * byte));
  }
  haudio->feedback_busy = 1U;
  if (USBD_LL_Transmit(pdev, AUDIO_FEEDBACK_EP, haudio->feedback,
                       AUDIO_FEEDBACK_BYTES) != USBD_OK)
  {
    haudio->feedback_busy = 0U;
    return USBD_FAIL;
  }
  return status;
}

static uint8_t USBD_AUDIO_IsoINIncomplete(USBD_HandleTypeDef *pdev, uint8_t epnum)
{
  USBD_AUDIO_HandleTypeDef *haudio = (USBD_AUDIO_HandleTypeDef *)pdev->pClassData;
  if ((haudio != NULL) && (epnum == (AUDIO_IN_EP & 0x0FU)))
  {
    haudio->tx_busy = 1U; /* SOF will reset the endpoint before the next packet. */
  }
  else if ((haudio != NULL) && (epnum == (AUDIO_FEEDBACK_EP & 0x0FU)))
  {
    haudio->feedback_busy = 1U;
  }
  return USBD_OK;
}

/* Retained for compatibility with the generated playback callback template. */
void USBD_AUDIO_Sync(USBD_HandleTypeDef *pdev, AUDIO_OffsetTypeDef offset)
{
  UNUSED(pdev);
  UNUSED(offset);
}

static uint8_t *USBD_AUDIO_GetCfgDesc(uint16_t *length)
{
  *length = sizeof(USBD_AUDIO_CfgDesc);
  return USBD_AUDIO_CfgDesc;
}

static uint8_t *USBD_AUDIO_GetDeviceQualifierDesc(uint16_t *length)
{
  *length = sizeof(USBD_AUDIO_DeviceQualifierDesc);
  return USBD_AUDIO_DeviceQualifierDesc;
}

uint8_t USBD_AUDIO_RegisterInterface(USBD_HandleTypeDef *pdev, USBD_AUDIO_ItfTypeDef *fops)
{
  if (fops == NULL)
  {
    return USBD_FAIL;
  }
  pdev->pUserData = fops;
  return USBD_OK;
}
