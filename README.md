# TDisplayS3-STM32F1-Flasher

Standalone production flasher for STM32F103 boards: a LilyGO T-Display S3
(ESP32-S3) programs the target over SWD. No PC, no ST-Link on the line. You
upload the target firmware to the jig over WiFi once; after that an operator
connects a board, the jig flashes and verifies it, and shows PASS or FAIL on
its screen. Developed and verified on a **Bluepill** (STM32F103C8T6).

Sibling project for STM8 targets: [TDisplayS3-STM8S-Flasher](https://github.com/holla2040/TDisplayS3-STM8S-Flasher).

## What you need

| Item | Notes |
|---|---|
| LilyGO T-Display S3, LCD version (not AMOLED), 16 MB flash / 8 MB PSRAM | Tested on the non-touch version. The build assumes 16 MB flash and OPI PSRAM. |
| Target board with an STM32F103 | Bluepill or your own PCB; see [Supported targets](#supported-targets-and-limits). |
| 4 wires, or a pogo-pin / clip fixture | Keep them short; see [Wiring](#wiring). |
| 5 V USB-C supply for the jig | The jig powers the target from its 3V3 pin. |
| A computer with arduino-cli | Only to build the jig firmware and the target image. |

## Build the jig firmware

Tested with arduino-cli 1.3.1, esp32 core 3.3.0, TFT_eSPI 2.5.43 and
WiFiManager 2.0.17 (by tzapu). Other versions probably work; these are known good.

```
arduino-cli config add board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32@3.3.0
arduino-cli lib install "TFT_eSPI@2.5.43" "WiFiManager@2.0.17"
```

**TFT_eSPI needs one manual edit** or the build succeeds and the screen stays
black. In `<sketchbook>/libraries/TFT_eSPI/User_Setup_Select.h` (sketchbook is
usually `~/Arduino`), comment out `#include <User_Setup.h>` and uncomment
`#include <User_Setups/Setup206_LilyGo_T_Display_S3.h>`. The edit applies to
every sketch that uses TFT_eSPI and is lost when the library is updated.

Clone into a folder named exactly `TDisplayS3-STM32F1-Flasher` (arduino-cli
requires the folder to match the .ino name; a GitHub ZIP unpacks to `...-main`,
so rename it). Then, on Linux/macOS:

```
make bin                        # compile into /tmp/arduino-build-stm32
make usb port=/dev/ttyACM0      # upload over USB
make test                       # optional: host-side check of the Intel HEX parser (gcc)
```

On Windows, or without make, use arduino-cli directly with the same board settings:

```
arduino-cli compile -b esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi --output-dir build .
arduino-cli upload  -b esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi -p COM5 --input-dir build .
```

Arduino IDE equivalent: board "ESP32S3 Dev Module", USB CDC On Boot: Enabled,
Flash Size: 16MB, Partition Scheme: "16M Flash (3MB APP/9.9MB FATFS)", PSRAM: OPI PSRAM.

If no serial port shows up for the first USB upload: hold BOOT, tap RST,
release BOOT, then upload. After that, updates can go over WiFi
([Updating the jig](#updating-the-jig)).

## Wiring

3.3 V, direct connection to the Bluepill's 4-pin SWD header:

![T-Display S3 to Bluepill SWD wiring](docs/wiring.png)

| T-Display S3 | Bluepill SWD header |
|---|---|
| GPIO 1 | DIO (SWDIO / PA13) |
| GPIO 2 | CLK (SWCLK / PA14) |
| GND | GND |
| 3V3 | 3.3 — the jig powers the board |

- No resistors. PA13 has an internal pull-up.
- NRST isn't used. The SWD header doesn't carry it, so the jig resets the target over SWD (SYSRESETREQ).
- Leave the Bluepill's USB unplugged while it's on the jig: that would tie the Bluepill's 3.3 V regulator to the jig's 3V3.
- BOOT0 must be 0 (both Bluepill jumpers toward the USB connector) or the flashed firmware won't run after reset.
- The 4-pin header often ships unsoldered, and its pin order varies between clones: follow the silkscreen, not the picture.
- Keep wires under about 20 cm. If connections are flaky (orange `comm error`, `no target` with a board connected), slow the SWD clock: raise `SWD_HALF_PERIOD` in `config.h` (60 is the default; try 120-240 for long leads or pogo fixtures).
- Self-powered targets: connect GND, DIO and CLK only, not 3V3.
- If the jig resets when a board is plugged in (brown-out from the board's inrush), power the target separately.

On your own PCB, bring SWDIO (PA13), SWCLK (PA14), GND and 3V3 to a header
or test pads, and keep PA13/PA14 free for SWD (see [limits](#supported-targets-and-limits)).

## First boot and WiFi

WiFi is only needed to load the target image and to watch status from a
browser. Flashing works without it.

1. On first boot the jig opens an open WiFi access point named `STM32Flasher`
   (no password); the screen's bottom line shows `AP: STM32Flasher`.
2. Join it from a phone or laptop. The setup page should pop up; if not, browse
   to http://192.168.4.1/. Pick your network and enter its password.
   2.4 GHz WPA2-PSK only; WPA2-Enterprise (common on factory networks) isn't supported.
3. The jig joins the network and shows its IP address on the bottom line. Its
   web page is at that IP, or `http://STM32Flasher.local/` where mDNS works
   (often not on Windows or corporate networks: use the IP).

The jig remembers the network. If that network is out of range at power-up,
the setup AP comes back by itself. To forget it deliberately, **hold BOOT for
5 seconds** while the jig is running: the screen shows `wifi forgotten` and the
jig restarts into the setup AP. (Don't hold BOOT while powering up: that puts
the ESP32-S3 into its USB download mode instead.)

More than one jig on the same network: give each a unique name by changing
`PROJECTNAME` in `TDisplayS3-STM32F1-Flasher.ino` (e.g. `STM32Flasher-2`)
before building. Otherwise their hostnames and setup APs collide.

## Making the target image

The jig writes the image at 0x08000000, the start of flash, with no bootloader
in front of it. Your build must be linked for that address.

**Intel HEX is the safer format.** It carries addresses, so a file linked for
the wrong address is rejected. A raw `.bin` carries no address: one linked for
a USB bootloader (at 0x08002000, say) can pass the jig's checks, flash, verify,
show PASS, and then never boot.

- **Arduino, stm32duino core** (board manager URL
  `https://github.com/stm32duino/BoardManagerFiles/raw/main/package_stmicroelectronics_index.json`):
  ```
  arduino-cli compile -b STMicroelectronics:stm32:GenF1:pnum=BLUEPILL_F103C8,upload_method=swdMethod --output-dir out mysketch
  ```
  Upload `out/mysketch.ino.hex`. `upload_method=swdMethod` (or serialMethod /
  dfuMethod / bmpMethod) links at 0x08000000. **Never use the HID or Maple DFU
  upload methods**: they link above 0x08000000.
- **STM32CubeIDE / Makefile / plain arm-none-eabi**: the default linker script
  (FLASH ORIGIN = 0x08000000) is right. Convert with
  `arm-none-eabi-objcopy -O ihex app.elf app.hex`.

Notes:

- Flashing overwrites any USB bootloader at 0x08000000 (Maple, HID). The board
  will no longer enumerate as a USB DFU/HID device.
- Keep SWD enabled in production firmware (don't remap PA13/PA14 or disable
  SWJ in AFIO). Otherwise the jig can never reach the board again; the header has no NRST.
- Images up to 64 KB.

## Loading the image onto the jig

Any of:

- The jig's web page: **Target firmware** → choose the file → **Upload and flash**.
- `curl -F image=@app.hex http://<jig-ip>/upload`
- `make image IMG=app.hex ip=<jig-ip>`

The jig stores the image on its own flash; it survives power cycles and jig
firmware updates. **A board already on the jig is flashed as soon as the upload
finishes.**

To confirm which firmware is loaded, the web page and `/status` show the stored
image's size and CRC32. It equals the CRC32 of the `.bin` (`crc32 app.bin`, or
Python `zlib.crc32`), whether you uploaded the `.bin` or the `.hex`.

Before replacing the stored image, the jig checks it. It rejects the file,
keeps the previous image, and returns HTTP 400 when:

- a HEX record has a bad checksum, or data outside 0x08000000-0x0800FFFF (option bytes, for example);
- the image is over 64 KB;
- the vector table doesn't look like an STM32 app: the stack pointer must be in SRAM (0x200xxxxx), and the reset vector must point inside the image and be odd (Thumb).

Gaps in a HEX file are filled with 0xFF. Only record types 00, 01 and 04 are used.

## Production operation

### Operator steps

1. Power the jig. The home screen shows the board status, the image size, the
   counters and the IP address.
2. Connect a board. Within about a second the jig switches to the flash screen:
   `write n/total`, then `read n/total`.
3. Read the result bar, then disconnect the board. The result also stays on
   the home screen until the board is removed.
4. Connect the next board.

The jig flashes each board **once per connection**. A board left connected is
never reflashed, whatever its result. To retry, disconnect and reconnect it.

Measured: an 11 KB image takes about 3.5 s from upload to PASS. Time scales
with image size.

### Screen messages

| Screen | Meaning | What to do |
|---|---|---|
| `no target` | Nothing answered on SWD. | Check the connection, BOOT0, and that the board is powered. |
| `STM32F103 64K` | Board detected (part, flash size). | — |
| `STM32F103 64K locked` | Read-out protection (RDP) is set. | See locked, below. |
| `dev 414 256K` (any other number) | Not a supported part. | Don't run; see [limits](#supported-targets-and-limits). |
| `no image loaded` | No target image stored yet. | Load one. |
| green `PASS` | Flashed, read back, matched. The board was reset and is running the new firmware. | Good board. |
| red `FAIL verify mismatch` | Read-back didn't match the image. | Reject. A board that FAILs twice is bad. |
| orange `comm error no verdict` | Three attempts couldn't finish. Usually contact or wiring, but also: the chip refused to erase/program (write-protected pages). | Reconnect once. If it repeats, set it aside and check the serial log. |
| orange `locked skipped` | RDP set; the jig doesn't touch it. | Rework: unlock (below). |
| orange `unsupported skipped` | Not an F103 low/medium-density part. | Wrong part. |
| orange `too big for chip` | The image is larger than this chip's flash. | Wrong part or wrong image. |
| `AP: STM32Flasher` / `wifi: connecting` | Not on WiFi (bottom line). | Only matters for uploads; see [WiFi](#first-boot-and-wifi). |
| `updating flasher` | Jig firmware update in progress. | Wait; it restarts. |

FAIL means one thing only: the read-back didn't match. A missing board or a
communication problem is never shown as FAIL. After a FAIL the board is still
reset and runs whatever is in its flash.

**Unlocking an RDP-locked chip** (this mass-erases it) needs an ST-Link or
similar:
```
openocd -f interface/stlink.cfg -f target/stm32f1x.cfg -c "init; reset halt; stm32f1x unlock 0; reset halt; exit"
```
or `STM32_Programmer_CLI -c port=SWD mode=UR -ob RDP=0xA5`. Then power-cycle the board.

### Counters, buttons and mode

- `pass` and `fail` count flash results since power-up, not boards: a board
  reconnected and flashed again counts again. They reset on power-cycle;
  record them first. `comm error` and skipped boards count as neither.
- Auto mode (the default) flashes every newly connected board. The mode isn't
  shown on the screen; the web page shows it. A short press of BOOT switches
  to manual mode, where nothing flashes until KEY (GPIO14) is pressed. If
  boards are detected but never flash, the jig is in manual mode: press BOOT
  once or power-cycle (it always starts in auto).
- KEY during the 1 s result display re-flashes the same board, in either mode.
- BOOT held 5 s forgets WiFi ([above](#first-boot-and-wifi)).

## What a flash does

1. Attach over SWD (line reset, JTAG-to-SWD switch, read IDCODE, power up the debug port).
2. Halt the core and reset it with reset vector catch (SYSRESETREQ), so the core stops before the old firmware runs an instruction, with all peripherals and any watchdog back at reset state.
3. Unlock the flash controller. For each 1 KB page the image covers: erase the page, then program it a halfword at a time.
4. Read the whole image back and compare.
5. Release the core and reset: the board runs the new firmware.

**Only the pages the image covers are erased.** Data a sketch keeps in
EEPROM-emulation pages at the top of flash (for example the Arduino EEPROM
library) survives a reflash, as long as the image doesn't grow into those
pages. There's no option to wipe it.

Between flashes the jig probes once a second: it attaches over SWD and reads
the chip ID. That doesn't halt or reset the board.

## Supported targets and limits

- **Supported**: STM32F103 low- and medium-density parts (device ID 0x412 and
  0x410: F103x4/x6/x8/xB), which have 1 KB flash pages. Verified on an
  STM32F103C8T6 Bluepill.
- **Refused** (orange `unsupported`): every other device ID, including
  high-density F103s (2 KB pages), other STM32 families, and the "Black Pill" (F401/F411).
- **Untested**: clones (GD32, CKS32, APM32 and others) report the same device
  ID and show as STM32F103; they may or may not program correctly.
- **Images up to 64 KB**, even on 128 KB parts.
- Write-protected pages (WRP option bytes) show as `comm error`.
- If the target firmware uses PA13/PA14 as GPIO, the once-a-second probe will
  fight it, and the board shows `no target` after its first flash.
- PASS checks the flash contents only. Option bytes aren't programmed or
  checked, and the board isn't functionally tested.

## Updating the jig

The target image and the WiFi settings survive a jig update.

- **Web page**: **Flasher firmware** → choose `TDisplayS3-STM32F1-Flasher.ino.bin`
  from the build folder (not the `.merged.bin` or `.bootloader.bin`) → **Update flasher**.
  The page reports when the jig is back on the new firmware.
- **curl**: `curl -F firmware=@TDisplayS3-STM32F1-Flasher.ino.bin http://<jig-ip>/update`
- **ArduinoOTA**: `make ota ip=<jig-ip>` (uses espota.py from the esp32 core; set
  `ESPOTA=` if yours lives elsewhere).
- **USB**: `make usb port=/dev/ttyACM0`.

ArduinoOTA from WSL2: espota needs the jig to connect back to the PC. Set
`networkingMode=mirrored` in `%USERPROFILE%\.wslconfig` (then `wsl --shutdown`),
allow inbound TCP 3233 in both the Windows firewall and the Hyper-V firewall
for WSL, and add `-P 3233` to the espota command. The web page method avoids all of this.

## Troubleshooting

The jig logs to USB serial at 115200 baud
(`arduino-cli monitor -p /dev/ttyACM0 -c baudrate=115200`).

| Log line | Meaning |
|---|---|
| `detected: STM32F103 64K` / `detected: no target` | Probe result changed. |
| `stm32: connected (IDCODE 0x1BA01477), core halted at reset` | Attach and reset-halt worked. |
| `stm32: reset-halt failed, DHCSR=...` | Attached, but the core wouldn't halt. Check BOOT0 and power. |
| `stm32: FPEC unlock rejected` | Flash controller stayed locked. |
| `stm32: flash error, SR=0x..` | Flash status bits: 0x10 = write-protected page (WRP), 0x04 = programming error. |
| `stm32: flash busy timeout` | Erase/program didn't finish. |
| `stm32: page @ 0x08000400 failed` | Which page the above happened on. |
| `stm32: verify mismatch @ 0x...: wrote 0x.. read 0x..` | The FAIL detail. |
| `flash: comm error, attempt n/3` | Retrying within one connection. |
| `image rejected: N bytes, SP 0x..., reset 0x...` | Upload failed the image checks. |

`no target` with a board connected: the wiring, BOOT0, a dead board, firmware
that disabled SWD, or wires too long for the SWD clock (raise `SWD_HALF_PERIOD`).

## Integration and security

- `GET /status` returns JSON, e.g.
  `{"detected":"STM32F103 64K","image":11192,"crc":"4aeabe4d","pass":2,"fail":0,"auto":true,"build":"...","md5":"..."}`.
  `image` and `crc` are the stored target image's size and CRC32; `md5` identifies the jig firmware.
  There's no per-board log or serial-number traceability.
- **Nothing is authenticated**: the setup AP, `/upload`, `/update` and
  ArduinoOTA are open to anyone on the same network. Anyone who can reach the
  jig can replace the target image or the jig firmware. Put jigs on an
  isolated network.

## Status

Verified on hardware: probe, reset-halt, program and verify of a Bluepill
(STM32F103C8, IDCODE 0x1BA01477), with both `.bin` and `.hex` images.

## License

MIT — see [LICENSE](LICENSE).
