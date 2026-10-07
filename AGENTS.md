# Repository Guidelines

STM32F405 C11 firmware. Read App/README.md for wiring, commands and waveform channels.

- Keep handwritten code in App. Use simple functions and named state; no extra frameworks.
- Preserve existing uncommitted changes and Flash calibration.
- Current loop 20 kHz; speed/position 1 kHz; JustFloat 1 kHz (84 B/frame).
- Core/Drivers are CubeMX/HAL/CMSIS. Keep edits in USER CODE blocks where possible.
- Update 405_FOC.ioc and regenerate; do not manually edit cmake/stm32cubemx/CMakeLists.txt.
- Build: cmake --preset Debug/Release, then cmake --build --preset Debug/Release.
- USB CDC and UART are the debug transports; switches and the sole command source are in App/Debug/debug.h. Do not restore CAN, Studio or stdio.
- Do not add permanent test frameworks. Use temporary checks when explicitly requested.
- Keep current usage docs only; store generated logs in ignored build directories.
