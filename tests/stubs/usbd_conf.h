#ifndef TEST_USBD_CONF_H
#define TEST_USBD_CONF_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define USBD_AUDIO_FREQ 48000U
#define USBD_MAX_NUM_INTERFACES 3U
#define UNUSED(value) ((void)(value))
#define USBD_malloc malloc
#define USBD_free free

#endif
