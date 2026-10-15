#
# 独立标定固件 (TEST/): 单独编译、单独烧录, 不与主固件混在一起.
#
#   构建:  cmake --build --preset Debug --target YLD_Cup_Calib
#   产物:  build/Debug/TEST/YLD_Cup_Calib.elf
#   CLion: 复制一份运行/调试配置, Target 选 YLD_Cup_Calib
#
# 应用逻辑全部在单个文件 calib_main.c 中 (自带 main/时钟/外设初始化),
# 只复用板级文件(启动/中断/MSP)和 OLED / AD5662 驱动.
#

# 本目录作用域: 给本目标的 map 文件换个名字, 避免与主固件的 YLD_Cup.map 冲突
string(REPLACE "${CMAKE_PROJECT_NAME}.map" "${CMAKE_PROJECT_NAME}_Calib.map"
       CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")

add_executable(${CMAKE_PROJECT_NAME}_Calib)

target_sources(${CMAKE_PROJECT_NAME}_Calib PRIVATE
    # 应用文件: 只有这一个 (自带 main)
    ${CMAKE_CURRENT_SOURCE_DIR}/calib_main.c
    # 板级公共文件 (与主固件相同的那套, 但不含 Core/Src/main.c)
    ${CMAKE_SOURCE_DIR}/startup_stm32f103xb.s
    ${CMAKE_SOURCE_DIR}/Core/Src/stm32f1xx_it.c
    ${CMAKE_SOURCE_DIR}/Core/Src/stm32f1xx_hal_msp.c
    ${CMAKE_SOURCE_DIR}/Core/Src/sysmem.c
    ${CMAKE_SOURCE_DIR}/Core/Src/syscalls.c
    # 自建驱动
    ${CMAKE_SOURCE_DIR}/OLED/OLED.c
    ${CMAKE_SOURCE_DIR}/OLED/OLED_Data.c
    ${CMAKE_SOURCE_DIR}/AD5662/AD5662.c
)

# 自建驱动的头文件路径
target_include_directories(${CMAKE_PROJECT_NAME}_Calib PRIVATE
    ${CMAKE_SOURCE_DIR}/OLED
    ${CMAKE_SOURCE_DIR}/AD5662
)

# 头文件路径 / USE_HAL_DRIVER / STM32F103xB 由 stm32cubemx 接口库带出
target_link_libraries(${CMAKE_PROJECT_NAME}_Calib PRIVATE
    stm32cubemx
    STM32_Drivers
    ${TOOLCHAIN_LINK_LIBRARIES}
)

set_target_properties(${CMAKE_PROJECT_NAME}_Calib PROPERTIES
    ADDITIONAL_CLEAN_FILES ${CMAKE_PROJECT_NAME}_Calib.map)
