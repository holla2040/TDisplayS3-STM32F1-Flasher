# TDisplayS3-STM32F1-Flasher

Production-jig flasher for STM32F103 boards (developed against a Bluepill,
STM32F103C8T6): a LilyGO T-Display S3 programs the target over bit-banged SWD
via the Bluepill's 4-pin SWD header. Ported from TDisplayS3-STM8S-Flasher
(same UI, web page, state machine); only the wire and device layers differ.

## Build / deploy

- `make bin` — compile into /tmp/arduino-build-stm32 (own dir: the STM8 jig uses /tmp/arduino-build)
- `make ota` / `make usb` — deploy to STM32Flasher.local / /dev/ttyACM0
- `make image IMG=path/to/app.ino.bin` — push a target image (.bin or Intel HEX)
- ihx host test: header of test_ihx.c.txt

## Layout

- `TDisplayS3-STM32F1-Flasher.ino` — UI, state machine, wifi/OTA/web, FFat image (`/firmware.img`, .bin or hex as uploaded)
- `swd.cpp/h` — ADIv5 SWD host: transactions (WAIT retry, FAULT clears sticky), AHB-AP 32-bit/16-bit block access, TAR re-set every 1 KB
- `stm32f1.cpp/h` — probe (DBGMCU_IDCODE, flash size, RDP), reset-halt, FPEC page erase/program, verify, run
- `config.h` — SWDIO=GPIO1, SWCLK=GPIO2, SWD_HALF_PERIOD (clock speed knob)

## Behavior contracts (don't regress)

- Erase only the pages the image covers: EEPROM-emulation data at the top of flash must survive. NO mass erase.
- No NRST on the SWD header: reset = SYSRESETREQ + DEMCR vector catch.
- Probe is non-intrusive (no halt, no reset), once a second in both modes.
- FAIL = verify mismatch only; comm errors retry 3x then an orange notice. Boards the jig won't touch get an orange notice too (stm32_refusal: "locked" = RDP, "unsupported" = not dev 0x410/0x412; "too big" = image > chip flash).
- Auto mode flashes once per insertion: `flashed` is set after ANY outcome (comm error included) and cleared on disconnect, so a seated board is never reflashed.
- The last result word stays on the home screen (line 4) until the board is removed.
- MODE_BUTTON (GPIO0): short press toggles auto/manual, held 5 s forgets WiFi. It can't be read at power-up (boot strap). Mode letter is hidden: enclosure has no buttons.
- WiFi never blocks flashing.
- flashing-shop.md is a local, untracked note (.gitignore).

## Caveats

- Verified on a Bluepill (2026-10-09): probe, reset-halt, program, verify PASS. SWD at ~1–2 MHz; if ACKs are flaky on the cable, raise SWD_HALF_PERIOD.
- Flash programming writes halfwords straight through the AHB-AP and relies on WAIT while the FPEC is busy (same as Black Magic Probe's stm32f1 driver); no RAM loader.
- PAGE is 1 KB (low/medium density); other parts are refused at probe time.
- If the target firmware remaps PA13/PA14 away from SWD, the jig can't attach (no NRST for connect-under-reset).
