#
# 独立 AD5662 测试固件 (TEST_AD5662/): 单独编译、单独烧录.
#
#   构建:  cmake --build --preset Debug --target YLD_Cup_AD5662Test
#   产物:  build/Debug/TEST_AD5662/YLD_Cup_AD5662Test.elf
#   CLion: 复制一份运行/调试配置, Target 选 YLD_Cup_AD5662Test
#
# 应用逻辑全部在 ad5662_test_main.c 中 (自带 main/时钟/GPIO/SPI 初始化),
# 只复用板级文件(启动/中断/MSP)和 AD5662 / OLED 驱动.
#

# 本目录作用域: 给本目标的 map 文件换个名字, 避免与主固件的 YLD_Cup.map 冲突
string(REPLACE "${CMAKE_PROJECT_NAME}.map" "${CMAKE_PROJECT_NAME}_AD5662Test.map"
       CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")

add_executable(${CMAKE_PROJECT_NAME}_AD5662Test)

target_sources(${CMAKE_PROJECT_NAME}_AD5662Test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/ad5662_test_main.c
    ${CMAKE_SOURCE_DIR}/startup_stm32f103xb.s
    ${CMAKE_SOURCE_DIR}/Core/Src/stm32f1xx_it.c
    ${CMAKE_SOURCE_DIR}/Core/Src/stm32f1xx_hal_msp.c
    ${CMAKE_SOURCE_DIR}/Core/Src/sysmem.c
    ${CMAKE_SOURCE_DIR}/Core/Src/syscalls.c
    ${CMAKE_SOURCE_DIR}/AD5662/AD5662.c
    ${CMAKE_SOURCE_DIR}/OLED/OLED.c
    ${CMAKE_SOURCE_DIR}/OLED/OLED_Data.c
)

target_include_directories(${CMAKE_PROJECT_NAME}_AD5662Test PRIVATE
    ${CMAKE_SOURCE_DIR}/AD5662
    ${CMAKE_SOURCE_DIR}/OLED
)

# 头文件路径 / USE_HAL_DRIVER / STM32F103xB 由 stm32cubemx 接口库带出
target_link_libraries(${CMAKE_PROJECT_NAME}_AD5662Test PRIVATE
    stm32cubemx
    STM32_Drivers
    ${TOOLCHAIN_LINK_LIBRARIES}
)

set_target_properties(${CMAKE_PROJECT_NAME}_AD5662Test PROPERTIES
    ADDITIONAL_CLEAN_FILES ${CMAKE_PROJECT_NAME}_AD5662Test.map)
