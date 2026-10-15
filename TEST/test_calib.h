/**
  ******************************************************************************
  * @file    test_calib.h
  * @brief   采样标定模式 (TEST 文件夹) 接口.
  *
  *  用途: 在本板上实测 采样电阻(R6/R7) 与 电压分压系数(Vin/Vout),
  *        测量步骤与计算公式见 TEST/README.md.
  ******************************************************************************
  */

#ifndef TEST_CALIB_H
#define TEST_CALIB_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include "AD5662.h"

/**
  * @brief  标定模式主循环 (不返回): 实时显示 4 个通道的 12 位码值与引脚电压.
  * @note   按键: K3(B9) 输出 MOS 开/关; K1(A8)/K2(B11) 目标电压 ±1V (5~15V).
  *         退出: 断电重启 (不按住 K3) 即可回到正常固件.
  * @param  hadc_v : 电压通道 ADC 句柄 (ADC1: CH7=Vin, CH6=Vout)
  * @param  hadc_i : 电流通道 ADC 句柄 (ADC2: CH9=Iin, CH8=Iout)
  * @param  dac    : 已初始化的 AD5662 句柄 (开输出前预置 DAC 用)
  * @retval 无 (函数不返回)
  */
void TestCalib_Run(ADC_HandleTypeDef *hadc_v, ADC_HandleTypeDef *hadc_i, AD5662_t *dac);

#ifdef __cplusplus
}
#endif

#endif /* TEST_CALIB_H */
