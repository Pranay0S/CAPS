# Measuring the IDS-RNN on an STM32 (Cortex-M4F)

Bare-metal builds of the detector for STM32 Nucleo boards, with no HAL or vendor SDK:

| Image | Purpose |
|---|---|
| `probe_base_<opt>.elf` / `probe_ids_<opt>.elf` | Identical programs except that only the second runs the detector. The size difference is the detector's true flash and RAM cost, including any library routines it pulls in (currently none). |
| `bench_<opt>.elf` / `.bin` | Runs on the board. Replays the golden vectors, checks the outputs against Python, counts cycles per step, and measures peak stack. |

`<opt>` is `Os` (smallest code) or `O2` (fastest code).

## Requirements

- Arm GNU toolchain: `pacman -S mingw-w64-ucrt-x86_64-arm-none-eabi-{gcc,binutils,newlib}` (installed on this PC: GCC 13.3.0)
- CMake and Ninja (already used for the host build)
- For the on-board run: a Nucleo board and a USB cable. No flashing software is needed, because the ST-Link appears as a USB drive.

## Build and size report (no board needed)

```sh
export PATH=/c/msys64/ucrt64/bin:$PATH     # Git Bash; the MSYS2 UCRT64 shell has it already
cmake -S firmware/mcu -B firmware/build-mcu -G Ninja -DCMAKE_TOOLCHAIN_FILE=$PWD/firmware/mcu/arm-none-eabi.cmake
cmake --build firmware/build-mcu
py -3.13 firmware/mcu/measure_size.py firmware/build-mcu   # fails if over the 6,144 B budget
```

Current result: **5,322 B** of flash at `-Os` (5,686 B at `-O2`) and 168 B of static RAM.

## Run the benchmark on the board

1. Plug in the Nucleo. A drive called `NODE_F446RE` (or similar) appears.
2. Copy `firmware/build-mcu/bench_Os.bin` onto that drive. The ST-Link flashes it and resets the board.
3. Run `powershell -File firmware/mcu/read_bench.ps1`. The report repeats every few seconds.

Boards: the default build targets **STM32F4** Nucleo-64 boards (F401RE, F411RE, F446RE). For an **STM32L4** board (L476RG and similar), configure with `-DBENCH_FAMILY=L4`. Any other family needs its USART2 register addresses adapted in `bench.c`. The results are also left in the global `g_bench`, so they can be read with a debugger on any board.

## What the benchmark reports, and its caveats

- **`max |dP| vs Python`**: largest probability difference from the Python int8 model over all golden rows (must be ≤ 2e-3). This checks the arithmetic on the real FPU, not just the PC build.
- **Cycles per step**: read from the DWT cycle counter around `ids_rnn_step()`, minus the cost of reading the counter. Pass 1 is *cold* (first execution); pass 2 is *warm*. Mean and max are over the 205 golden rows.
- **Clock and wait states**: the CPU runs on its reset clock (16 MHz HSI on F4, 4 MHz MSI on L4) with zero flash wait states. At full speed (84–180 MHz) the flash needs wait states, and the ART accelerator hides most but not all of them. Cycles measured at the reset clock are therefore a slightly optimistic estimate at full clock. The µs figures printed for 84 and 180 MHz are simply cycles ÷ frequency.
- **Stack**: free RAM is filled with a pattern; afterwards the deepest overwritten word gives the peak. "Detector" stack is that peak minus what `main()` used before the detector ran, so it also includes the small benchmark-loop frame.
