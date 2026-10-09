#include <Arduino.h>
#include "driver/gpio.h"
#include "soc/gpio_struct.h"
#include "config.h"
#include "swd.h"

// ARM ADIv5 references: IHI0031 (Debug Interface Architecture). The
// transaction format is the same on every Cortex-M; nothing here is STM32-specific.

#define ACK_OK    1
#define ACK_WAIT  2
#define ACK_FAULT 4
#define WAIT_RETRIES 200   // a flash halfword program stalls the AHB-AP ~50 us

// DP registers (bank 0) and AHB-AP registers (AP 0, bank 0)
#define DP_IDCODE 0x0   // read
#define DP_ABORT  0x0   // write
#define DP_CTRL   0x4
#define DP_SELECT 0x8
#define DP_RDBUFF 0xC
#define AP_CSW    0x0
#define AP_TAR    0x4
#define AP_DRW    0xC

// CSW: 0x23000000 = HPROT data/privileged + MasterDebug; low bits = size/increment
#define CSW_32    0x23000002
#define CSW_32INC 0x23000012
#define CSW_16INC 0x23000011

// Request header: start, APnDP, RnW, A[3:2], parity(APnDP..A3), stop, park.
static constexpr uint8_t req(bool ap, bool rd, uint8_t a) {
  return 0x81 | ap << 1 | rd << 2 | (a & 0xC) << 1 | (ap ^ rd ^ (a >> 2 & 1) ^ (a >> 3 & 1)) << 5;
}
static_assert(req(false, true, DP_IDCODE) == 0xA5, "DP IDCODE read header");
static_assert(req(true, false, AP_TAR) == 0x8B, "AP TAR write header");
static_assert(req(true, true, AP_DRW) == 0x9F, "AP DRW read header");

static const uint32_t DIO = 1UL << SWDIO_PIN, CLK = 1UL << SWCLK_PIN;

static inline void half() {
  uint32_t t0 = ESP.getCycleCount();
  while (ESP.getCycleCount() - t0 < SWD_HALF_PERIOD) {}
}
static inline void drive(bool on) {
  if (on) GPIO.enable_w1ts = DIO;
  else GPIO.enable_w1tc = DIO;
}

// Host changes SWDIO while SWCLK is low; target samples on the rising edge.
static void write_bits(uint32_t v, int n) {
  for (int i = 0; i < n; i++, v >>= 1) {
    if (v & 1) GPIO.out_w1ts = DIO;
    else GPIO.out_w1tc = DIO;
    GPIO.out_w1tc = CLK; half();
    GPIO.out_w1ts = CLK; half();
  }
}

// Target changes SWDIO on the rising edge; host samples just before the next.
static uint32_t read_bits(int n) {
  uint32_t v = 0;
  for (int i = 0; i < n; i++) {
    GPIO.out_w1tc = CLK; half();
    if (GPIO.in & DIO) v |= 1UL << i;
    GPIO.out_w1ts = CLK; half();
  }
  return v;
}

static int xfer(uint8_t r, uint32_t *data);

// FAULT sets a sticky error that blocks every later AP access until cleared.
static void clear_sticky() {
  uint32_t v = 0x1E;   // STKCMPCLR | STKERRCLR | WDERRCLR | ORUNERRCLR
  xfer(req(false, false, DP_ABORT), &v);
}

// One SWD transaction, retried on WAIT. Returns the final ACK (or -1 on
// read parity error). A floating line (no target) reads as ACK 0b111.
static int xfer(uint8_t r, uint32_t *data) {
  for (int tries = 0; tries < WAIT_RETRIES; tries++) {
    drive(true);
    write_bits(r, 8);
    drive(false);
    read_bits(1);                                  // turnaround
    int ack = read_bits(3);
    if (ack != ACK_OK) {
      read_bits(1);                                // turnaround
      drive(true);
      write_bits(0, 8);
      if (ack == ACK_WAIT) continue;
      if (ack == ACK_FAULT) clear_sticky();
      return ack;
    }
    if (r & 0x04) {                                // read
      uint32_t v = read_bits(32);
      uint32_t par = read_bits(1);
      read_bits(1);                                // turnaround
      drive(true);
      write_bits(0, 8);                            // idle
      if (par != (uint32_t)(__builtin_popcount(v) & 1)) return -1;
      *data = v;
    } else {
      read_bits(1);                                // turnaround
      drive(true);
      write_bits(*data, 32);
      write_bits(__builtin_popcount(*data) & 1, 1);
      write_bits(0, 8);                            // idle: lets the posted write go out
    }
    return ACK_OK;
  }
  return ACK_WAIT;
}

static bool dp_read(uint8_t a, uint32_t *v)  { return xfer(req(false, true, a), v) == ACK_OK; }
static bool dp_write(uint8_t a, uint32_t v)  { return xfer(req(false, false, a), &v) == ACK_OK; }
static bool ap_read(uint8_t a, uint32_t *v)  { return xfer(req(true, true, a), v) == ACK_OK; }   // posted: returns the PREVIOUS read
static bool ap_write(uint8_t a, uint32_t v)  { return xfer(req(true, false, a), &v) == ACK_OK; }
static bool ap_setup(uint32_t csw, uint32_t addr) {
  return ap_write(AP_CSW, csw) && ap_write(AP_TAR, addr);
}

void swd_init() {
  gpio_config_t c = {};
  c.pin_bit_mask = 1ULL << SWDIO_PIN;
  c.mode = GPIO_MODE_INPUT_OUTPUT;        // read back while released
  c.pull_up_en = GPIO_PULLUP_ENABLE;      // no target => ACK reads 0b111
  gpio_config(&c);
  drive(false);
  pinMode(SWCLK_PIN, OUTPUT);
  GPIO.out_w1ts = CLK;
}

bool swd_connect(uint32_t *idcode) {
  drive(true);
  write_bits(0xFFFFFFFF, 32); write_bits(0xFFFFFFFF, 24);   // line reset (>= 50 high)
  write_bits(0xE79E, 16);                                   // JTAG-to-SWD select
  write_bits(0xFFFFFFFF, 32); write_bits(0xFFFFFFFF, 24);
  write_bits(0, 8);
  if (!dp_read(DP_IDCODE, idcode)) return false;            // mandatory first read
  if (!dp_write(DP_ABORT, 0x1E)) return false;
  if (!dp_write(DP_CTRL, 0x50000000)) return false;         // CSYSPWRUPREQ | CDBGPWRUPREQ
  uint32_t ctrl = 0;
  for (int i = 0; i < 100 && (ctrl & 0xA0000000) != 0xA0000000; i++)
    if (!dp_read(DP_CTRL, &ctrl)) return false;
  if ((ctrl & 0xA0000000) != 0xA0000000) return false;      // power-up never acked
  return dp_write(DP_SELECT, 0);
}

void swd_disconnect() {
  dp_write(DP_CTRL, 0);
}

bool swd_read32(uint32_t addr, uint32_t *val) {
  uint32_t stale;
  return ap_setup(CSW_32, addr) && ap_read(AP_DRW, &stale) && dp_read(DP_RDBUFF, val);
}

bool swd_write32(uint32_t addr, uint32_t val) {
  uint32_t flush;
  return ap_setup(CSW_32, addr) && ap_write(AP_DRW, val) && dp_read(DP_RDBUFF, &flush);
}

// TAR auto-increment is only guaranteed within a 1 KB block, so re-set TAR
// at every 1 KB boundary.
bool swd_read_block(uint32_t addr, uint32_t *buf, size_t words) {
  while (words) {
    size_t n = min(words, (size_t)((0x400 - (addr & 0x3FF)) / 4));
    uint32_t stale;
    if (!ap_setup(CSW_32INC, addr) || !ap_read(AP_DRW, &stale)) return false;
    for (size_t i = 1; i < n; i++)
      if (!ap_read(AP_DRW, &buf[i - 1])) return false;
    if (!dp_read(DP_RDBUFF, &buf[n - 1])) return false;
    addr += n * 4; buf += n; words -= n;
  }
  return true;
}

bool swd_write_block16(uint32_t addr, const uint16_t *buf, size_t halfwords) {
  while (halfwords) {
    size_t n = min(halfwords, (size_t)((0x400 - (addr & 0x3FF)) / 2));
    if (!ap_setup(CSW_16INC, addr)) return false;
    for (size_t i = 0; i < n; i++) {
      uint32_t lane = ((addr + i * 2) & 2) * 8;   // halfword sits on its byte lane
      if (!ap_write(AP_DRW, (uint32_t)buf[i] << lane)) return false;
    }
    addr += n * 2; buf += n; halfwords -= n;
  }
  uint32_t flush;
  return dp_read(DP_RDBUFF, &flush);
}
