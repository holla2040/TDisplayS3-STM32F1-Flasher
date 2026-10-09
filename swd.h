#pragma once
#include <stdint.h>
#include <stddef.h>

// Bit-banged ARM SWD (ADIv5) host: DP/AP transactions and AHB-AP memory
// access. SWD is host-clocked, so interrupts only stretch the clock — no
// critical sections needed (unlike SWIM).

void swd_init();                          // configure SWDIO/SWCLK GPIOs
bool swd_connect(uint32_t *idcode);       // JTAG-to-SWD, read IDCODE, power up debug
void swd_disconnect();                    // drop the debug power-up request
bool swd_read32(uint32_t addr, uint32_t *val);
bool swd_write32(uint32_t addr, uint32_t val);
bool swd_read_block(uint32_t addr, uint32_t *buf, size_t words);
// Halfword writes (FPEC programs 16 bits at a time); the AHB-AP returns WAIT
// while the flash is busy, so no per-halfword BSY polling is needed.
bool swd_write_block16(uint32_t addr, const uint16_t *buf, size_t halfwords);
