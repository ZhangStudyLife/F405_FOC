cmake_minimum_required(VERSION 3.22)
project(405_FOC C ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

add_executable(${CMAKE_PROJECT_NAME})
add_subdirectory(cmake/stm32cubemx)
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    App/Control/app.c
    App/Control/control.c
    App/FOC/foc.c
    App/Hardware/bsp/bsp_motor.c
    App/Hardware/bsp/bsp_adc.c
    App/Hardware/bsp/bsp_can.c
    App/Hardware/bsp/bsp_uart.c
    App/Hardware/bsp/bsp_usb.c
    App/Hardware/mt6835/mt6835.c
    App/Hardware/mt6835/mt6835_port_stm32.c
)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    App/Control
    App/FOC
    App/Hardware/bsp
    App/Hardware/mt6835
    App/Protocols/JustFloat
)
option(FOC_CAPTURE "2048-sample current capture (separate debug build)" OFF)
if(FOC_CAPTURE)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE FOC_CAPTURE)
endif()
target_link_libraries(${CMAKE_PROJECT_NAME} stm32cubemx m)
# USB FIFO service must stay short even in Debug builds.
target_compile_options(USB_Device_Library PRIVATE -O3)
set_source_files_properties(
    App/Hardware/bsp/bsp_usb.c USB_DEVICE/App/usbd_cdc_if.c
    USB_DEVICE/Target/usbd_conf.c
    PROPERTIES COMPILE_OPTIONS "-O3")
set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_pcd.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_ll_usb.c
    DIRECTORY cmake/stm32cubemx PROPERTIES COMPILE_OPTIONS "-O3")
# Keep the 20 kHz path optimized in Debug too; -O0 misses PWM deadlines.
set_source_files_properties(
    App/FOC/foc.c App/Hardware/bsp/bsp_motor.c
    App/Control/app.c App/Control/control.c App/Hardware/bsp/bsp_adc.c App/Hardware/bsp/bsp_uart.c
    App/Hardware/mt6835/mt6835.c App/Hardware/mt6835/mt6835_port_stm32.c
    PROPERTIES COMPILE_OPTIONS "-O3")
add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${CMAKE_PROJECT_NAME}> ${CMAKE_PROJECT_NAME}.bin
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:${CMAKE_PROJECT_NAME}> ${CMAKE_PROJECT_NAME}.hex
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${CMAKE_PROJECT_NAME}>
)
