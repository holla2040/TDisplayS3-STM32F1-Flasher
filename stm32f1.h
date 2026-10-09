#pragma once
#include <stdint.h>
#include <stddef.h>

#define STM32_FLASH_BASE 0x08000000

// Called after each page/chunk with (bytes done, total) — for progress UI.
typedef void (*stm32_progress_t)(size_t done, size_t total);

// Non-intrusive: attach, identify, detach; the target keeps running. Returns
// "no target" or e.g. "STM32F103 64K". Also latches the two calls below.
const char *stm32_probe();
// Why the jig must not flash the last probed chip: "locked" (RDP set),
// "unsupported" (not an F103 low/medium-density part, i.e. not 1 KB pages),
// or nullptr when it's fine to flash.
const char *stm32_refusal();
size_t stm32_flash_size();                            // bytes, from the chip's flash-size register
bool stm32_connect();                                 // attach + reset-halt (SYSRESETREQ)
// Erases only the pages the image covers: the EEPROM-emulation pages at the
// top of flash (e.g. the Arduino EEPROM library) survive a reflash.
bool stm32_program(const uint8_t *image, size_t len, stm32_progress_t progress = nullptr);
// 1 = match, 0 = mismatch, -1 = communication error (couldn't read back)
int stm32_verify(const uint8_t *image, size_t len, stm32_progress_t progress = nullptr);
void stm32_run();                                     // reset into the new firmware, detach
