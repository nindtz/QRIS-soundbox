/*
 * qris.h - QRIS payload checks + BLE payment authentication
 *
 *  - qrisCheck()      : validates EMVCo TLV structure and CRC16 (tag 63)
 *  - qrisGetField()   : reads a top-level tag, e.g. "59" merchant name, "60" city
 *  - authInit()       : derives the HMAC key from the real QRIS string
 *  - parsePayment()   : parses + verifies "<id>;<amount>;<tag>" written by the phone
 *
 * Authentication scheme (phone must do the same):
 *   key = SHA-256( QRIS_PAYLOAD || AUTH_SECRET )          (AUTH_SECRET may be "")
 *   tag = hex( HMAC-SHA256( key, "<id>;<amount>" ) )[0..15]  (first 8 bytes, lowercase hex)
 *   BLE write = "<id>;<amount>;<tag>"   e.g. "a1b2c3d4;25000;3f9c0e5a7b21d4e8"
 *
 * A notification signed for a different QR (other merchant / changed QR) fails the check.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <strings.h>

#if defined(ARDUINO) || defined(ESP_PLATFORM)
  #include "mbedtls/md.h"
#else                                   // host build, used only for unit tests
  #include <openssl/hmac.h>
  #include <openssl/sha.h>
#endif

// ---------- QRIS / EMVCo helpers ----------
// CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF), over everything up to and incl. "6304"
static uint16_t qrisCrc16(const char* s, size_t n) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < n; i++) {
    crc ^= (uint16_t)(uint8_t)s[i] << 8;
    for (int b = 0; b < 8; b++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
  }
  return crc;
}

static int twoDigits(const char* p) {
  if (!isdigit((unsigned char)p[0]) || !isdigit((unsigned char)p[1])) return -1;
  return (p[0] - '0') * 10 + (p[1] - '0');
}

// Walks top-level TLVs: ID(2) LEN(2) VALUE(LEN). Returns false if the structure is broken.
// If `tag` is given and found, copies its value into out (NUL-terminated, truncated to outLen-1).
static bool qrisWalk(const char* p, const char* tag, char* out, size_t outLen, bool* found) {
  size_t n = strlen(p), i = 0;
  if (found) *found = false;
  while (i < n) {
    if (i + 4 > n) return false;
    int id = twoDigits(p + i), len = twoDigits(p + i + 2);
    if (id < 0 || len < 0 || i + 4 + (size_t)len > n) return false;
    if (tag && found && !*found && strncmp(p + i, tag, 2) == 0) {
      size_t c = (size_t)len < outLen - 1 ? (size_t)len : outLen - 1;
      memcpy(out, p + i + 4, c);
      out[c] = 0;
      *found = true;
    }
    i += 4 + len;
  }
  return i == n;
}

static bool qrisGetField(const char* p, const char* tag, char* out, size_t outLen) {
  bool found = false;
  if (!qrisWalk(p, tag, out, outLen, &found) || !found) { if (outLen) out[0] = 0; return false; }
  return true;
}

// Full sanity check of a QRIS string before we show it to customers.
static bool qrisCheck(const char* p) {
  size_t n = strlen(p);
  if (n < 16 || strncmp(p, "000201", 6) != 0) return false;          // payload format indicator
  if (strncmp(p + n - 8, "6304", 4) != 0) return false;              // CRC must be the last tag
  if (!qrisWalk(p, nullptr, nullptr, 0, nullptr)) return false;
  char want[5];
  snprintf(want, sizeof(want), "%04X", qrisCrc16(p, n - 4));
  return strncasecmp(want, p + n - 4, 4) == 0;
}

// ---------- crypto (mbedtls on ESP32, OpenSSL on the host test) ----------
static void sha256(const uint8_t* d, size_t n, uint8_t out[32]) {
#if defined(ARDUINO) || defined(ESP_PLATFORM)
  mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), d, n, out);
#else
  SHA256(d, n, out);
#endif
}

static void hmacSha256(const uint8_t* k, size_t kn, const uint8_t* m, size_t mn, uint8_t out[32]) {
#if defined(ARDUINO) || defined(ESP_PLATFORM)
  mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), k, kn, m, mn, out);
#else
  unsigned int len = 32;
  HMAC(EVP_sha256(), k, (int)kn, m, mn, out, &len);
#endif
}

#define AUTH_TAG_BYTES 8                       // 16 hex chars on the wire
static uint8_t g_authKey[32];

static void authInit(const char* qrisPayload, const char* secret) {
  // key = SHA-256(QRIS || secret), done in one pass without allocating
  size_t a = strlen(qrisPayload), b = secret ? strlen(secret) : 0;
  uint8_t buf[512];
  if (a + b <= sizeof(buf)) {
    memcpy(buf, qrisPayload, a);
    if (b) memcpy(buf + a, secret, b);
    sha256(buf, a + b, g_authKey);
    memset(buf, 0, sizeof(buf));
  } else {
    sha256((const uint8_t*)qrisPayload, a, g_authKey);   // QRIS is never this long; just in case
  }
}

static int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

#ifndef QRIS_MAX_LEN
#define QRIS_MAX_LEN 512                               // EMVCo maximum
#endif

// Config write handed from the BLE callback to the main loop. Lives in a header (not the .ino)
// because the Arduino IDE puts auto-generated function prototypes above .ino type definitions.
struct CfgMsg {
  bool reset;
  char qris[QRIS_MAX_LEN + 1];
};

// ---------- payment payload ----------
struct Payment {
  char     id[16];
  uint32_t amount;
};

enum PayResult { PAY_OK, PAY_BAD_FORMAT, PAY_BAD_TAG };

// buf: NUL-terminated "<id>;<amount>;<tag>"
static PayResult parsePayment(const char* buf, Payment& p) {
  const char* s1 = strchr(buf, ';');
  if (!s1) return PAY_BAD_FORMAT;
  const char* s2 = strchr(s1 + 1, ';');
  if (!s2 || strchr(s2 + 1, ';')) return PAY_BAD_FORMAT;

  // id: 1..15 chars of [A-Za-z0-9_-]
  size_t idLen = s1 - buf;
  if (idLen == 0 || idLen >= sizeof(p.id)) return PAY_BAD_FORMAT;
  for (size_t i = 0; i < idLen; i++) {
    char c = buf[i];
    if (!(isalnum((unsigned char)c) || c == '_' || c == '-')) return PAY_BAD_FORMAT;
  }

  // amount: 1..9 digits, no sign/space/leading zero, 1 .. 999,999,999
  size_t aLen = s2 - (s1 + 1);
  if (aLen == 0 || aLen > 9 || s1[1] == '0') return PAY_BAD_FORMAT;
  uint32_t amt = 0;
  for (size_t i = 0; i < aLen; i++) {
    char c = s1[1 + i];
    if (!isdigit((unsigned char)c)) return PAY_BAD_FORMAT;
    amt = amt * 10 + (uint32_t)(c - '0');
  }

  // tag: exactly 16 hex chars
  const char* t = s2 + 1;
  if (strlen(t) != AUTH_TAG_BYTES * 2) return PAY_BAD_FORMAT;
  uint8_t got[AUTH_TAG_BYTES];
  for (int i = 0; i < AUTH_TAG_BYTES; i++) {
    int h = hexNibble(t[2 * i]), l = hexNibble(t[2 * i + 1]);
    if (h < 0 || l < 0) return PAY_BAD_FORMAT;
    got[i] = (uint8_t)((h << 4) | l);
  }

  // HMAC over the exact bytes "<id>;<amount>"
  uint8_t mac[32];
  hmacSha256(g_authKey, sizeof(g_authKey), (const uint8_t*)buf, (size_t)(s2 - buf), mac);
  uint8_t diff = 0;                                     // constant-time compare
  for (int i = 0; i < AUTH_TAG_BYTES; i++) diff |= (uint8_t)(mac[i] ^ got[i]);
  if (diff) return PAY_BAD_TAG;

  memcpy(p.id, buf, idLen);
  p.id[idLen] = 0;
  p.amount = amt;
  return PAY_OK;
}
