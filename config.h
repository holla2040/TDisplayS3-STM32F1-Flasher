#pragma once

// --- Pin assignments (LilyGO T-Display S3; adjust to your jig wiring) ---
// Direct 3.3 V connection to the Bluepill's 4-pin SWD header. NRST isn't on
// that header, so resets go through SYSRESETREQ.
#define SWDIO_PIN       1   // header DIO; target has an internal pull-up on PA13
#define SWCLK_PIN       2   // header CLK

// SWD clock half-period in ESP32 CPU cycles (240 MHz). 60 ≈ 2 MHz max; GPIO
// latency makes it slower in practice. Raise it for long cables / flaky ACKs.
#define SWD_HALF_PERIOD 60

// T-Display S3 user buttons (per LilyGO schematic: BOOT=IO0, KEY=IO14)
#define MODE_BUTTON     0   // short press: toggle auto/manual; hold 5 s: forget WiFi
#define FLASH_BUTTON    14  // manual mode: press to flash
