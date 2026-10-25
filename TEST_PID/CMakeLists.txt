# 独立恒流调参固件，不链接业务 main.c。
string(REGEX REPLACE "-Wl,-Map=[^ ]+" "" CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
add_executable(YLD_Cup_CCPIDTune
    cc_pid_main.c cc_pid.c
    ${CMAKE_SOURCE_DIR}/TEST_MODULES/test_board.c
    ${CMAKE_SOURCE_DIR}/startup_stm32g431xx.s
    ${CMAKE_SOURCE_DIR}/Core/Src/stm32g4xx_it.c
    ${CMAKE_SOURCE_DIR}/Core/Src/stm32g4xx_hal_msp.c
    ${CMAKE_SOURCE_DIR}/Core/Src/sysmem.c
    ${CMAKE_SOURCE_DIR}/Core/Src/syscalls.c
    ${CMAKE_SOURCE_DIR}/OLED/OLED.c
    ${CMAKE_SOURCE_DIR}/OLED/OLED_Data.c)
target_include_directories(YLD_Cup_CCPIDTune PRIVATE
    ${CMAKE_SOURCE_DIR}/OLED ${CMAKE_SOURCE_DIR}/TEST_MODULES)
# 和主程序相同的 ADC 均值次数；只影响本固件。
target_compile_definitions(YLD_Cup_CCPIDTune PRIVATE TB_ADC_SAMPLES=4U)
target_link_libraries(YLD_Cup_CCPIDTune PRIVATE
    stm32cubemx STM32_Drivers ${TOOLCHAIN_LINK_LIBRARIES})
target_link_options(YLD_Cup_CCPIDTune PRIVATE
    "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/YLD_Cup_CCPIDTune.map")
