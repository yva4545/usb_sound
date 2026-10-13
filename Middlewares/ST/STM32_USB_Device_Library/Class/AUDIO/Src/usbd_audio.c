/**
  ******************************************************************************
  * @file    usbd_audio.c
  * @author  MCD Application Team
  * @brief   USB Audio Class 1.0, adapted for six-channel ADC capture.
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
  NULL,
  USBD_AUDIO_SOF,
  USBD_AUDIO_IsoINIncomplete,
  NULL,
  USBD_AUDIO_GetCfgDesc,
  USBD_AUDIO_GetCfgDesc,
  USBD_AUDIO_GetCfgDesc,
  USBD_AUDIO_GetDeviceQualifierDesc,
};

/* Signal path: six-channel line input (1) -> master mute (2) -> USB streaming (3).
 * Interface 0 is AudioControl; interface 1 has idle and capture alternate settings.
 */
__ALIGN_BEGIN static uint8_t USBD_AUDIO_CfgDesc[] __ALIGN_END =
{
  /* Configuration: two interfaces, bus powered, 100 mA. */
  0x09, USB_DESC_TYPE_CONFIGURATION,
  LOBYTE(USB_AUDIO_CONFIG_DESC_SIZ), HIBYTE(USB_AUDIO_CONFIG_DESC_SIZ),
  0x02, 0x01, 0x00, 0x80, 0x32,

  /* Standard AudioControl interface. */
  0x09, USB_DESC_TYPE_INTERFACE, 0x00, 0x00, 0x00,
  USB_DEVICE_CLASS_AUDIO, AUDIO_SUBCLASS_AUDIOCONTROL, AUDIO_PROTOCOL_UNDEFINED, 0x00,

  /* UAC 1.0 header; AC descriptors total 9 + 12 + 14 + 9 = 44 bytes. */
  0x09, AUDIO_INTERFACE_DESCRIPTOR_TYPE, AUDIO_CONTROL_HEADER,
  0x00, 0x01, 0x2C, 0x00, 0x01, 0x01,

  /* Physical input terminal: line connector (0x0603), six independent inputs.
   * No spatial channel assignments; USB channel order is PA1 through PA6.
   */
  0x0C, AUDIO_INTERFACE_DESCRIPTOR_TYPE, AUDIO_CONTROL_INPUT_TERMINAL,
  0x01, 0x03, 0x06, 0x00, AUDIO_CHANNELS, 0x00, 0x00, 0x00, 0x00,

  /* Feature unit: master mute, no individual channel controls. */
  0x0E, AUDIO_INTERFACE_DESCRIPTOR_TYPE, AUDIO_CONTROL_FEATURE_UNIT,
  AUDIO_FEATURE_UNIT_ID, 0x01, 0x01, AUDIO_CONTROL_MUTE,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

  /* Output terminal: USB streaming (0x0101), sourced from feature unit 2. */
  0x09, AUDIO_INTERFACE_DESCRIPTOR_TYPE, AUDIO_CONTROL_OUTPUT_TERMINAL,
  0x03, 0x01, 0x01, 0x00, AUDIO_FEATURE_UNIT_ID, 0x00,

  /* AudioStreaming alternate 0: zero bandwidth. */
  0x09, USB_DESC_TYPE_INTERFACE, 0x01, 0x00, 0x00,
  USB_DEVICE_CLASS_AUDIO, AUDIO_SUBCLASS_AUDIOSTREAMING, AUDIO_PROTOCOL_UNDEFINED, 0x00,

  /* AudioStreaming alternate 1: one capture endpoint. */
  0x09, USB_DESC_TYPE_INTERFACE, 0x01, 0x01, 0x01,
  USB_DEVICE_CLASS_AUDIO, AUDIO_SUBCLASS_AUDIOSTREAMING, AUDIO_PROTOCOL_UNDEFINED, 0x00,

  /* Link to USB output terminal 3; PCM8 is unsigned 8-bit audio. */
  0x07, AUDIO_INTERFACE_DESCRIPTOR_TYPE, AUDIO_STREAMING_GENERAL,
  0x03, 0x01, LOBYTE(AUDIO_FORMAT_PCM8), HIBYTE(AUDIO_FORMAT_PCM8),

  /* Type I: six channels, unsigned 8-bit PCM at a fixed 48 kHz. */
  0x0B, AUDIO_INTERFACE_DESCRIPTOR_TYPE, AUDIO_STREAMING_FORMAT_TYPE,
  AUDIO_FORMAT_TYPE_I, AUDIO_CHANNELS, AUDIO_BYTES_PER_SAMPLE, 8, 0x01,
  AUDIO_SAMPLE_FREQ(USBD_AUDIO_FREQ),

  /* Asynchronous isochronous IN, up to 49 six-channel frames every 1 ms.
   * Capture packet lengths report the source clock rate; no feedback EP is needed.
   */
  0x09, USB_DESC_TYPE_ENDPOINT, AUDIO_IN_EP, 0x05,
  LOBYTE(AUDIO_IN_MAX_PACKET), HIBYTE(AUDIO_IN_MAX_PACKET), 0x01, 0x00, 0x00,

  /* No sampling-frequency or pitch controls; fixed sampling rate. */
  0x07, AUDIO_ENDPOINT_DESCRIPTOR_TYPE, AUDIO_ENDPOINT_GENERAL,
  0x00, 0x00, 0x00, 0x00,
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

  if ((fops == NULL) || (fops->GetPacket == NULL))
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

static void USBD_AUDIO_Stop(USBD_HandleTypeDef *pdev)
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

static uint8_t USBD_AUDIO_DeInit(USBD_HandleTypeDef *pdev, uint8_t cfgidx)
{
  UNUSED(cfgidx);
  if (pdev->pClassData != NULL)
  {
    USBD_AUDIO_Stop(pdev);
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
    /* Only the advertised master mute of feature unit 2 on AC interface 0. */
    if (((req->bmRequest & USB_REQ_RECIPIENT_MASK) == USB_REQ_RECIPIENT_INTERFACE) &&
        (req->wIndex == (AUDIO_FEATURE_UNIT_ID << 8)) &&
        (req->wValue == (AUDIO_CONTROL_MUTE << 8)) && (req->wLength == 1U))
    {
      if ((req->bRequest == AUDIO_REQ_GET_CUR) && ((req->bmRequest & 0x80U) != 0U))
      {
        haudio->control.data[0] = haudio->mute;
        return USBD_CtlSendData(pdev, haudio->control.data, 1U);
      }
      if ((req->bRequest == AUDIO_REQ_SET_CUR) && ((req->bmRequest & 0x80U) == 0U))
      {
        haudio->control.cmd = AUDIO_REQ_SET_CUR;
        return USBD_CtlPrepareRx(pdev, haudio->control.data, 1U);
      }
    }
  }
  else if (((req->bmRequest & USB_REQ_TYPE_MASK) == USB_REQ_TYPE_STANDARD) &&
           ((req->bmRequest & USB_REQ_RECIPIENT_MASK) == USB_REQ_RECIPIENT_INTERFACE) &&
           (req->wIndex < 2U))
  {
    switch (req->bRequest)
    {
      case USB_REQ_GET_STATUS:
        haudio->control.data[0] = 0U;
        haudio->control.data[1] = 0U;
        return USBD_CtlSendData(pdev, haudio->control.data, MIN(req->wLength, 2U));

      case USB_REQ_GET_INTERFACE:
        haudio->control.data[0] = (req->wIndex == 1U) ? (uint8_t)haudio->alt_setting : 0U;
        return USBD_CtlSendData(pdev, haudio->control.data, MIN(req->wLength, 1U));

      case USB_REQ_SET_INTERFACE:
        if ((req->wLength != 0U) || (req->wValue > 1U) ||
            ((req->wIndex == 0U) && (req->wValue != 0U)))
        {
          break;
        }
        if (req->wIndex == 0U)
        {
          return USBD_OK;
        }
        USBD_AUDIO_Stop(pdev);
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
    haudio->mute = (haudio->control.data[0] != 0U) ? 1U : 0U;
    ((USBD_AUDIO_ItfTypeDef *)pdev->pUserData)->MuteCtl(haudio->mute);
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
  return USBD_OK;
}

static uint8_t USBD_AUDIO_SOF(USBD_HandleTypeDef *pdev)
{
  USBD_AUDIO_HandleTypeDef *haudio = (USBD_AUDIO_HandleTypeDef *)pdev->pClassData;
  uint16_t size;
  if ((haudio == NULL) || (haudio->alt_setting != 1U))
  {
    return USBD_OK;
  }
  if (haudio->tx_busy != 0U)
  {
    /* The previous frame was not polled (including suspend/resume).
     * F103 does not report ISO IN incomplete: discard the expired transfer.
     */
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

static uint8_t USBD_AUDIO_IsoINIncomplete(USBD_HandleTypeDef *pdev, uint8_t epnum)
{
  USBD_AUDIO_HandleTypeDef *haudio = (USBD_AUDIO_HandleTypeDef *)pdev->pClassData;
  if ((haudio != NULL) && (epnum == (AUDIO_IN_EP & 0x0FU)))
  {
    haudio->tx_busy = 1U; /* SOF will reset the endpoint before the next packet. */
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
