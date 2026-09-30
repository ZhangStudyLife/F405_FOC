# Repository Guidelines

STM32F405 C11 firmware. Read App/README.md for wiring, commands and waveform channels.

- Keep handwritten code in App. Use simple functions and named state; no extra frameworks.
- Preserve existing uncommitted changes and Flash calibration.
- Current loop 20 kHz; speed/position 1 kHz; UART JustFloat 500 Hz.
- Core/Drivers are CubeMX/HAL/CMSIS. Keep edits in USER CODE blocks where possible.
- Update 405_FOC.ioc and regenerate; do not manually edit cmake/stm32cubemx/CMakeLists.txt.
- Build: cmake --preset Debug/Release, then cmake --build --preset Debug/Release.
- UART is the only firmware command/data transport. Do not restore USB, CAN, Studio or stdio.
- Do not add permanent test frameworks. Use temporary checks when explicitly requested.
- Old tests/tools/Studio/history remain pending deletion because recursive cleanup was blocked.
