# 每个测试有自己的 main()、ELF 和 map，不链接 Core/Src/main.c。
string(REGEX REPLACE "-Wl,-Map=[^ ]+" "" CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")

function(add_module_test name source)
    add_executable(${name}
        ${source} test_board.c
        ${CMAKE_SOURCE_DIR}/startup_stm32g431xx.s
        ${CMAKE_SOURCE_DIR}/Core/Src/stm32g4xx_it.c
        ${CMAKE_SOURCE_DIR}/Core/Src/stm32g4xx_hal_msp.c
        ${CMAKE_SOURCE_DIR}/Core/Src/sysmem.c
        ${CMAKE_SOURCE_DIR}/Core/Src/syscalls.c
        ${CMAKE_SOURCE_DIR}/OLED/OLED.c
        ${CMAKE_SOURCE_DIR}/OLED/OLED_Data.c)
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/OLED)
    target_link_libraries(${name} PRIVATE stm32cubemx STM32_Drivers ${TOOLCHAIN_LINK_LIBRARIES})
    target_link_options(${name} PRIVATE "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/${name}.map")
    set_target_properties(${name} PROPERTIES ADDITIONAL_CLEAN_FILES "${name}.map")
endfunction()

add_module_test(YLD_Cup_OpampTest opamp_test_main.c)
add_module_test(YLD_Cup_ADCTest adc_test_main.c)
add_module_test(YLD_Cup_GPIOTest gpio_test_main.c)
