#ifndef TEST_TIM_H
#define TEST_TIM_H

typedef struct { int unused; } TIM_HandleTypeDef;
extern TIM_HandleTypeDef htim3;
int HAL_TIM_Base_Start(TIM_HandleTypeDef *timer);

#endif
