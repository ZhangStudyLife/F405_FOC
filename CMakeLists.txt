cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME 405_FOC)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Generate .bin/.hex and print size after build (used by the flash tooling)
add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${CMAKE_PROJECT_NAME}> ${CMAKE_PROJECT_NAME}.bin
    COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:${CMAKE_PROJECT_NAME}> ${CMAKE_PROJECT_NAME}.hex
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${CMAKE_PROJECT_NAME}>
    COMMENT "Generating ${CMAKE_PROJECT_NAME}.bin / .hex"
)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
#
# App/ 是本工程的业务代码层，分层依赖规则见 App/README.md：
#   App/Hardware/   外设驱动 + HAL 封装（驱动本体不含 HAL，板级 port 才含）
#   App/Protocols/  通信协议（纯逻辑，不碰硬件）
#   App/FOC/        电机控制算法（纯逻辑，不碰硬件）
#
# 注意：不要去改 cmake/stm32cubemx/CMakeLists.txt —— 那个文件每次 CubeMX
#       重新生成都会被覆盖；本文件只生成一次，才是可以安全手工维护的地方。
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # ---- App/Hardware/bsp：HAL 封装层 ----
    App/Hardware/bsp/bsp_gpio.c
    App/Hardware/bsp/bsp_spi.c
    App/Hardware/bsp/bsp_time.c

    # ---- App/Hardware/mt6835：21-bit 磁编码器驱动 ----
    App/Hardware/mt6835/mt6835.c
    App/Hardware/mt6835/mt6835_port_stm32.c

    # ---- App/Protocols/scope：RAM 示波器（主机经 SWD 抓取）----
    # scope_encoder.c 是"编码器 -> 示波器"的绑定层，通道布局与放大系数都在里面。
    # main.c 里若没有调用它，--gc-sections 会把这两个文件整个丢掉，
    # Flash 和 RAM 一分不占。
    App/Protocols/scope/scope.c
    App/Protocols/scope/scope_encoder.c
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    App/Hardware/bsp
    App/Hardware/mt6835
    App/Protocols/scope

    # 下面几条等对应目录真正有头文件时再打开
    # （现在只有 .gitkeep，加空 include 路径没有意义）：
    # App/Hardware
    # App/Protocols
    # App/FOC
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)
