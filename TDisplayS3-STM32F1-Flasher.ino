#include <WiFiClient.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <FFat.h>
#include <Update.h>
#include <TFT_eSPI.h>
#include <esp_rom_crc.h>

#include "config.h"
#include "swd.h"
#include "stm32f1.h"
#include "ihx.h"
#include "webpage.h"

#define PROJECTNAME "STM32Flasher"
#define RESULT_HOLD_MS 1000   // pass/fail bar hold on the flash page

TFT_eSPI tft = TFT_eSPI();
WiFiManager wifiManager;
WebServer server(80);

// Target image lives in FFat, uploaded over HTTP; survives reboots.
static const char *IMAGE_PATH = "/firmware.img";   // raw .bin or Intel HEX, as uploaded
static const char *UPLOAD_TMP = "/upload.tmp";
static File uploadFile;

alignas(4) static uint8_t image[64 * 1024];   // Bluepill: STM32F103C8, 64 KB flash
static size_t imageLen = 0;
static uint32_t imageCrc = 0;   // CRC32 of the image bytes: matches `crc32 app.bin` / Python zlib.crc32
static uint32_t passCount = 0, failCount = 0;

// Auto mode flashes once per insertion: set after every attempt (PASS, FAIL,
// skipped, or comm error), cleared when the target disconnects. A comm-error
// board is NOT retried while it stays seated: every retry rewrites flash.
static bool flashed = false;

// Last verdict, shown on the home screen until the board is removed.
static char lastResult[16] = "";
static uint16_t lastBg = TFT_BLACK;

// WiFi is secondary: the jig flashes with or without it. WiFiManager runs
// non-blocking; OTA + mDNS start whenever a connection eventually appears.
static bool netUp = false;

static uint32_t lastPoll = 0;
static bool autoMode = true;   // default auto (enclosure has no buttons); MODE_BUTTON toggles
static char detected[32] = "no target";   // live probe result, home page line 1

// Home page: status + waiting for the next target.
static void showHome() {
  tft.fillScreen(TFT_BLACK);
  tft.setCursor(0, 0, 2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.println(detected);                    // live detection
  if (imageLen) tft.printf("image %u\n", (unsigned)imageLen);
  else tft.println("no image loaded");
  tft.printf("pass %lu fail %lu\n", passCount, failCount);
  if (lastResult[0]) {
    tft.setTextColor(TFT_WHITE, lastBg);
    tft.printf(" %s ", lastResult);
  }
  tft.setCursor(0, 4 * 32, 2);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  if (WiFi.status() == WL_CONNECTED) tft.print(WiFi.localIP());
  else tft.print(wifiManager.getConfigPortalActive() ? "AP: " PROJECTNAME : "wifi: connecting");

  // ponytail: mode letter hidden — the enclosure has no button access, so the
  // jig always runs auto. Toggle code stays; re-enable this if buttons return.
  // tft.setTextColor(TFT_CYAN, TFT_BLACK);
  // tft.setTextDatum(TR_DATUM);
  // tft.drawString(autoMode ? "A" : "M", tft.width() - 6, 0, 2);   // enclosure covers the right edge
  // tft.setTextDatum(TL_DATUM);
}

static bool wantRetest = false;
static bool wantUploadFlash = false;   // fresh upload: flash immediately from loop()

// Debounced release: returns once the pin has read high for 50 ms straight.
// Every button-triggered test starts from here, so a single press can never
// span into the next page and fire twice.
static void waitRelease(uint8_t pin) {
  uint32_t hi = millis();
  while (millis() - hi < 50) {
    if (!digitalRead(pin)) hi = millis();
    delay(5);
  }
}

// Result line at the bottom of the flash page: hold RESULT_HOLD_MS, then home.
// FLASH_BUTTON during the hold cuts it short and requests a retest.
static void showResult(const char *word, uint16_t bg, const char *stage) {
  tft.setCursor(0, 4 * 32, 2);
  tft.setTextColor(TFT_WHITE, bg);
  tft.printf(" %s %s ", word, stage);
  Serial.printf("%s: %s\n", word, stage);
  strlcpy(lastResult, word, sizeof(lastResult));
  lastBg = bg;
  uint32_t start = millis();
  while (millis() - start < RESULT_HOLD_MS) {
    if (!digitalRead(FLASH_BUTTON)) {
      waitRelease(FLASH_BUTTON);
      wantRetest = true;
      return;
    }
    delay(10);
  }
}

// Load an uploaded image from FFat into the RAM buffer: Intel HEX if it
// starts with ':', otherwise a raw .bin that must be linked at 0x08000000.
// Either way the vector table must look like an STM32F1 app — catches an
// obviously wrong file (e.g. this flasher's own .ino.bin). A .bin carries no
// address, so one linked above 0x08000000 (for a USB bootloader) can still
// pass; a HEX file can't, which is why HEX is the safer format.
// On failure the buffer is trashed — caller must reload.
static bool loadImage(const char *path) {
  File f = FFat.open(path);
  if (!f) return false;
  size_t sz = f.size();
  char *raw = (char *)malloc(sz + 1);
  if (!raw) { f.close(); return false; }
  f.read((uint8_t *)raw, sz);
  raw[sz] = 0;
  f.close();
  size_t n = 0;
  if (sz && raw[0] == ':') {
    n = ihx_parse(raw, STM32_FLASH_BASE, image, sizeof(image));
  } else if (sz <= sizeof(image)) {
    memset(image, 0xFF, sizeof(image));
    memcpy(image, raw, sz);
    n = sz;
  }
  free(raw);
  uint32_t sp, pc;
  memcpy(&sp, image, 4);
  memcpy(&pc, image + 4, 4);
  bool vectors = (sp & 0xFFF00000) == 0x20000000 && (pc & 1) &&
                 pc >= STM32_FLASH_BASE && pc < STM32_FLASH_BASE + n;
  if (n < 8 || !vectors) {
    Serial.printf("image rejected: %u bytes, SP 0x%08lX, reset 0x%08lX\n", (unsigned)n, sp, pc);
    return false;
  }
  imageLen = n;
  imageCrc = esp_rom_crc32_le(0, image, n);
  return true;
}

static void handleRoot() {
  server.send(200, "text/html", PAGE);
}

static void handleStatus() {
  static String md5 = ESP.getSketchMD5();   // lets the page confirm a firmware update took
  char json[320];
  snprintf(json, sizeof(json),
    "{\"detected\":\"%s\",\"image\":%u,\"crc\":\"%08lx\",\"pass\":%lu,\"fail\":%lu,\"auto\":%s,"
    "\"build\":\"" __DATE__ " " __TIME__ "\",\"md5\":\"%s\"}",
    detected, (unsigned)imageLen, imageCrc, passCount, failCount, autoMode ? "true" : "false", md5.c_str());
  server.send(200, "application/json", json);
}

// Flasher self-update: stream the .ino.bin into the spare OTA slot. Update
// rejects anything not starting with the ESP image magic byte.
static void handleUpdateChunk() {
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    tft.fillScreen(TFT_BLACK);
    tft.setCursor(0, 0, 2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.println("updating flasher");
    Serial.printf("firmware update: %s\n", up.filename.c_str());
    Update.begin(UPDATE_SIZE_UNKNOWN);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    Update.write(up.buf, up.currentSize);
  } else if (up.status == UPLOAD_FILE_END) {
    Update.end(true);
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
  }
}

static void handleUpdateDone() {
  if (Update.hasError() || !Update.isFinished()) {
    char msg[96];
    snprintf(msg, sizeof(msg), "Update failed: %s. The flasher kept its current firmware.",
             Update.errorString());
    Serial.println(msg);
    server.send(500, "text/plain", msg);
    Update.abort();
    showHome();
    return;
  }
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain", "ok: restarting\n");
  Serial.println("firmware update ok, restarting");
  delay(300);
  ESP.restart();
}

static void handleUploadChunk() {
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) uploadFile = FFat.open(UPLOAD_TMP, FILE_WRITE);
  else if (up.status == UPLOAD_FILE_WRITE && uploadFile) uploadFile.write(up.buf, up.currentSize);
  else if (up.status == UPLOAD_FILE_END && uploadFile) uploadFile.close();
}

static void handleUploadDone() {
  if (!loadImage(UPLOAD_TMP)) {
    FFat.remove(UPLOAD_TMP);
    if (!loadImage(IMAGE_PATH)) imageLen = 0;   // restore previous image to RAM
    server.send(400, "text/plain", "That file isn't an STM32F1 firmware image linked at 0x08000000 (.bin or Intel HEX, 64 KB max). The previous target image is still loaded.\n");
    return;
  }
  FFat.remove(IMAGE_PATH);
  FFat.rename(UPLOAD_TMP, IMAGE_PATH);
  char msg[48];
  snprintf(msg, sizeof(msg), "ok: image %u bytes\n", (unsigned)imageLen);
  server.send(200, "text/plain", msg);
  Serial.printf("new image: %u bytes\n", (unsigned)imageLen);
  showHome();
  wantUploadFlash = true;   // flash the new image right away, loop() picks it up
}

// Programming page: white on black, one line per phase, updated in place.
static const int LINE_H = 32;   // font 2 (16 px) at textSize 2

static void writeProgress(size_t done, size_t total) {
  tft.setCursor(0, 2 * LINE_H, 2);
  tft.printf("write %u/%u  ", (unsigned)done, (unsigned)total);
}

static void readProgress(size_t done, size_t total) {
  tft.setCursor(0, 3 * LINE_H, 2);
  tft.printf("read  %u/%u  ", (unsigned)done, (unsigned)total);
}

// Target is present: reset-halt, program, verify, run. Blocking is fine.
// Returns false on a communication hiccup: no verdict, caller just retries.
// FAIL means one thing only: the readback didn't match the image.
static bool flashCycle() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(0, 0, 2);
  tft.printf("image %u", (unsigned)imageLen);
  tft.setCursor(0, LINE_H, 2);
  tft.print(detected);

  // Boards the jig won't touch get an orange notice, never a FAIL.
  if (const char *why = stm32_refusal()) {   // "locked" (RDP set) or "unsupported"
    showResult(why, TFT_ORANGE, "skipped");
    return true;
  }
  if (imageLen > stm32_flash_size()) {
    showResult("too big", TFT_ORANGE, "for chip");
    return true;
  }
  if (!stm32_connect()) {
    Serial.println("connect: comm error, retrying");
    return false;
  }
  writeProgress(0, imageLen);
  if (!stm32_program(image, imageLen, writeProgress)) {
    Serial.println("program: comm error, retrying");
    return false;
  }
  readProgress(0, imageLen);
  int v = stm32_verify(image, imageLen, readProgress);
  if (v < 0) {
    Serial.println("verify: comm error, retrying");
    return false;
  }

  stm32_run();
  v ? passCount++ : failCount++;
  showResult(v ? "PASS" : "FAIL", v ? TFT_DARKGREEN : TFT_RED,
             v ? "done" : "verify mismatch");
  return true;
}

// One verdicted flash: up to 3 flashCycle attempts (each re-attaches) on comm
// errors. If no attempt reaches a verdict, show an orange notice — NOT a red
// FAIL (that's reserved for verify mismatch) — instead of silently going home.
static void flashWithRetry() {
  for (int attempt = 1; attempt <= 3; attempt++) {
    if (flashCycle()) return;
    Serial.printf("flash: comm error, attempt %d/3\n", attempt);
  }
  showResult("comm error", TFT_ORANGE, "no verdict");
}

// Flash, honor retest presses during the result hold, then home.
static void runTests() {
  flashWithRetry();
  while (wantRetest) {
    wantRetest = false;
    flashWithRetry();
  }
  flashed = true;   // whatever the outcome: no retry until the board is reseated
  showHome();
}

// Refresh `detected`; true if a target answered.
static bool probe() {
  strlcpy(detected, stm32_probe(), sizeof(detected));
  return strcmp(detected, "no target") != 0;
}

static void maybeStartNetwork() {
  if (netUp || WiFi.status() != WL_CONNECTED) return;
  netUp = true;
  MDNS.begin(PROJECTNAME);                 // STM32Flasher.local
  ArduinoOTA.setHostname(PROJECTNAME);
  ArduinoOTA.begin();
  server.begin();
  MDNS.addService("http", "tcp", 80);
  Serial.printf("%s @ ", PROJECTNAME);
  Serial.println(WiFi.localIP());
  showHome();                           // refresh the wifi status line
}

void setup() {
  Serial.begin(115200);

  pinMode(MODE_BUTTON, INPUT_PULLUP);
  pinMode(FLASH_BUTTON, INPUT_PULLUP);
  swd_init();

  pinMode(15, OUTPUT);        // T-Display S3: LCD power enable, required on battery
  digitalWrite(15, HIGH);

  tft.init();
  tft.setRotation(3);

  wifiManager.setConfigPortalBlocking(false);
  wifiManager.autoConnect(PROJECTNAME);    // returns immediately; flashing never waits on wifi

  server.on("/", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/upload", HTTP_POST, handleUploadDone, handleUploadChunk);
  server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateChunk);

  FFat.begin(true);                        // format on first boot
  if (!loadImage(IMAGE_PATH))
    Serial.println("no firmware image; upload at http://" PROJECTNAME ".local/");

  showHome();
}

void loop() {
  wifiManager.process();
  maybeStartNetwork();
  if (netUp) {
    ArduinoOTA.handle();
    server.handleClient();
  }

  // Fresh curl upload: flash it now, same as a manual button press. No target
  // connected is not a FAIL (contract) — just log and let the poll carry on.
  if (wantUploadFlash) {
    wantUploadFlash = false;
    if (probe()) runTests();
    else Serial.println("upload flash: no target answered");
    return;
  }

  // Buttons idle high. KEY: press edge, 200 ms lockout. BOOT acts on hold
  // time: a short press (< 1 s) toggles auto/manual on release; held 5 s it
  // forgets WiFi and reboots into the setup AP. (BOOT can't be read at
  // power-up: holding it then puts the ESP32-S3 into ROM download mode.)
  static bool flashPrev = HIGH;
  static uint32_t lastEdge = 0, modeDown = 0;
  bool flashNow = digitalRead(FLASH_BUTTON);
  bool flashPressed = flashNow == LOW && flashPrev == HIGH && millis() - lastEdge >= 200;
  flashPrev = flashNow;
  if (flashPressed) lastEdge = millis();

  if (!digitalRead(MODE_BUTTON)) {
    if (!modeDown) modeDown = millis();
    else if (millis() - modeDown > 5000) {
      Serial.println("wifiManager reset");
      tft.fillScreen(TFT_BLACK);
      tft.setCursor(0, 0, 2);
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.print("wifi forgotten");
      wifiManager.resetSettings();
      delay(1000);
      ESP.restart();
    }
  } else if (modeDown) {
    uint32_t held = millis() - modeDown;
    modeDown = 0;
    if (held >= 50 && held < 1000) {
      autoMode = !autoMode;
      Serial.printf("mode: %s\n", autoMode ? "auto" : "manual");
      showHome();
      return;
    }
  }

  // Manual test: press = probe + flash right now.
  if (!autoMode && flashPressed) {
    waitRelease(FLASH_BUTTON);   // test starts on release, not press
    if (!probe()) {
      Serial.println("manual: no target answered");
      showHome();
      tft.setCursor(0, 3 * 32, 2);
      tft.setTextColor(TFT_RED, TFT_BLACK);
      tft.print("no target, check wiring");
      return;
    }
    if (imageLen) runTests();
    else showHome();
    return;
  }

  // Live detection probe, both modes, once a second. SWD attach is
  // non-intrusive (no reset, the target keeps running). In auto mode a newly
  // inserted target rolls straight into the flash cycle (once per insertion).
  if (millis() - lastPoll < 1000) return;
  lastPoll = millis();

  char prev[sizeof(detected)];
  strcpy(prev, detected);
  if (!probe()) {
    flashed = false;     // next insertion gets flashed
    lastResult[0] = 0;   // verdict was for the board just removed
  } else if (autoMode && imageLen && !flashed) {
    runTests();
    return;
  }
  if (strcmp(detected, prev)) {
    Serial.printf("detected: %s\n", detected);
    showHome();
  }
}
