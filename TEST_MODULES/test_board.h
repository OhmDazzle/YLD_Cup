#ifndef YLD_TEST_BOARD_H
#define YLD_TEST_BOARD_H

#include <stdbool.h>
#include "stm32g4xx_hal.h"

/* 最新原理图：K1=PB15, K2=PB14, K3=PB13，按下为低。 */
#define TB_K1 1U
#define TB_K2 2U
#define TB_K3 4U
#define TB_VREF 3.3f
#ifndef TB_ADC_SAMPLES
#define TB_ADC_SAMPLES 16U
#endif

typedef struct {
  uint16_t code;
  float pin_v;
  bool valid;
} TB_AdcSample;

/* OLED 故障时可在调试器 Watch 中查看；fault=0 只说明初始化未报错。 */
typedef struct {
  uint32_t fault;
  uint32_t heartbeat;
  uint8_t keys_raw;
  uint8_t keys_down;
  uint16_t dac_code;
  float dac_v;
} TB_Status;

extern volatile TB_Status tb_status;

void TB_Init(void);
uint8_t TB_KeysPoll(void); /* 非阻塞消抖，返回本次按下事件位；松手后可再次触发。 */
void TB_Buzzer(bool on);
void TB_DacInit(float volts);
void TB_DacWrite(float volts);
uint16_t TB_DacCode(float volts);
void TB_AdcInit(void);
bool TB_AdcRead(unsigned index, TB_AdcSample *sample); /* VIN,VOUT,IIN,IOUT */
void Error_Handler(void);

#endif
