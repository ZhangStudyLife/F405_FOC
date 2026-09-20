# Repository Guidelines

## Project Structure & Module Organization

This is STM32F405 firmware written in C11. The current firmware target runs ADC bring-up and UART/JustFloat telemetry; encoder sources remain available separately.

- `App/Control/`: application orchestration, currently `adc_test.c`.
- `App/Hardware/bsp/`: board peripherals; `App/Hardware/mt6835/`: encoder driver and STM32 adapter.
- `App/Protocols/JustFloat/`: telemetry framing.
- `Core/` and `Drivers/`: CubeMX-generated initialization, HAL, and CMSIS.
- `cmake/`, `CMakePresets.json`, and `405_FOC.ioc`: build and peripheral configuration.
- `tests/`: host C tests and ADC/UART hardware validation records. Hardware references include `硬件PCB拓扑.md`.

## Build, Test, and Development Commands

Run from this repository root with CMake 3.22+, Ninja, and `arm-none-eabi` tools on `PATH`:

```sh
cmake --preset Debug
cmake --build --preset Debug
cmake --preset Release
cmake --build --preset Release
```

Configure before building each preset. Outputs include firmware ELF, BIN, and HEX files under `build/<preset>/`. Use Release for timing measurements. Firmware runs on the board; host tests run locally.

## Coding Style & Naming Conventions

Follow surrounding C style: four-space indentation in handwritten code, module-prefixed `snake_case` functions, `_t` type names, and uppercase constants. Use header guards such as `APP_<DIRECTORY>_<FILE>_H`. No repository formatter is configured; `.clangd` uses `build/Debug/compile_commands.json`.

Keep reusable drivers and protocol logic free of HAL types; isolate hardware dependencies in BSP and `*_port_stm32.*` files. Put handwritten code in `App/`, register sources in the top-level `CMakeLists.txt`, and limit generated-file edits to `USER CODE` blocks. Do not manually edit `cmake/stm32cubemx/CMakeLists.txt`.

## Testing Guidelines

Tests are standalone C programs, outside firmware CMake; no coverage threshold is configured. From the repository root, using host GCC:

```sh
gcc -std=c11 -Wall -Wextra -O2 -I App/Hardware/mt6835 tests/test_mt6835_crc.c App/Hardware/mt6835/mt6835.c -lm -o build/test_mt6835_crc.exe
./build/test_mt6835_crc.exe
```

Repeat with `test_mt6835_speed`. Name tests `test_<module>_<behavior>.c`. For fixes, reproduce the failure and verify the correction. Follow `tests/ADC_TEST.md` and `tests/UART_TEST.md` for relevant board checks; distinguish historical measurements from current results.

## Commit & Pull Request Guidelines

Recent commits commonly use `feat(uart): ...`, `docs(hw): ...`, and `chore(cubemx): ...`; follow that scoped style. PRs should describe the behavior change, affected peripherals, related issues, and actual build/test results. Include captures when needed to substantiate hardware behavior. Exclude generated build products.

## Contributor Workflow

State assumptions and verifiable success criteria before editing. Keep changes narrowly scoped; avoid speculative abstractions, unnecessary embedded variables/functions, and unrelated cleanup. Preserve existing uncommitted work.
