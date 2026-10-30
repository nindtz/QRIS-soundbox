# QRIS soundbox v3

## First setup
No QRIS stored -> the screen shows a QR to `SETUP_URL` (download the app, `https://example.com` for now) plus setup steps.
The app pairs (passkey), then writes the merchant's QRIS text to the **config characteristic** `6f1c0003-...`:
- write `<QRIS text>` (long write, up to 512 bytes): checked (TLV + CRC), stored in flash, screen switches to payments.
- write `RESET`: forget the QRIS, back to the setup screen.
- read / notify: `SETUP`, `READY:<crc4>`, `ERR:INVALID`, `ERR:LENGTH`, `ERR:BUSY`, `ERR:FLASH`.
Test: `python3 tools/ble_test.py --setup`, `--status`, `--reset`.

## Icons and logo
- Bluetooth rune (slashed when disconnected) and battery meter (4 segments, bolt when charging) top-right.
- Battery is read from the TG28 PMU, **assumed AXP2101-compatible at I2C 0x34** (not confirmed by Waveshare docs). If nothing answers there, the battery icon is hidden and Serial prints an I2C scan - send me that line.
- The QRIS logo is `qris_logo.h`, a 200x74 1-bit bitmap made from `tools/qris.svg` with `tools/svg2bits.py` (`python3 svg2bits.py qris.svg 200 ../qris_logo.h preview.png`).

## What changed
- **QR is generated on the device** from `QRIS_PAYLOAD` (qrcodegen, MIT, vendored as `qrcodegen.c/.h`). No more `/bmp/qris.bmp`. The string's TLV structure and CRC16 (tag 63) are checked at boot; a broken string shows "QRIS tidak valid" instead of a QR.
- **TrueType UI** via OpenFontRender, font loaded from `/fonts/ui.ttf` into PSRAM. Merchant name (tag 59) and city (tag 60) are read from the QRIS string. Missing font → Waveshare bitmap fonts.
- **Signed BLE payload**: `"<id>;<amount>;<tag>"`. Payments are signed with the *stored* QRIS. Anything with a wrong/missing tag shows **TRANSAKSI TIDAK VALID** for 8 s and is not announced.

## Signing scheme (phone side)
```
key = SHA-256(QRIS_PAYLOAD + AUTH_SECRET)
tag = lowercase hex of HMAC-SHA256(key, "<id>;<amount>"), first 16 chars
```
- `id`: 1–15 chars `[A-Za-z0-9_-]`; `amount`: 1–999999999, no leading zero.
- Test: `python3 tools/make_payload.py a1b2c3d4 25000` → `a1b2c3d4;25000;f8e8e17d4b573047`

Kotlin:
```kotlin
fun signPayload(qris: String, secret: String, id: String, amount: Long): String {
    val key = MessageDigest.getInstance("SHA-256").digest((qris + secret).toByteArray())
    val msg = "$id;$amount"
    val mac = Mac.getInstance("HmacSHA256").apply { init(SecretKeySpec(key, "HmacSHA256")) }
    val tag = mac.doFinal(msg.toByteArray()).take(8).joinToString("") { "%02x".format(it) }
    return "$msg;$tag"
}
// Android: call gatt.requestMtu(64) after connecting — the payload is ~31 bytes.
```

## Security note
The QRIS string is printed on the screen, so anyone who scans it can compute `SHA-256(QRIS)`. With `AUTH_SECRET = ""` the tag only proves the sender targeted *this* QR (stops notifications meant for another merchant or an old QR). For protection against forged announcements, set `AUTH_SECRET` to a random string shared only with the merchant app. BLE pairing (passkey + encryption) is still the first barrier.

## Setup
- Library Manager: NimBLE-Arduino 2.x, OpenFontRender.
- Board: ESP32S3 Dev Module, PSRAM **OPI**, 16 MB flash, a partition scheme with ≥3 MB app.
- SD: `/fonts/ui.ttf`, `/clips/00.wav..21.wav` (unchanged).
- `display_ui.h` uses the same panel calls as your v1 file: `Init`+`Display` for a full refresh every 10 updates, `Init_Fast`+`Display_Fast` otherwise, then `Sleep`.
