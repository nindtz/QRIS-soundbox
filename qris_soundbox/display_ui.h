/*
 * display_ui.h - 800x480 e-paper UI for the QRIS soundbox
 *
 *  SETUP mode (no QRIS stored yet) - always LANDSCAPE:
 *    left : QR to SETUP_URL ("download the app") + caption
 *    right: QRIS logo + 3 setup steps
 *  READY mode, LANDSCAPE (default):
 *    left : merchant QRIS generated on the device (qrcodegen.c/.h)
 *    right: QRIS logo, merchant name/city, status (idle / amount / invalid), history
 *    Bluetooth + battery icons in the top-right corner.
 *  READY mode, PORTRAIT (480x800, selected from the app with "ORIENT:P"):
 *    top bar : amount just paid (top-left, cleared after PORTRAIT_AMOUNT_MS) |
 *              Bluetooth + battery (top-right), ruled lines
 *    body    : QRIS logo, store name (no city), QR code
 *
 * Libraries:  OpenFontRender (takkaO) - Arduino Library Manager: "OpenFontRender"
 * Font:       a bold sans TTF (e.g. Inter Bold, Noto Sans Bold) reads best. Looked up in order:
 *             1. binaryttf.h in the sketch folder (made with OpenFontRender's ttf2bin.py)
 *             2. /fonts/ui.ttf on the SD card
 *             3. the Waveshare bitmap fonts
 *
 * Public API (all non-blocking except epdUiInit, safe to call from BLE callbacks):
 *   epdUiInit(sdOk, qrisOrNull, portrait)  draw the first screen (blocking, ~3 s)
 *   epdUiStart()                  start the background refresh task
 *   epdSetQris(qrisOrNull)        switch to READY with this QRIS (nullptr -> back to SETUP)
 *   epdSetPortrait(on)            READY screen orientation (SETUP stays landscape)
 *   epdShow(amount) / epdShowInvalid() / epdSetBle(on)
 */
#pragma once
#include <Arduino.h>
#include "FS.h"
#include "SD_MMC.h"
#include <OpenFontRender.h>
#include "qrcodegen.h"
#include "qris.h"
#include "qris_logo.h"
#include "battery.h"
#include "DEV_Config.h"
#include "EPD_3IN97.h"
#include "GUI_Paint.h"
#include "fonts.h"
#include <esp_task_wdt.h>

#ifndef SETUP_URL
#define SETUP_URL        "https://example.com"      // where the merchant downloads the app
#endif
#ifndef BLE_DEVICE_NAME
#define BLE_DEVICE_NAME  "QRIS-Soundbox"
#endif
// Which way portrait turns. If the portrait screen comes out upside down for the way
// the box is mounted, change this to ROTATE_270.
#ifndef PORTRAIT_ROTATE
#define PORTRAIT_ROTATE  ROTATE_270
#endif
#define FONT_PATH        "/fonts/ui.ttf"
#define INVALID_SHOW_MS  8000    // "invalid" banner time before returning to previous status
#define PORTRAIT_AMOUNT_MS 10000 // portrait (customer-facing): amount disappears after this
#define USE_PARTIAL      1       // 1 = partial refresh (no flash), 0 = old fast refresh (flashes)
#define FULL_EVERY       100     // full refresh after this many partial/fast ones (clears ghosting)
#define BLE_DEBOUNCE_MS  2000    // a flapping BLE link doesn't cause a burst of refreshes
#define BAT_POLL_MS      60000   // battery is read every minute...
#define BAT_STEP         10      // ...but the screen only refreshes when it crosses a 10% step

static_assert(EPD_3IN97_WIDTH > EPD_3IN97_HEIGHT, "layout expects a landscape 800x480 panel");
static const int SCR_W   = EPD_3IN97_WIDTH;          // 800 (physical, landscape)
static const int SCR_H   = EPD_3IN97_HEIGHT;         // 480
static const int QR_W    = SCR_H;                    // square QR area on the left
static const int PANEL_X = QR_W;                     // 480
static const int PANEL_W = SCR_W - QR_W;             // 320

// Portrait layout (logical coordinates after rotation)
static const int P_W     = SCR_H;                    // 480
static const int P_H     = SCR_W;                    // 800
static const int BAR_H   = 76;                       // top bar height, bottom rule included
static const int BAR_SPLIT = P_W - 132;              // vertical rule: payment | icons

// ====================== PANEL PORT ======================
// Driver calls taken from the previous display_ui.h (known to work on this panel).
static void panelInit() {
  DEV_Module_Init();
  EPD_3IN97_Init();
  EPD_3IN97_Clear();
}
static void panelFull(UBYTE* img) {                  // slow, flashes, clears ghosting
  EPD_3IN97_Init();
  EPD_3IN97_Display(img);
  EPD_3IN97_Sleep();                                 // ~0 power until the next Init
}
static void panelFast(UBYTE* img) {                  // fast refresh of the whole frame (flashes)
  EPD_3IN97_Init_Fast();
  EPD_3IN97_Display_Fast(img);
  EPD_3IN97_Sleep();
}

// ---- Partial refresh: only pixels that changed are driven, no flashing ----
// Implemented here because the Waveshare driver keeps its SPI helpers static and its
// EPD_3IN97_Init_Partial() is declared but never defined. The whole frame goes to the
// "new" RAM (0x24) and the previously shown frame to the "old" RAM (0x26); the partial
// waveform (0x22 = 0xFF, same value as the driver's TurnOnDisplay_Part) then only moves
// the pixels that differ. Sending both frames every time means nothing depends on the
// panel keeping its RAM through deep sleep, and it works the same in both orientations.
static void epdCmd(UBYTE c) {
  DEV_Digital_Write(EPD_DC_PIN, 0);
  DEV_Digital_Write(EPD_CS_PIN, 0);
  DEV_SPI_WriteByte(c);
  DEV_Digital_Write(EPD_CS_PIN, 1);
}
static void epdData(UBYTE d) {
  DEV_Digital_Write(EPD_DC_PIN, 1);
  DEV_Digital_Write(EPD_CS_PIN, 0);
  DEV_SPI_WriteByte(d);
  DEV_Digital_Write(EPD_CS_PIN, 1);
}
static void epdBlock(const UBYTE* p, size_t n) {
  DEV_Digital_Write(EPD_DC_PIN, 1);
  for (size_t i = 0; i < n; i++) {
    DEV_Digital_Write(EPD_CS_PIN, 0);
    DEV_SPI_WriteByte(p[i]);
    DEV_Digital_Write(EPD_CS_PIN, 1);
    if ((i & 2047) == 2047) vTaskDelay(1);           // let Bluetooth + idle run (core 0)
  }
}
static bool epdTrouble = false;                      // a busy timeout happened: next refresh is full
static void epdBusy() {
  DEV_Delay_ms(20);
  uint32_t t0 = millis();
  while (DEV_Digital_Read(EPD_BUSY_PIN) == 1) {      // HIGH = busy
    if (millis() - t0 > 10000) {
      Serial.println("EPD: panel stayed busy for 10 s, will do a full refresh next");
      epdTrouble = true;
      return;
    }
    DEV_Delay_ms(5);
  }
}
// Same register setup as EPD_3IN97_Init(), except the border follows VCOM (0x80, as in
// the driver's Display_Partial) so the frame around the image doesn't flicker.
static void panelPartialInit() {
  DEV_Digital_Write(EPD_RST_PIN, 1); DEV_Delay_ms(20);
  DEV_Digital_Write(EPD_RST_PIN, 0); DEV_Delay_ms(2);
  DEV_Digital_Write(EPD_RST_PIN, 1); DEV_Delay_ms(20);
  epdBusy();
  epdCmd(0x12);                                      // SWRESET
  epdBusy();
  epdCmd(0x18); epdData(0x80);                       // internal temperature sensor
  epdCmd(0x0C); epdData(0xAE); epdData(0xC7); epdData(0xC3); epdData(0xC0); epdData(0x80);
  epdCmd(0x01);                                      // driver output control
  epdData((EPD_3IN97_HEIGHT - 1) % 256); epdData((EPD_3IN97_HEIGHT - 1) / 256); epdData(0x02);
  epdCmd(0x3C); epdData(0x80);                       // border waveform: follow VCOM
  epdCmd(0x11); epdData(0x01);                       // data entry mode
  epdCmd(0x44);                                      // RAM X start/end
  epdData(0x00); epdData(0x00);
  epdData((EPD_3IN97_WIDTH - 1) % 256); epdData((EPD_3IN97_WIDTH - 1) / 256);
  epdCmd(0x45);                                      // RAM Y start/end
  epdData((EPD_3IN97_HEIGHT - 1) % 256); epdData((EPD_3IN97_HEIGHT - 1) / 256);
  epdData(0x00); epdData(0x00);
  epdCmd(0x4E); epdData(0x00); epdData(0x00);        // RAM X counter
  epdCmd(0x4F); epdData(0x00); epdData(0x00);        // RAM Y counter
  epdBusy();
}
static void panelPartial(const UBYTE* img, const UBYTE* shown) {
  const size_t n = (EPD_3IN97_WIDTH / 8) * EPD_3IN97_HEIGHT;
  panelPartialInit();
  epdCmd(0x24); epdBlock(img, n);                    // new image   (same order as Display_Base)
  epdCmd(0x26); epdBlock(shown, n);                  // what is on the panel now
  epdCmd(0x22); epdData(0xFF);                       // partial update waveform
  epdCmd(0x20);
  epdBusy();
  EPD_3IN97_Sleep();
}
// ========================================================

static UBYTE* fullBuf = nullptr;                     // SCR_W x SCR_H, 1 bpp
static UBYTE* shownBuf = nullptr;                    // copy of what the panel shows now
static int canvasW = 0, canvasH = 0;                 // logical size of the current canvas

// The frame buffer always has the panel's native 800x480 layout; in portrait,
// GUI_Paint rotates every pixel, so all drawing below works in 480x800 coordinates.
static void selectCanvas(bool portrait) {
  Paint_NewImage(fullBuf, SCR_W, SCR_H, portrait ? PORTRAIT_ROTATE : ROTATE_0, WHITE);
  Paint_SelectImage(fullBuf);
  canvasW = portrait ? P_W : SCR_W;
  canvasH = portrait ? P_H : SCR_H;
}
static inline void px(int x, int y, bool black) {
  if (x < 0 || y < 0 || x >= canvasW || y >= canvasH) return;
  Paint_SetPixel(x, y, black ? BLACK : WHITE);
}
static void fillRect(int x, int y, int w, int h, bool black) {
  for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) px(i, j, black);
}
static void line(int x1, int y1, int x2, int y2) {
  Paint_DrawLine(x1, y1, x2, y2, BLACK, DOT_PIXEL_2X2, LINE_STYLE_SOLID);
}
// 1 bpp, MSB first, 1 = black (format written by tools/svg2bits.py)
static void drawBitmap(int x, int y, const uint8_t* bits, int w, int h) {
  const int bpr = (w + 7) / 8;
  for (int j = 0; j < h; j++)
    for (int i = 0; i < w; i++)
      if (bits[j * bpr + i / 8] & (0x80 >> (i % 8))) px(x + i, y + j, true);
}

// ---------- TrueType text ----------
static OpenFontRender ofr;
static uint8_t* fontData = nullptr;
static bool ttfOk = false;

// OpenFontRender anti-aliases between fg and bg; threshold to 1 bpp on the green channel.
static void ofrPixel(int32_t x, int32_t y, uint16_t c) {
  px(x, y, ((c >> 5) & 0x3F) < 32);
}

// Font compiled into the firmware: run OpenFontRender's ttf2bin.py on your .ttf and put the
// generated binaryttf.h in the sketch folder. Without it, the font is read from the SD card.
#if __has_include("binaryttf.h")
#include "binaryttf.h"
#define HAVE_EMBEDDED_TTF 1
#endif

static bool loadEmbeddedTtf() {
#ifdef HAVE_EMBEDDED_TTF
  ofr.setDrawPixel(ofrPixel);
  if (ofr.loadFont(binaryttf, sizeof(binaryttf))) {  // read straight from flash, no RAM copy
    Serial.println("TTF: FreeType could not load the built-in font");
    return false;
  }
  Serial.printf("TTF: built-in font, %u bytes\n", (unsigned)sizeof(binaryttf));
  return true;
#else
  return false;
#endif
}

static bool loadTtf() {
  File f = SD_MMC.open(FONT_PATH, FILE_READ);
  if (!f) { Serial.println("TTF: " FONT_PATH " not found, using bitmap fonts"); return false; }
  size_t n = f.size();
  fontData = (uint8_t*)ps_malloc(n);                 // N16R8 module: plenty of PSRAM
  if (!fontData) { f.close(); Serial.println("TTF: out of memory"); return false; }
  size_t got = f.read(fontData, n);
  f.close();
  if (got != n) { free(fontData); fontData = nullptr; return false; }

  ofr.setDrawPixel(ofrPixel);
  if (ofr.loadFont(fontData, n)) {                   // FT_Error, 0 = ok
    Serial.println("TTF: FreeType could not load the font");
    free(fontData); fontData = nullptr;
    return false;
  }
  Serial.printf("TTF: loaded %u bytes\n", (unsigned)n);
  return true;
}

enum TAlign { T_LEFT, T_CENTER, T_RIGHT };

static sFONT* bitmapFor(int sizePx) {
  return sizePx >= 34 ? &Font24 : sizePx >= 22 ? &Font20 : &Font16;
}

static int textWidth(const char* s, int sizePx) {
  if (ttfOk) { ofr.setFontSize(sizePx); return (int)ofr.getTextWidth("%s", s); }
  return (int)strlen(s) * bitmapFor(sizePx)->Width;
}

// Draws s with its top edge at y; x is the left/center/right anchor. white=true for text on black.
static void drawText(const char* s, int x, int y, int sizePx, TAlign a, bool white = false) {
  if (ttfOk) {
    ofr.setFontSize(sizePx);
    ofr.setAlignment(a == T_LEFT ? Align::TopLeft : a == T_CENTER ? Align::TopCenter : Align::TopRight);
    uint16_t fg = white ? 0xFFFF : 0x0000, bg = white ? 0x0000 : 0xFFFF;
    ofr.drawString(s, x, y, fg, bg);
    return;
  }
  sFONT* f = bitmapFor(sizePx);
  int w = (int)strlen(s) * f->Width;
  int x0 = a == T_LEFT ? x : a == T_CENTER ? x - w / 2 : x - w;
  if (x0 < 0) x0 = 0;
  Paint_DrawString_EN(x0, y, s, f, white ? WHITE : BLACK, white ? BLACK : WHITE);
}

// Largest size <= maxPx that fits in maxW
static int fitSize(const char* s, int maxPx, int minPx, int maxW) {
  int sz = maxPx;
  while (sz > minPx && textWidth(s, sz) > maxW) sz -= 2;
  return sz;
}

// "Rp 1.250.000"
static void formatRupiah(uint32_t v, char* out, size_t n) {
  char d[12];
  snprintf(d, sizeof(d), "%lu", (unsigned long)v);
  int len = strlen(d), o = snprintf(out, n, "Rp ");
  for (int i = 0; i < len && o < (int)n - 1; i++) {
    out[o++] = d[i];
    int left = len - 1 - i;
    if (left > 0 && left % 3 == 0 && o < (int)n - 1) out[o++] = '.';
  }
  out[o] = 0;
}

// ---------- icons ----------
// Bluetooth rune, ~28 px tall, centred on (cx, cy). Slashed when not connected.
static void drawBleIcon(int cx, int cy, bool on) {
  static const int8_t pts[6][2] = {{-7, -7}, {7, 6}, {0, 13}, {0, -13}, {7, -6}, {-7, 7}};
  for (int i = 0; i < 5; i++) line(cx + pts[i][0], cy + pts[i][1], cx + pts[i + 1][0], cy + pts[i + 1][1]);
  if (!on) line(cx - 12, cy + 14, cx + 12, cy - 14);
}

// Battery 40x20 body + nub, top-left at (x, y); 4 segments; bolt to the left when charging.
static void drawBattery(int x, int y, const BatteryInfo& b) {
  const int w = 40, h = 20;
  Paint_DrawRectangle(x, y, x + w, y + h, BLACK, DOT_PIXEL_2X2, DRAW_FILL_EMPTY);
  fillRect(x + w + 1, y + 6, 4, h - 11, true);       // nub
  int segs = (b.percent + 12) / 25;                  // 0..4, rounded
  if (segs > 4) segs = 4;
  for (int i = 0; i < segs; i++) fillRect(x + 4 + i * 9, y + 4, 7, h - 7, true);
  if (b.charging) {                                  // lightning bolt left of the battery
    int bx = x - 14;
    line(bx + 4, y - 1, bx - 2, y + 11);
    line(bx - 2, y + 11, bx + 5, y + 10);
    line(bx + 5, y + 10, bx - 1, y + 22);
  }
}

// ---------- state ----------
enum UiMode   : uint8_t { MODE_SETUP, MODE_READY };
enum UiStatus : uint8_t { ST_IDLE, ST_PAID, ST_INVALID };
static struct {
  UiMode   mode = MODE_SETUP;
  bool     portrait = false;                         // preference; only used in READY mode
  bool     sdOk = false, ble = false, shownBle = false;
  UiStatus status = ST_IDLE, before = ST_IDLE;       // `before` = status to restore after INVALID
  uint32_t amount = 0;
  uint32_t hist[3] = {0, 0, 0};                      // previous amounts, newest first
  uint32_t invalidUntil = 0;
  uint32_t amountUntil = 0;                          // portrait: when the amount is cleared
  bool     amountTimer = false;                      // portrait: amount still showing
  BatteryInfo bat{false, false, 0};
  char     merchant[40] = "", city[24] = "";
  char     qris[QRIS_MAX_LEN + 1] = "";              // owned by the UI task
} ui;

// The setup screen is always landscape.
static inline bool isPortrait() { return ui.portrait && ui.mode == MODE_READY; }

// [bt] [bolt][battery], battery body top-left at (batX, batY)
static void drawStatusIcons(int batX, int batY) {
  if (ui.bat.present) drawBattery(batX, batY, ui.bat);
  drawBleIcon(batX - (ui.bat.present ? (ui.bat.charging ? 40 : 26) : -30), batY + 10, ui.ble);
}

// epdSetQris() hands a new string to the UI task through this buffer
static char pendingQris[QRIS_MAX_LEN + 1];
static SemaphoreHandle_t pendingLock = nullptr;

static void applyQris(const char* q) {
  ui.amount = 0;
  memset(ui.hist, 0, sizeof(ui.hist));
  ui.status = ui.before = ST_IDLE;
  if (q && q[0] && qrisCheck(q)) {
    strlcpy(ui.qris, q, sizeof(ui.qris));
    qrisGetField(q, "59", ui.merchant, sizeof(ui.merchant));
    qrisGetField(q, "60", ui.city, sizeof(ui.city));
    ui.mode = MODE_READY;
  } else {
    ui.qris[0] = ui.merchant[0] = ui.city[0] = 0;
    ui.mode = MODE_SETUP;
  }
}

// ---------- QR ----------
// Draws `text` as a QR centred in the box (x0,y0,w,h). maxScale keeps short URLs from getting huge.
static bool drawQr(const char* text, int x0, int y0, int w, int h, int maxScale) {
  static uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(20)];
  static uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(20)];
  if (!qrcodegen_encodeText(text, tmp, qr, qrcodegen_Ecc_MEDIUM, 1, 20, qrcodegen_Mask_AUTO, true))
    return false;
  int n = qrcodegen_getSize(qr);
  int box = (w < h ? w : h) - 16;
  int scale = box / (n + 8);                         // keep a 4-module quiet zone each side
  if (scale > maxScale) scale = maxScale;
  if (scale < 1) return false;
  int ox = x0 + (w - n * scale) / 2, oy = y0 + (h - n * scale) / 2;
  for (int y = 0; y < n; y++)
    for (int x = 0; x < n; x++)
      if (qrcodegen_getModule(qr, x, y)) fillRect(ox + x * scale, oy + y * scale, scale, scale, true);
  Serial.printf("QR: version %d, %dx%d modules, %d px/module\n", (n - 17) / 4, n, n, scale);
  return true;
}

// ====================== LANDSCAPE ======================
static void drawLeft() {
  fillRect(0, 0, QR_W, SCR_H, false);
  if (ui.mode == MODE_SETUP) {
    drawQr(SETUP_URL, 0, 0, QR_W, 390, 10);
    drawText("Scan untuk unduh aplikasi", QR_W / 2, 396, 26, T_CENTER);
    drawText(SETUP_URL, QR_W / 2, 434, fitSize(SETUP_URL, 20, 14, QR_W - 40), T_CENTER);
    return;
  }
  if (!drawQr(ui.qris, 0, 0, QR_W, SCR_H, 12)) {
    drawText("QRIS", QR_W / 2, SCR_H / 2 - 50, 48, T_CENTER);
    drawText("tidak valid", QR_W / 2, SCR_H / 2 + 10, 32, T_CENTER);
  }
}

static void drawPanel() {
  const int ox = PANEL_X, cx = ox + PANEL_W / 2, inner = PANEL_W - 32;
  fillRect(ox, 0, PANEL_W, SCR_H, false);
  fillRect(ox, 0, 2, SCR_H, true);                   // divider next to the QR

  drawStatusIcons(ox + PANEL_W - 16 - 46, 12);

  drawBitmap(ox + (PANEL_W - QRIS_LOGO_W) / 2, 46, QRIS_LOGO, QRIS_LOGO_W, QRIS_LOGO_H);

  if (ui.mode == MODE_SETUP) {
    drawText("Pengaturan awal", cx, 138, 28, T_CENTER);
    fillRect(ox + 24, 182, PANEL_W - 48, 2, true);
    static const char* steps[][2] = {
      {"1. Scan QR di kiri", "untuk unduh aplikasi"},
      {"2. Di aplikasi, pilih", BLE_DEVICE_NAME},
      {"3. Kirim kode QRIS", "toko Anda"},
    };
    for (int i = 0; i < 3; i++) {
      drawText(steps[i][0], ox + 24, 204 + i * 72, fitSize(steps[i][0], 22, 14, PANEL_W - 40), T_LEFT);
      drawText(steps[i][1], ox + 46, 232 + i * 72, fitSize(steps[i][1], 22, 14, PANEL_W - 62), T_LEFT);
    }
    if (!ui.sdOk) drawText("SD error", ox + PANEL_W - 16, SCR_H - 30, 18, T_RIGHT);
    return;
  }

  if (ui.merchant[0]) drawText(ui.merchant, cx, 134, fitSize(ui.merchant, 30, 16, inner), T_CENTER);
  if (ui.city[0])     drawText(ui.city, cx, 172, 22, T_CENTER);
  fillRect(ox + 24, 208, PANEL_W - 48, 2, true);

  char amt[24];
  switch (ui.status) {
    case ST_IDLE:
      drawText("Silakan scan", cx, 256, 28, T_CENTER);
      drawText("untuk membayar", cx, 294, 28, T_CENTER);
      break;
    case ST_PAID:
      formatRupiah(ui.amount, amt, sizeof(amt));
      drawText("Pembayaran diterima", cx, 226, fitSize("Pembayaran diterima", 26, 16, inner), T_CENTER);
      drawText(amt, cx, 262, fitSize(amt, 56, 22, inner), T_CENTER);
      if (ui.hist[0]) {
        drawText("Sebelumnya", ox + 24, 344, 18, T_LEFT);
        for (int i = 0; i < 3 && ui.hist[i]; i++) {
          formatRupiah(ui.hist[i], amt, sizeof(amt));
          drawText(amt, ox + 24, 370 + i * 28, 22, T_LEFT);
        }
      }
      break;
    case ST_INVALID:
      fillRect(ox + 16, 232, PANEL_W - 32, 130, true);
      drawText("TRANSAKSI", cx, 250, 34, T_CENTER, true);
      drawText("TIDAK VALID", cx, 300, 34, T_CENTER, true);
      break;
  }
  if (!ui.sdOk) drawText("SD error", ox + PANEL_W - 16, SCR_H - 30, 18, T_RIGHT);
}

// ====================== PORTRAIT ======================
// Top bar: last payment on the left, icons on the right, a vertical rule between them
// and a horizontal rule underneath. This is the only part redrawn on a payment.
static void drawPortraitBar() {
  const int ruleY = BAR_H - 2;
  fillRect(0, 0, P_W, BAR_H, false);

  if (ui.status == ST_INVALID) {
    fillRect(0, 0, BAR_SPLIT, ruleY, true);
    const char* s = "TIDAK VALID";
    int sz = fitSize(s, 34, 16, BAR_SPLIT - 24);
    drawText(s, BAR_SPLIT / 2, (ruleY - sz) / 2, sz, T_CENTER, true);
  } else if (ui.status == ST_PAID && ui.amount && ui.amountTimer) {   // amount only, for a few seconds
    char amt[24];
    formatRupiah(ui.amount, amt, sizeof(amt));
    int sz = fitSize(amt, 40, 18, BAR_SPLIT - 32);
    drawText(amt, 16, (ruleY - sz) / 2, sz, T_LEFT);
  }

  fillRect(BAR_SPLIT, 0, 2, ruleY, true);            // vertical rule: payment | icons
  fillRect(0, ruleY, P_W, 2, true);                  // rule under the bar
  drawStatusIcons(P_W - 16 - 46, (ruleY - 20) / 2);
}

// Body: QRIS logo, store name, QR. Only redrawn on a full refresh.
static void drawPortraitBody() {
  fillRect(0, BAR_H, P_W, P_H - BAR_H, false);
  int y = BAR_H + 28;
  drawBitmap((P_W - QRIS_LOGO_W) / 2, y, QRIS_LOGO, QRIS_LOGO_W, QRIS_LOGO_H);
  y += QRIS_LOGO_H + 24;

  if (ui.merchant[0]) {
    int sz = fitSize(ui.merchant, 40, 18, P_W - 48);
    drawText(ui.merchant, P_W / 2, y, sz, T_CENTER);
    y += sz + 20;
  }

  const int qrBottom = P_H - 32;                     // room for "SD error" underneath
  if (!drawQr(ui.qris, 0, y, P_W, qrBottom - y, 12)) {
    drawText("QRIS", P_W / 2, (y + qrBottom) / 2 - 50, 48, T_CENTER);
    drawText("tidak valid", P_W / 2, (y + qrBottom) / 2 + 10, 32, T_CENTER);
  }
  if (!ui.sdOk) drawText("SD error", P_W - 16, P_H - 26, 18, T_RIGHT);
}

// ====================== RENDER ======================
static void renderFull() {
  bool p = isPortrait();
  selectCanvas(p);
  Paint_Clear(WHITE);
  if (p) { drawPortraitBody(); drawPortraitBar(); }
  else   { drawLeft(); drawPanel(); }
  panelFull(fullBuf);
  if (shownBuf) memcpy(shownBuf, fullBuf, SCR_W / 8 * SCR_H);
}
// The QR part is unchanged in fullBuf, so only the status area is redrawn.
// Orientation and mode changes always go through renderFull first.
// Returns false if nothing on screen changed (no refresh was done).
static bool renderUpdate() {
  bool p = isPortrait();
  selectCanvas(p);
  if (p) drawPortraitBar(); else drawPanel();
  const size_t n = SCR_W / 8 * SCR_H;
  if (shownBuf && memcmp(shownBuf, fullBuf, n) == 0) return false;   // identical frame
#if USE_PARTIAL
  if (shownBuf) panelPartial(fullBuf, shownBuf);
  else          panelFast(fullBuf);
#else
  panelFast(fullBuf);
#endif
  if (shownBuf) memcpy(shownBuf, fullBuf, n);
  return true;
}

// ---------- public API ----------
enum UiEvtKind : uint8_t { EV_PAID, EV_INVALID, EV_BLE, EV_QRIS, EV_ORIENT };
struct UiEvt { UiEvtKind k; uint32_t v; };
static QueueHandle_t uiQueue = nullptr;

static bool epdUiInit(bool sdOk, const char* qris, bool portrait = false) {
  ui.sdOk = sdOk;
  ui.portrait = portrait;
  applyQris(qris);
  pendingLock = xSemaphoreCreateMutex();

  fullBuf = (UBYTE*)ps_malloc(SCR_W / 8 * SCR_H);   // 48 KB in PSRAM
  if (!fullBuf) { Serial.println("EPD: no memory for frame buffer"); return false; }
  shownBuf = (UBYTE*)ps_malloc(SCR_W / 8 * SCR_H);  // +48 KB; without it, fast refresh is used
  if (!shownBuf) Serial.println("EPD: no memory for partial refresh, using fast refresh");

  ttfOk = loadEmbeddedTtf() || (sdOk && loadTtf());  // built-in font works even without SD
  batteryInit();
  ui.bat = batteryRead();
  panelInit();
  renderFull();
  ui.shownBle = ui.ble;
  uiQueue = xQueueCreate(8, sizeof(UiEvt));
  return true;
}

static void uiPost(UiEvtKind k, uint32_t v) {
  if (!uiQueue) return;
  UiEvt e{k, v};
  xQueueSend(uiQueue, &e, 0);
}
static void epdShow(uint32_t amount) { uiPost(EV_PAID, amount); }
static void epdShowInvalid()         { uiPost(EV_INVALID, 0); }
static void epdSetBle(bool on)       { uiPost(EV_BLE, on); }
static void epdSetPortrait(bool on)  { uiPost(EV_ORIENT, on); }
static void epdSetQris(const char* q) {
  if (!pendingLock) return;
  xSemaphoreTake(pendingLock, portMAX_DELAY);
  strlcpy(pendingQris, q ? q : "", sizeof(pendingQris));
  xSemaphoreGive(pendingLock);
  uiPost(EV_QRIS, 0);
}

// Returns: 0 = BLE-only (debounced), 1 = show now (fast refresh), 2 = show now (full refresh)
static int applyEvt(const UiEvt& e) {
  switch (e.k) {
    case EV_QRIS:
      xSemaphoreTake(pendingLock, portMAX_DELAY);
      applyQris(pendingQris);
      xSemaphoreGive(pendingLock);
      return 2;                                      // the QR changed: redraw everything
    case EV_ORIENT: {
      bool p = e.v != 0;
      if (p == ui.portrait) return 0;
      ui.portrait = p;
      return ui.mode == MODE_READY ? 2 : 0;          // setup screen stays landscape
    }
    case EV_PAID:
      if (ui.mode != MODE_READY) return 0;
      if (ui.amount) {                               // push previous amount into history
        ui.hist[2] = ui.hist[1]; ui.hist[1] = ui.hist[0]; ui.hist[0] = ui.amount;
      }
      ui.amount = e.v;
      ui.status = ST_PAID;
      ui.amountUntil = millis() + PORTRAIT_AMOUNT_MS;
      ui.amountTimer = true;
      if (ui.before == ST_INVALID) ui.before = ST_PAID;
      return 1;
    case EV_INVALID:
      if (ui.mode != MODE_READY) return 0;
      if (ui.status != ST_INVALID) ui.before = ui.status;
      ui.status = ST_INVALID;
      ui.invalidUntil = millis() + INVALID_SHOW_MS;
      return 1;
    case EV_BLE:
      ui.ble = e.v != 0;
      return 0;
  }
  return 0;
}

static void uiTask(void*) {
  esp_task_wdt_add(nullptr);                         // watched: a stuck refresh reboots the box
  int fastCount = 0;
  uint32_t nextBat = millis() + BAT_POLL_MS;
  for (;;) {
    esp_task_wdt_reset();                            // a frozen panel/refresh reboots the box
    int32_t waitMs = (int32_t)(nextBat - millis());
    if (ui.status == ST_INVALID) {
      int32_t left = (int32_t)(ui.invalidUntil - millis());
      if (left < waitMs) waitMs = left;
    }
    if (ui.amountTimer) {
      int32_t left = (int32_t)(ui.amountUntil - millis());
      if (left < waitMs) waitMs = left;
    }
    if (waitMs < 0) waitMs = 0;
    if (waitMs > 10000) waitMs = 10000;              // wake up regularly to feed the watchdog

    UiEvt e;
    int level = 0;                                   // max refresh level requested
    bool got = false;
    if (xQueueReceive(uiQueue, &e, pdMS_TO_TICKS(waitMs)) == pdTRUE) {
      got = true;
      level = applyEvt(e);
      // Payments refresh immediately; BLE-only changes wait for the link to settle.
      // Events arriving meanwhile are merged into the same refresh.
      TickType_t settle = level ? 0 : pdMS_TO_TICKS(BLE_DEBOUNCE_MS);
      while (xQueueReceive(uiQueue, &e, settle) == pdTRUE) {
        esp_task_wdt_reset();
        int l = applyEvt(e);
        if (l > level) level = l;
        if (level) settle = 0;
      }
    }
    if (ui.status == ST_INVALID && (int32_t)(ui.invalidUntil - millis()) <= 0) {
      ui.status = ui.before;
      if (!level) level = 1;
    }
    if (ui.amountTimer && (int32_t)(ui.amountUntil - millis()) <= 0) {
      ui.amountTimer = false;                        // landscape keeps showing the amount
      if (isPortrait() && !level) level = 1;
    }
    if ((int32_t)(millis() - nextBat) >= 0) {
      nextBat = millis() + BAT_POLL_MS;
      BatteryInfo b = batteryRead();
      bool changed = b.present != ui.bat.present || b.charging != ui.bat.charging ||
                     b.percent / BAT_STEP != ui.bat.percent / BAT_STEP;
      ui.bat = b;
      if (changed && !level) level = 1;
    }
    if (got && !level && ui.ble != ui.shownBle) level = 1;   // settled BLE change
    if (!level) continue;

    if (level == 2 || fastCount >= FULL_EVERY || epdTrouble) {
      fastCount = 0;
      epdTrouble = false;
      renderFull();
    }
    else if (renderUpdate()) fastCount++;
    ui.shownBle = ui.ble;
  }
}

static void epdUiStart() {
  // FreeType needs a deep stack
  xTaskCreatePinnedToCore(uiTask, "epd", 24 * 1024, nullptr, 1, nullptr, 0);
}