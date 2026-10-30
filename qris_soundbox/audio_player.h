#pragma once
/*
 * Clip player for Waveshare ESP32-S3-ePaper-3.97 (ES8311 + NS4150B).
 * Clips live on the SD card: /clips/NN.wav  (PCM 16-bit, MONO, 24000 Hz).
 * The SD card must already be mounted (see sdInit() in the .ino).
 *
 * If PSRAM is available, all clips are cached in RAM at boot so playback has
 * no SD latency between words. Otherwise they are streamed from the card.
 * Needs es8311.h / es8311.c from the Waveshare example in the sketch folder.
 */
#include <Arduino.h>
#include <vector>
#include "ESP_I2S.h"
#include "esp_check.h"
#include "Wire.h"
#include "es8311.h"
#include "FS.h"
#include "SD_MMC.h"

#define I2C_SDA 41
#define I2C_SCL 42
#define I2S_MCK_PIN 13
#define I2S_BCK_PIN 14
#define I2S_LRCK_PIN 47
#define I2S_DOUT_PIN 48
#define I2S_DIN_PIN 21
#define PA_CTRL 39

#define AUDIO_SAMPLE_RATE 24000
#define AUDIO_MCLK_FREQ_HZ (AUDIO_SAMPLE_RATE * 256)
#define AUDIO_VOLUME 70          // 0..100, tune for your speaker
#define CLIP_COUNT 21            // array size; clips are 01.wav .. 20.wav (index 0 unused)

static I2SClass audioI2s;

struct ClipBuf { uint8_t* data; uint32_t len; };
static ClipBuf clipCache[CLIP_COUNT];   // zero-initialised (static storage)

static bool audioCodecInit() {
  es8311_handle_t es = es8311_create(I2C_NUM_0, ES8311_ADDRRES_0);
  if (!es) return false;
  const es8311_clock_config_t clk = {
    .mclk_inverted = false,
    .sclk_inverted = false,
    .mclk_from_mclk_pin = true,
    .mclk_frequency = AUDIO_MCLK_FREQ_HZ,
    .sample_frequency = AUDIO_SAMPLE_RATE
  };
  if (es8311_init(es, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK) return false;
  if (es8311_voice_volume_set(es, AUDIO_VOLUME, NULL) != ESP_OK) return false;
  if (es8311_microphone_config(es, false) != ESP_OK) return false;
  return true;
}

/** Codec + I2S init. Leaves the amplifier OFF. */
static bool audioInit() {
  pinMode(PA_CTRL, OUTPUT);
  digitalWrite(PA_CTRL, LOW);

  Wire.begin(I2C_SDA, I2C_SCL);
  if (!audioCodecInit()) { Serial.println("ES8311 init failed"); return false; }

  audioI2s.setPins(I2S_BCK_PIN, I2S_LRCK_PIN, I2S_DOUT_PIN, I2S_DIN_PIN, I2S_MCK_PIN);
  if (!audioI2s.begin(I2S_MODE_STD, AUDIO_SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT,
                      I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT)) {
    Serial.println("I2S init failed");
    return false;
  }
  return true;
}

// Zeros: avoids pops when the amp switches and stops the tail being cut off.
static void audioSilence(uint32_t ms) {
  static uint8_t zeros[1024] = {0};
  size_t bytes = (size_t)(AUDIO_SAMPLE_RATE / 1000) * ms * 2;
  while (bytes) {
    size_t n = min(bytes, sizeof(zeros));
    audioI2s.write(zeros, n);
    bytes -= n;
  }
}

// Find the "data" chunk, check the format. Leaves the file positioned at the PCM data.
static bool wavOpenData(File& f, uint32_t& dataLen) {
  uint8_t hdr[12];
  if (f.read(hdr, 12) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) return false;
  uint8_t ck[8];
  while (f.read(ck, 8) == 8) {
    uint32_t sz = ck[4] | (ck[5] << 8) | (ck[6] << 16) | ((uint32_t)ck[7] << 24);
    if (!memcmp(ck, "fmt ", 4) && sz >= 16) {
      uint8_t fmt[16];
      f.read(fmt, 16);
      uint16_t ch   = fmt[2] | (fmt[3] << 8);
      uint32_t rate = fmt[4] | (fmt[5] << 8) | (fmt[6] << 16) | ((uint32_t)fmt[7] << 24);
      uint16_t bits = fmt[14] | (fmt[15] << 8);
      if (ch != 1 || rate != AUDIO_SAMPLE_RATE || bits != 16) {
        Serial.printf("Clip format mismatch: %uch %luHz %ubit (need 1ch 24000Hz 16bit)\n",
                      ch, (unsigned long)rate, bits);
        return false;
      }
      f.seek(f.position() + (sz - 16) + (sz & 1));
      continue;
    }
    if (!memcmp(ck, "data", 4)) { dataLen = sz; return true; }
    f.seek(f.position() + sz + (sz & 1));
  }
  return false;
}

/** Call after the SD card is mounted. Caches all clips in PSRAM if present. */
static void audioLoadClips() {
  if (!psramFound()) {
    Serial.println("No PSRAM: clips will be streamed from SD");
    return;
  }
  uint8_t loaded = 0;
  for (uint8_t i = 1; i < CLIP_COUNT; i++) {
    char path[24];
    snprintf(path, sizeof(path), "/clips/%02u.wav", i);
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) { Serial.printf("Missing %s\n", path); continue; }
    uint32_t len = 0;
    if (!wavOpenData(f, len)) { f.close(); continue; }
    len = min<uint32_t>(len, f.size() - f.position());
    uint8_t* buf = (uint8_t*)ps_malloc(len);
    if (buf && f.read(buf, len) == (int)len) {
      clipCache[i].data = buf;
      clipCache[i].len = len;
      loaded++;
    } else if (buf) {
      free(buf);
    }
    f.close();
  }
  Serial.printf("Cached %u/%u clips in PSRAM\n", loaded, CLIP_COUNT - 1);
}

static void audioPlayFile(uint8_t clip) {
  if (clip < CLIP_COUNT && clipCache[clip].data) {      // fast path: from RAM
    audioI2s.write(clipCache[clip].data, clipCache[clip].len);
    return;
  }
  char path[24];                                         // fallback: stream from SD
  snprintf(path, sizeof(path), "/clips/%02u.wav", clip);
  File f = SD_MMC.open(path, FILE_READ);
  if (!f) { Serial.printf("Missing %s\n", path); return; }

  uint32_t remaining = 0;
  if (!wavOpenData(f, remaining)) { Serial.printf("Bad WAV %s\n", path); f.close(); return; }

  static uint8_t buf[4096];
  while (remaining) {
    int n = f.read(buf, min<uint32_t>(remaining, sizeof(buf)));
    if (n <= 0) break;
    audioI2s.write(buf, n);
    remaining -= n;
  }
  f.close();
}

/** Blocking: plays the clips back to back, amp on only while playing. */
static void audioPlayClips(const std::vector<uint8_t>& clips) {
  digitalWrite(PA_CTRL, HIGH);
  delay(30);                 // let the NS4150B settle
  audioSilence(40);
  for (uint8_t c : clips) audioPlayFile(c);
  audioSilence(150);
  digitalWrite(PA_CTRL, LOW);
}
