#include <Arduino.h>
#include "swd.h"
#include "stm32f1.h"

// STM32F10x flash controller (FPEC), RM0008 / PM0075
#define FLASH_KEYR 0x40022004
#define FLASH_SR   0x4002200C
#define FLASH_CR   0x40022010
#define FLASH_AR   0x40022014
#define FLASH_OBR  0x4002201C
#define SR_BSY      0x01
#define SR_PGERR    0x04
#define SR_WRPRTERR 0x10
#define SR_EOP      0x20
#define CR_PG   0x01
#define CR_PER  0x02
#define CR_STRT 0x40
#define CR_LOCK 0x80
#define OBR_RDPRT 0x02
// ponytail: 1 KB pages = low/medium density (F103C8/CB). High density is 2 KB.
#define PAGE 1024

// Cortex-M3 debug / system control
#define DHCSR 0xE000EDF0
#define DEMCR 0xE000EDFC
#define AIRCR 0xE000ED0C
#define DBGKEY       0xA05F0000
#define C_DEBUGEN    0x01
#define C_HALT       0x02
#define S_HALT       (1UL << 17)
#define VC_CORERESET 0x01
#define SYSRESETREQ  0x05FA0004

#define DBGMCU_IDCODE 0xE0042000
#define FLASH_SIZE_KB 0x1FFFF7E0   // 16-bit, in the low half of the word

static bool rdp = false;

const char *stm32_probe() {
  static char name[32];
  uint32_t idcode, dev, kb, obr;
  if (!swd_connect(&idcode) || !swd_read32(DBGMCU_IDCODE, &dev) ||
      !swd_read32(FLASH_SIZE_KB, &kb) || !swd_read32(FLASH_OBR, &obr))
    return "no target";
  swd_disconnect();
  rdp = obr & OBR_RDPRT;
  dev &= 0xFFF;
  kb &= 0xFFFF;
  if (dev == 0x410) snprintf(name, sizeof(name), "STM32F103 %luK%s", kb, rdp ? " locked" : "");
  else snprintf(name, sizeof(name), "dev %03lX %luK%s", dev, kb, rdp ? " locked" : "");
  return name;
}

bool stm32_locked() { return rdp; }

// No NRST on the Bluepill's SWD header, so reset via SYSRESETREQ with
// reset vector catch: the core halts before the old firmware runs a single
// instruction, with every peripheral (and any watchdog) back at reset state.
bool stm32_connect() {
  uint32_t id, dhcsr = 0;
  if (!swd_connect(&id)) return false;
  if (!swd_write32(DHCSR, DBGKEY | C_DEBUGEN | C_HALT)) return false;
  if (!swd_write32(DEMCR, VC_CORERESET)) return false;
  swd_write32(AIRCR, SYSRESETREQ);   // may not ack across the reset; checked below
  delay(20);
  if (!swd_connect(&id) || !swd_read32(DHCSR, &dhcsr)) return false;
  if (!(dhcsr & S_HALT)) {
    Serial.printf("stm32: reset-halt failed, DHCSR=0x%08lX\n", dhcsr);
    return false;
  }
  Serial.printf("stm32: connected (IDCODE 0x%08lX), core halted at reset\n", id);
  return true;
}

static bool wait_idle(uint32_t ms) {
  uint32_t sr = 0, t0 = millis();
  do {
    if (!swd_read32(FLASH_SR, &sr)) return false;
    if (!(sr & SR_BSY)) {
      if (!(sr & (SR_PGERR | SR_WRPRTERR))) return true;
      Serial.printf("stm32: flash error, SR=0x%02lX\n", sr);
      return false;
    }
  } while (millis() - t0 < ms);
  Serial.println("stm32: flash busy timeout");
  return false;
}

bool stm32_program(const uint8_t *image, size_t len, stm32_progress_t progress) {
  uint32_t cr = CR_LOCK;
  if (!swd_write32(FLASH_KEYR, 0x45670123) || !swd_write32(FLASH_KEYR, 0xCDEF89AB) ||
      !swd_read32(FLASH_CR, &cr)) return false;
  if (cr & CR_LOCK) {
    Serial.println("stm32: FPEC unlock rejected");
    return false;
  }
  Serial.printf("stm32: programming %u bytes in %u-byte pages\n", (unsigned)len, PAGE);
  for (size_t off = 0; off < len; off += PAGE) {
    uint32_t addr = STM32_FLASH_BASE + off;
    size_t n = min((size_t)PAGE, len - off);
    // page erase (tERASE 20-40 ms), then halfword program of just this page
    if (!swd_write32(FLASH_SR, SR_EOP | SR_PGERR | SR_WRPRTERR) ||   // clear stale flags
        !swd_write32(FLASH_CR, CR_PER) || !swd_write32(FLASH_AR, addr) ||
        !swd_write32(FLASH_CR, CR_PER | CR_STRT) || !wait_idle(100) ||
        !swd_write32(FLASH_CR, CR_PG) ||
        !swd_write_block16(addr, (const uint16_t *)(image + off), (n + 1) / 2) ||
        !wait_idle(10)) {
      Serial.printf("stm32: page @ 0x%08lX failed\n", addr);
      return false;
    }
    if (progress) progress(off + n, len);
  }
  swd_write32(FLASH_CR, CR_LOCK);
  Serial.println("stm32: program complete");
  return true;
}

int stm32_verify(const uint8_t *image, size_t len, stm32_progress_t progress) {
  uint32_t buf[PAGE / 4];
  for (size_t off = 0; off < len; off += PAGE) {
    size_t n = min((size_t)PAGE, len - off);
    if (!swd_read_block(STM32_FLASH_BASE + off, buf, (n + 3) / 4)) return -1;
    if (progress) progress(off + n, len);
    const uint8_t *got = (const uint8_t *)buf;
    for (size_t i = 0; i < n; i++) {
      if (got[i] != image[off + i]) {
        Serial.printf("stm32: verify mismatch @ 0x%08lX: wrote 0x%02X read 0x%02X\n",
                      STM32_FLASH_BASE + off + i, image[off + i], got[i]);
        return 0;
      }
    }
  }
  return 1;
}

void stm32_run() {
  swd_write32(DEMCR, 0);              // no vector catch on this reset
  swd_write32(DHCSR, DBGKEY);         // release halt, debug off
  swd_write32(AIRCR, SYSRESETREQ);
  delay(5);
  swd_disconnect();
}
