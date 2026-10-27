#ifndef BUZZER_H
#define BUZZER_H

#include "stm32g4xx_hal.h"

/* 无源蜂鸣器：独占 TIM2，PA2/AF1/TIM2_CH3 -> Q2。
 * 系统时钟和 GPIO 初始化后调用一次；初始化完成时保持静音。
 * PWM 无需中断，主循环阻塞或关中断时仍能发声。 */
HAL_StatusTypeDef Buzzer_Init(void);
void Buzzer_Set(uint8_t on);

#endif
