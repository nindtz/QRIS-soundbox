/*
 * QRIS soundbox - Waveshare ESP32-S3-ePaper-3.97
 * BLE peripheral (NimBLE-Arduino 2.x) + ES8311 voice from SD + e-paper with QRIS
 * generated on the device, TrueType text, battery + Bluetooth icons.
 *
 * FIRST SETUP
 *   With no QRIS stored, the screen shows a QR to SETUP_URL (download the app) and steps.
 *   The app pairs ("Just Works", no passkey) and writes the merchant's QRIS text to the CONFIG characteristic.
 *   The box checks its CRC, stores it in flash (NVS) and switches to the payment screen.
 *   CONFIG characteristic (encrypted). Every write is "<PIN>|<command>", PIN = 4..16 digits:
 *     "<PIN>|<QRIS text>"    set / replace the QRIS          (up to 512 bytes, long write)
 *     "<PIN>|RESET"          forget the QRIS AND the PIN, back to the setup screen
 *                            (orientation is kept)
 *     "<PIN>|ORIENT:P"       payment screen in portrait  (also "ORIENT:PORTRAIT")
 *     "<PIN>|ORIENT:L"       payment screen in landscape (also "ORIENT:LANDSCAPE", default)
 *                            Saved in flash. The setup screen is always landscape.
 *     "<PIN>|PIN:<newPIN>"   change the PIN
 *   The box has no PIN until the first accepted command: the PIN in that command (normally
 *   the "<PIN>|<QRIS>" sent during setup) becomes the box's PIN. After that, a command with
 *   any other PIN is refused, so another phone or app can't reset the box or swap its QRIS.
 *   After PIN_TRIES wrong PINs, all commands are refused for PIN_LOCK_MS (ERR:WAIT).
 *   Forgot the PIN: flash once with FORGET_QRIS_ON_BOOT (clears the QRIS and the PIN).
 *     read / notify  status: "SETUP" | "READY:<crc4>:<L|P>" | "ERR:PIN" | "ERR:WAIT"
 *                            | "ERR:INVALID" | "ERR:LENGTH" | "ERR:BUSY" | "ERR:FLASH"
 *
 * PAYMENTS (PAY characteristic, encrypted)
 *   "<id>;<amount>;<tag>", e.g. "a1b2c3d4;25000;3f9c0e5a7b21d4e8"
 *   tag = first 8 bytes (16 hex) of HMAC-SHA256(key, "<id>;<amount>")
 *   key = SHA-256(stored QRIS + AUTH_SECRET). Wrong tag -> "TIDAK VALID". See qris.h.
 *   The payload is ~31 bytes: the phone should request an MTU of 64+ (Android: requestMtu).
 *
 * SD card layout:
 *   /fonts/ui.ttf          TrueType font for the UI (optional, falls back to bitmap fonts)
 *   /clips/00.wav..21.wav  voice clips, PCM 16-bit mono 24 kHz
 *
 * Libraries (Library Manager): NimBLE-Arduino 2.x, OpenFontRender.
 * Sketch folder: audio_player.h, display_ui.h, qris.h, qris_logo.h, battery.h, qrcodegen.c/.h,
 * es8311.h/.c and the Waveshare EPD driver files (EPD_3IN97.*, GUI_Paint.*,
 * DEV_Config.*, fonts.*, font*.c).
 * Board: ESP32S3 Dev Module, PSRAM = OPI PSRAM, Flash 16 MB, Partition with >= 3 MB app.
 */
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <vector>
#include <esp_task_wdt.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include "FS.h"
#include "SD_MMC.h"

#define SETUP_URL       "https://example.com"       // app download page shown on first setup
#define BLE_DEVICE_NAME "QRIS-Soundbox"
// #define PORTRAIT_ROTATE ROTATE_270               // if portrait comes out upside down

#include "qris.h"
#include "audio_player.h"
#include "display_ui.h"

// Extra secret shared only with the merchant app. The QRIS string itself is public
// (anyone can scan it), so with an empty secret anyone who scans the QR can forge
// a valid tag. Set this, and the same value in the app, for real use.
static const char AUTH_SECRET[] = "";

// ---------- SD (pins from the Waveshare SD example) ----------
#define SD_CLK 16
#define SD_CMD 17
#define SD_D0  15
#define SD_D1  7
#define SD_D2  8
#define SD_D3  18

static bool sdInit() {
  SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0, SD_D1, SD_D2, SD_D3);
  if (!SD_MMC.begin("/sdcard", true)) {          // same call as the example (1-bit mode)
    Serial.println("SD card failed to mount");
    return false;
  }
  Serial.println("SD card mounted");
  return true;
}

// ---------- BLE config ----------
#define SERVICE_UUID "6f1c0001-5b7a-4c1e-9d3a-2a4f7c9b0001"
#define CHAR_UUID    "6f1c0002-5b7a-4c1e-9d3a-2a4f7c9b0001"   // payments
#define CFG_UUID     "6f1c0003-5b7a-4c1e-9d3a-2a4f7c9b0001"   // QRIS setup
#define PAYLOAD_MAX  64
// Pairing is "Just Works" (no passkey): the link is encrypted, but any phone nearby can pair.
// Settings are protected by the app's PIN instead (see CONFIG above); payments by the HMAC
// tag - set AUTH_SECRET above for real use.
#define PIN_MIN      4
#define PIN_MAX      16
#define PIN_TRIES    5                           // wrong PINs in a row before a lockout...
#define PIN_LOCK_MS  (5UL * 60 * 1000)           // ...of 5 minutes (stops guessing the PIN)
// Development aids - un-comment ONCE, flash, then comment out again:
#define CLEAR_BONDS_ON_BOOT                   // pairing stuck (stale bond)
#define FORGET_QRIS_ON_BOOT                   // back to the setup screen (also clears the PIN)

// ---------- stored QRIS + orientation ----------
static Preferences prefs;
static char storedQris[QRIS_MAX_LEN + 1] = "";
static volatile bool qrisValid = false;          // written/read only in the NimBLE host task after setup()
static bool portraitPref = false;                // owned by the main loop after setup()
static volatile int8_t pendingOrient = -1;       // BLE callback -> loop: 0 = landscape, 1 = portrait
static NimBLECharacteristic* cfgChr = nullptr;

// ---------- settings PIN ----------
// devicePin, pinFails and pinLockUntil are only used in the NimBLE host task after setup();
// flash writes go through pinQueue to loop(), like the QRIS.
static char devicePin[PIN_MAX + 1] = "";         // "" = no PIN yet
static QueueHandle_t pinQueue;                   // PIN to save, "" = remove
static uint8_t  pinFails = 0;
static uint32_t pinLockUntil = 0;                // 0 = not locked out

static bool pinFormatOk(const char* p) {
  size_t n = strlen(p);
  if (n < PIN_MIN || n > PIN_MAX) return false;
  for (size_t i = 0; i < n; i++) if (p[i] < '0' || p[i] > '9') return false;
  return true;
}
static bool pinEqual(const char* a, const char* b) {   // constant time
  size_t na = strlen(a), nb = strlen(b);
  uint8_t d = na != nb;
  for (size_t i = 0; i < PIN_MAX; i++) d |= (uint8_t)((i < na ? a[i] : 0) ^ (i < nb ? b[i] : 0));
  return d == 0;
}
static void setDevicePin(const char* p) {        // NimBLE host task only
  char buf[PIN_MAX + 1];
  strlcpy(devicePin, p, sizeof(devicePin));
  strlcpy(buf, p, sizeof(buf));
  xQueueSend(pinQueue, buf, 0);
}

static void setCfgStatus(const char* s, bool notify) {
  if (!cfgChr) return;
  cfgChr->setValue((const uint8_t*)s, strlen(s));
  if (notify) cfgChr->notify();
  Serial.printf("Config status: %s\n", s);
}
static void statusForCurrent(bool notify) {
  char s[16];
  if (qrisValid) snprintf(s, sizeof(s), "READY:%s:%c", storedQris + strlen(storedQris) - 4,
                          portraitPref ? 'P' : 'L');
  else           strcpy(s, "SETUP");
  setCfgStatus(s, notify);
}

// BLE callback -> main loop (NVS write + screen change happen there)
static QueueHandle_t cfgQueue;

// ---------- Payment queue (BLE callback -> main loop) ----------
static QueueHandle_t payQueue;

#define SEEN_N 32
static char seenIds[SEEN_N][16];
static uint8_t seenPos = 0;

static bool alreadySeen(const char* id) {
  for (auto& s : seenIds) if (strcmp(s, id) == 0) return true;
  strlcpy(seenIds[seenPos], id, sizeof(seenIds[0]));
  seenPos = (seenPos + 1) % SEEN_N;
  return false;
}

// ---------- Indonesian number -> clip sequence ----------
// Clip index == file name: /clips/NN.wav
enum Clip : uint8_t {
  C_SATU = 1, C_DUA, C_TIGA, C_EMPAT, C_LIMA, C_ENAM, C_TUJUH, C_DELAPAN, C_SEMBILAN, // 1..9
  C_SEPULUH, C_SEBELAS, C_BELAS, C_PULUH, C_SERATUS, C_RATUS,                         // 10..15
  C_SERIBU, C_RIBU, C_JUTA,                                                           // 16..18
  C_RUPIAH, C_PEMBAYARAN_DITERIMA                                                     // 19..20
};

static void say3(uint32_t n, std::vector<uint8_t>& o) {   // n in 1..999
  uint32_t h = n / 100, r = n % 100;
  if (h == 1)      o.push_back(C_SERATUS);
  else if (h > 1) { o.push_back(h); o.push_back(C_RATUS); }
  if (r == 0) return;
  if (r < 10)        o.push_back(r);
  else if (r == 10)  o.push_back(C_SEPULUH);
  else if (r == 11)  o.push_back(C_SEBELAS);
  else if (r < 20)  { o.push_back(r - 10); o.push_back(C_BELAS); }
  else {
    o.push_back(r / 10); o.push_back(C_PULUH);
    if (r % 10) o.push_back(r % 10);
  }
}

static void sayNumber(uint32_t n, std::vector<uint8_t>& o) {   // supports 1 .. 999,999,999
  uint32_t juta   = (n / 1000000UL) % 1000;
  uint32_t ribu   = (n / 1000UL) % 1000;
  uint32_t sisa   = n % 1000;
  if (juta)   { say3(juta, o);   o.push_back(C_JUTA); }
  if (ribu)   { if (ribu == 1) o.push_back(C_SERIBU); else { say3(ribu, o); o.push_back(C_RIBU); } }
  if (sisa)   say3(sisa, o);
}

// ---------- BLE callbacks ----------
class ServerCb : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*, NimBLEConnInfo& info) override {
    Serial.printf("BLE connected: %s\n", info.getAddress().toString().c_str());
    // No startSecurity() here: the phone starts encryption itself on its first request.
    // Both sides starting it at once made Android 9 stall the link after reconnects.
  }
  // The icon turns "connected" only once the link is encrypted, i.e. writes will be accepted.
  void onAuthenticationComplete(NimBLEConnInfo& info) override {
    Serial.printf("BLE auth done, encrypted=%d\n", info.isEncrypted());
    if (info.isEncrypted()) epdSetBle(true);
  }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int reason) override {
    Serial.printf("BLE disconnected (reason %d)\n", reason);
    epdSetBle(false);
  }
  void onMTUChange(uint16_t mtu, NimBLEConnInfo&) override {
    Serial.printf("BLE MTU %u\n", mtu);
  }
};

class PayCb : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    NimBLEAttValue v = c->getValue();
    char buf[PAYLOAD_MAX + 1];
    size_t n = min((size_t)v.size(), sizeof(buf) - 1);
    memcpy(buf, v.data(), n);
    buf[n] = 0;
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;  // tolerate test tools

    if (!qrisValid) { Serial.printf("Rejected (no QRIS set up yet): %s\n", buf); return; }
    Payment p{};
    switch (parsePayment(buf, p)) {
      case PAY_OK:
        xQueueSend(payQueue, &p, 0);   // keep the callback fast
        break;
      case PAY_BAD_TAG:
        Serial.printf("Rejected (tag does not match this QRIS): %s\n", buf);
        epdShowInvalid();
        break;
      default:
        Serial.printf("Rejected (bad format): %s\n", buf);
        epdShowInvalid();
        break;
    }
  }
};

class CfgCb : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    NimBLEAttValue v = c->getValue();
    if (v.size() > QRIS_MAX_LEN) { setCfgStatus("ERR:LENGTH", true); return; }
    static CfgMsg m;                                 // 514 bytes: keep it off the BLE task stack
    memset(&m, 0, sizeof(m));
    size_t n = v.size();
    memcpy(m.qris, v.data(), n);
    while (n && (m.qris[n - 1] == '\n' || m.qris[n - 1] == '\r' || m.qris[n - 1] == ' ')) m.qris[--n] = 0;

    // ---- PIN check: "<PIN>|<command>" ----
    if (pinLockUntil) {
      if ((int32_t)(pinLockUntil - millis()) > 0) { setCfgStatus("ERR:WAIT", true); return; }
      pinLockUntil = 0;
    }
    char* bar = strchr(m.qris, '|');
    if (!bar) { Serial.println("Config: no PIN given"); setCfgStatus("ERR:PIN", true); return; }
    *bar = 0;
    if (!pinFormatOk(m.qris)) { setCfgStatus("ERR:PIN", true); return; }
    char pin[PIN_MAX + 1];
    strlcpy(pin, m.qris, sizeof(pin));
    memmove(m.qris, bar + 1, strlen(bar + 1) + 1);  // m.qris now holds just the command
    const bool fresh = devicePin[0] == 0;           // no PIN yet: this one will be adopted
    if (!fresh && !pinEqual(pin, devicePin)) {
      Serial.println("Config: wrong PIN");
      if (++pinFails >= PIN_TRIES) {
        pinFails = 0;
        pinLockUntil = millis() + PIN_LOCK_MS;
        if (!pinLockUntil) pinLockUntil = 1;
        setCfgStatus("ERR:WAIT", true);
      } else {
        setCfgStatus("ERR:PIN", true);
      }
      return;
    }
    pinFails = 0;

    // Change PIN
    if (strncmp(m.qris, "PIN:", 4) == 0) {
      if (!pinFormatOk(m.qris + 4)) { setCfgStatus("ERR:INVALID", true); return; }
      setDevicePin(m.qris + 4);                     // saved + status sent from loop()
      return;
    }

    // Orientation command: the flash write + redraw happen in loop()
    if (strncmp(m.qris, "ORIENT:", 7) == 0) {
      const char* o = m.qris + 7;
      if      (!strcmp(o, "P") || !strcmp(o, "PORTRAIT"))  pendingOrient = 1;
      else if (!strcmp(o, "L") || !strcmp(o, "LANDSCAPE")) pendingOrient = 0;
      else { setCfgStatus("ERR:INVALID", true); return; }
      if (fresh) setDevicePin(pin);
      return;
    }

    if (strcmp(m.qris, "RESET") == 0) {
      m.reset = true;
    } else if (!qrisCheck(m.qris)) {
      Serial.printf("Config: rejected QRIS (bad TLV/CRC): %s\n", m.qris);
      setCfgStatus("ERR:INVALID", true);
      return;
    }
    if (xQueueSend(cfgQueue, &m, 0) != pdTRUE) { setCfgStatus("ERR:BUSY", true); return; }
    // Switch the payment check over right here, in the same task that verifies payments.
    if (m.reset) { qrisValid = false; setDevicePin(""); }   // RESET also forgets the PIN
    else {
      authInit(m.qris, AUTH_SECRET);
      qrisValid = true;
      if (fresh) setDevicePin(pin);                 // first setup: this PIN becomes the box's PIN
    }
  }
};

// ---------- setup / loop ----------
static void loadQris() {
  prefs.begin("soundbox", false);
#ifdef FORGET_QRIS_ON_BOOT
  prefs.remove("qris");
  prefs.remove("pin");
  Serial.println("Stored QRIS and PIN forgotten");
#endif
  prefs.getString("pin", devicePin, sizeof(devicePin));
  if (!pinFormatOk(devicePin)) devicePin[0] = 0;
  Serial.printf("Settings PIN: %s\n", devicePin[0] ? "set" : "none yet (next command sets it)");
  portraitPref = prefs.getUChar("orient", 0) == 1;
  size_t n = prefs.getString("qris", storedQris, sizeof(storedQris));
  if (n && qrisCheck(storedQris)) {
    authInit(storedQris, AUTH_SECRET);
    qrisValid = true;
    Serial.printf("QRIS loaded from flash (%u chars), %s\n", (unsigned)strlen(storedQris),
                  portraitPref ? "portrait" : "landscape");
  } else {
    storedQris[0] = 0;
    qrisValid = false;
    Serial.println("No QRIS stored: showing the setup screen");
  }
}

// ---------- watchdog + health ----------
// If the main loop or the display task stops for WDT_TIMEOUT_S, the box reboots by itself
// (about 5 s, back to the payment screen) instead of staying frozen.
#define WDT_TIMEOUT_S   30
#define HEALTH_EVERY_MS 60000UL
#define HEAP_RESTART_KB 12            // reboot before the internal heap runs out completely

static void wdtSetup() {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t cfg;
  cfg.timeout_ms = WDT_TIMEOUT_S * 1000;
  cfg.idle_core_mask = 0;             // watch our tasks only
  cfg.trigger_panic = true;           // panic = reboot
  if (esp_task_wdt_reconfigure(&cfg) != ESP_OK) esp_task_wdt_init(&cfg);
#else
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
#endif
}

static const char* resetReason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "power on";
    case ESP_RST_SW:       return "software restart";
    case ESP_RST_PANIC:    return "CRASH (panic)";
    case ESP_RST_INT_WDT:  return "HANG (interrupt watchdog)";
    case ESP_RST_TASK_WDT: return "HANG (task watchdog)";
    case ESP_RST_WDT:      return "HANG (other watchdog)";
    case ESP_RST_BROWNOUT: return "BROWNOUT (supply voltage dipped)";
    case ESP_RST_DEEPSLEEP:return "deep sleep";
    default:               return "other";
  }
}

// One line a minute: if "internal heap" keeps going down, something leaks.
static void healthCheck() {
  static uint32_t next = 0;
  if ((int32_t)(millis() - next) < 0) return;
  next = millis() + HEALTH_EVERY_MS;
  size_t freeInt = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  NimBLEServer* srv = NimBLEDevice::getServer();
  Serial.printf("[health] up %lu min | internal heap %u B free (lowest %u, largest block %u) | "
                "PSRAM %u B free | BLE clients %u\n",
                (unsigned long)(millis() / 60000), (unsigned)freeInt,
                (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                (unsigned)ESP.getFreePsram(), srv ? (unsigned)srv->getConnectedCount() : 0u);
  if (freeInt < HEAP_RESTART_KB * 1024) {
    Serial.println("[health] internal heap almost gone: restarting");
    Serial.flush();
    ESP.restart();
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.printf("\n=== boot, last reset: %s ===\n", resetReason());
  wdtSetup();
  payQueue = xQueueCreate(8, sizeof(Payment));
  cfgQueue = xQueueCreate(2, sizeof(CfgMsg));
  pinQueue = xQueueCreate(4, PIN_MAX + 1);

  loadQris();
  bool sdOk = sdInit();
  audioInit();                                     // starts I2C, which the battery reader shares
  if (sdOk) audioLoadClips();
  epdUiInit(sdOk, qrisValid ? storedQris : nullptr, portraitPref);   // first screen (~3 s)
  epdUiStart();                                    // background refresh task

  NimBLEDevice::init(BLE_DEVICE_NAME);
#ifdef CLEAR_BONDS_ON_BOOT
  NimBLEDevice::deleteAllBonds();
  Serial.println("All stored BLE bonds deleted");
#endif
  NimBLEDevice::setMTU(247);                               // fewer chunks for the QRIS write
  NimBLEDevice::setSecurityAuth(true, false, true);         // bonding, no MITM, secure connections
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT); // "Just Works": no passkey to type

  NimBLEServer* server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCb());
  server->advertiseOnDisconnect(true);

  NimBLEService* svc = server->createService(SERVICE_UUID);
  NimBLECharacteristic* chr = svc->createCharacteristic(
      CHAR_UUID,
      NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC,   // encrypted; AUTHEN needs a passkey
      PAYLOAD_MAX);
  chr->setCallbacks(new PayCb());

  cfgChr = svc->createCharacteristic(
      CFG_UUID,
      NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC |
      NIMBLE_PROPERTY::READ  | NIMBLE_PROPERTY::READ_ENC  |
      NIMBLE_PROPERTY::NOTIFY,
      QRIS_MAX_LEN);
  cfgChr->setCallbacks(new CfgCb());
  statusForCurrent(false);
  svc->start();

  // The 128-bit service UUID fills most of the 31-byte advertisement, so the name goes in
  // the scan response; otherwise phone scanners (nRF Connect, iOS) show the box as "N/A".
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  NimBLEAdvertisementData advData, scanData;
  advData.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  advData.addServiceUUID(NimBLEUUID(SERVICE_UUID));
  scanData.setName(BLE_DEVICE_NAME);
  adv->setAdvertisementData(advData);
  adv->setScanResponseData(scanData);
  adv->enableScanResponse(true);
  adv->start();

  Serial.printf("Ready (%s). Pairing: Just Works (no passkey)\n", qrisValid ? "payments" : "setup");
  enableLoopWDT();                                 // loop() is fed after every pass
}

static void handleConfig(const CfgMsg& m) {
  if (m.reset) {
    prefs.remove("qris");
    storedQris[0] = 0;
    epdSetQris(nullptr);                           // back to the setup screen
  } else {
    if (prefs.putString("qris", m.qris) == 0) { setCfgStatus("ERR:FLASH", true); return; }
    strlcpy(storedQris, m.qris, sizeof(storedQris));
    memset(seenIds, 0, sizeof(seenIds));           // new QR, new history
    epdSetQris(storedQris);
    Serial.println("QRIS saved");
  }
  statusForCurrent(true);
}

static void handleOrient(bool portrait) {
  if (portrait != portraitPref) {
    if (prefs.putUChar("orient", portrait ? 1 : 0) == 0) { setCfgStatus("ERR:FLASH", true); return; }
    portraitPref = portrait;
    epdSetPortrait(portrait);                      // full refresh if on the payment screen
    Serial.printf("Orientation: %s\n", portrait ? "portrait" : "landscape");
  }
  statusForCurrent(true);
}

static void handlePin(const char* pin) {
  if (pin[0]) {
    if (prefs.putString("pin", pin) == 0) { setCfgStatus("ERR:FLASH", true); return; }
    Serial.println("Settings PIN saved");
  } else {
    prefs.remove("pin");
    Serial.println("Settings PIN cleared");
  }
  statusForCurrent(true);
}

void loop() {
  healthCheck();
  static CfgMsg m;
  if (xQueueReceive(cfgQueue, &m, 0) == pdTRUE) handleConfig(m);

  char pinBuf[PIN_MAX + 1];
  if (xQueueReceive(pinQueue, pinBuf, 0) == pdTRUE) handlePin(pinBuf);

  int8_t o = pendingOrient;
  if (o >= 0) { pendingOrient = -1; handleOrient(o == 1); }

  Payment p;
  if (xQueueReceive(payQueue, &p, pdMS_TO_TICKS(50)) == pdTRUE) {
    if (alreadySeen(p.id)) { Serial.printf("Duplicate %s ignored\n", p.id); return; }
    Serial.printf("Payment %s: Rp %lu\n", p.id, (unsigned long)p.amount);

    std::vector<uint8_t> clips;
    clips.push_back(C_PEMBAYARAN_DITERIMA);
    sayNumber(p.amount, clips);
    clips.push_back(C_RUPIAH);

    epdShow(p.amount);         // non-blocking: e-paper refreshes in its own task...
    audioPlayClips(clips);     // ...while the voice plays here
  }
}
