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
# Filter obsolete CubeMX sources here; keep the generated CMake file untouched.
get_target_property(app_sources ${CMAKE_PROJECT_NAME} SOURCES)
list(FILTER app_sources EXCLUDE REGEX "/(syscalls)\\.c$")
set_property(TARGET ${CMAKE_PROJECT_NAME} PROPERTY SOURCES "${app_sources}")
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    App/Control/app.c
    App/Control/control.c
    App/FOC/foc.c
    App/Music/music.c
    App/Hardware/bsp/bsp_motor.c
    App/Hardware/bsp/bsp_adc.c
    App/Hardware/bsp/bsp_uart.c
    App/Hardware/mt6835/mt6835.c
    App/Hardware/mt6835/mt6835_port_stm32.c
)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    App/Control
    App/Config
    App/FOC
    App/Music
    App/Hardware/bsp
    App/Hardware/mt6835
)
target_link_libraries(${CMAKE_PROJECT_NAME} stm32cubemx m)
# Keep the 20 kHz path optimized in Debug too; -O0 misses PWM deadlines.
set_source_files_properties(
    App/FOC/foc.c App/Music/music.c App/Hardware/bsp/bsp_motor.c App/Hardware/bsp/bsp_uart.c
    App/Control/app.c App/Control/control.c App/Hardware/bsp/bsp_adc.c
    App/Hardware/mt6835/mt6835.c App/Hardware/mt6835/mt6835_port_stm32.c
    PROPERTIES COMPILE_OPTIONS "-O3")
add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${CMAKE_PROJECT_NAME}> ${CMAKE_PROJECT_NAME}.bin
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:${CMAKE_PROJECT_NAME}> ${CMAKE_PROJECT_NAME}.hex
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${CMAKE_PROJECT_NAME}>
)
