# Bare-metal STM32 (Cortex-M4F) build of the IDS-RNN detector: size probes and
# an on-target benchmark.
#
#   cmake -S firmware/mcu -B firmware/build-mcu -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=firmware/mcu/arm-none-eabi.cmake
#   cmake --build firmware/build-mcu
#   python firmware/mcu/measure_size.py firmware/build-mcu
#
# -DBENCH_FAMILY=L4 builds the benchmark's UART code for STM32L4 Nucleo boards.
cmake_minimum_required(VERSION 3.16)
project(ivpump_mcu LANGUAGES C)

set(BENCH_FAMILY F4 CACHE STRING "STM32 family for the benchmark UART: F4 or L4")
set(FW ${CMAKE_CURRENT_SOURCE_DIR}/..)

set(CPU_FLAGS -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16)
set(COMMON_FLAGS ${CPU_FLAGS} -std=c11 -ffunction-sections -fdata-sections -fno-common
    -Wall -Wextra -Wdouble-promotion -fstack-usage)
set(LINK_FLAGS ${CPU_FLAGS} -T${CMAKE_CURRENT_SOURCE_DIR}/stm32_min.ld -nostartfiles
    --specs=nano.specs --specs=nosys.specs -Wl,--gc-sections -Wl,--print-memory-usage)

function(mcu_image name opt)
  add_executable(${name}.elf ${ARGN} ${CMAKE_CURRENT_SOURCE_DIR}/startup_cm4.c)
  target_include_directories(${name}.elf PRIVATE ${FW}/include ${FW}/tests)
  target_compile_options(${name}.elf PRIVATE ${COMMON_FLAGS} ${opt})
  target_link_options(${name}.elf PRIVATE ${LINK_FLAGS} -Wl,-Map=${name}.map)
  target_link_libraries(${name}.elf PRIVATE m)
  add_custom_command(TARGET ${name}.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary ${name}.elf ${name}.bin
    COMMAND ${CMAKE_SIZE} ${name}.elf)
endfunction()

foreach(opt Os O2)
  mcu_image(probe_base_${opt} -${opt} ${CMAKE_CURRENT_SOURCE_DIR}/size_probe.c)
  target_compile_definitions(probe_base_${opt}.elf PRIVATE WITH_IDS=0)
  mcu_image(probe_ids_${opt} -${opt} ${CMAKE_CURRENT_SOURCE_DIR}/size_probe.c ${FW}/src/ids_rnn.c)
  target_compile_definitions(probe_ids_${opt}.elf PRIVATE WITH_IDS=1)

  mcu_image(bench_${opt} -${opt} ${CMAKE_CURRENT_SOURCE_DIR}/bench.c ${FW}/src/ids_rnn.c)
  if(BENCH_FAMILY STREQUAL "L4")
    target_compile_definitions(bench_${opt}.elf PRIVATE BENCH_STM32L4)
  endif()
endforeach()
