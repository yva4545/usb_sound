#ifndef TEST_TIM_H
#define TEST_TIM_H

#include <stdint.h>

#define TIM1 ((void *)1)
#define TIM3 ((void *)3)
#define TIM_DMABASE_CCR1 13U
#define TIM_DMA_UPDATE 256U
#define TIM_DMABURSTLENGTH_2TRANSFERS 256U
#define TIM_CHANNEL_1 0U
#define TIM_CHANNEL_2 4U
#define CoreDebug_DEMCR_TRCENA_Msk 1U
#define DWT_CTRL_CYCCNTENA_Msk 1U
typedef struct { uint32_t DEMCR; } TestCoreDebug;
typedef struct { uint32_t CTRL; uint32_t CYCCNT; } TestDWT;
extern TestCoreDebug test_core_debug;
extern TestDWT test_dwt;
#define CoreDebug (&test_core_debug)
#define DWT (&test_dwt)

typedef struct { void *Instance; } TIM_HandleTypeDef;
extern TIM_HandleTypeDef htim3;
extern TIM_HandleTypeDef htim1;
int HAL_TIM_Base_Start(TIM_HandleTypeDef *timer);
int HAL_TIM_DMABurst_MultiWriteStart(TIM_HandleTypeDef *timer, uint32_t base,
                                     uint32_t source, const uint32_t *buffer,
                                     uint32_t burst, uint32_t length);
int HAL_TIM_PWM_Start(TIM_HandleTypeDef *timer, uint32_t channel);
#define __HAL_TIM_MOE_DISABLE(timer) ((void)(timer))

#endif
