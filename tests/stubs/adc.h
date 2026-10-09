#ifndef TEST_ADC_H
#define TEST_ADC_H

#include <stdint.h>

#define ADC1 ((void *)(uintptr_t)1U)
#define HAL_OK 0
typedef struct { void *Instance; } ADC_HandleTypeDef;
extern ADC_HandleTypeDef hadc1;
int HAL_ADCEx_Calibration_Start(ADC_HandleTypeDef *adc);
int HAL_ADC_Start_DMA(ADC_HandleTypeDef *adc, uint32_t *buffer, uint32_t length);
void Error_Handler(void);
uint32_t __get_PRIMASK(void);
void __disable_irq(void);
void __set_PRIMASK(uint32_t state);

#endif
