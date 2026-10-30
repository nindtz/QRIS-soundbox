# Soundbox Bridge (Android, test app)

Listens to WhatsApp notifications, finds an amount in the message (e.g. "Rp 25.000"),
signs it and sends it to the QRIS soundbox over Bluetooth, the same way `ble_test.py` does.
It can also set up the box (QRIS, PIN, portrait/landscape, reset).

Kotlin, minSdk 26 (Android 8), targetSdk 34. No extra libraries beyond AndroidX + Material.

## Build

1. Android Studio (Koala or newer) > **Open** > this `SoundboxBridge` folder.
   Let it sync; it downloads Gradle 8.7 and the Android plugin. If it asks about the
   Gradle wrapper, accept its offer to use / create it.
2. Plug in the phone with USB debugging on, press **Run**.

## First run

1. **Choose soundbox**: allow the Bluetooth permissions, pick "QRIS-Soundbox" from the list.
   The first write shows an Android "Pair" prompt: accept it (no code needed).
2. **Box setup**
   - Paste the merchant's full QRIS string, enter a PIN (4-16 digits).
     If the firmware has an `AUTH_SECRET`, enter it too.
   - **Send QRIS** (box on the setup screen): the PIN becomes the box's PIN.
   - **Portrait / Landscape**, **Read status**, **Change PIN**, **Reset box** as needed.
   - The app keeps the QRIS: it signs payments with it, so it must be the same QRIS the box has.
     The log warns if they differ.
3. **Allow notification access** > turn on "Soundbox Bridge".
   On Android 13+, a sideloaded app may show "Restricted setting". Then open
   Settings > Apps > Soundbox Bridge > ⋮ (top right) > **Allow restricted settings**, and try again.
4. **Start bridge**: runs in the background with a small notification, keeps the BLE link up.
   Also set Settings > Apps > Soundbox Bridge > Battery to **Unrestricted**, or the phone may
   stop it after a while.

## Testing with WhatsApp

From another phone, send a WhatsApp message to this phone, e.g.

    Rp 25.000
    Pembayaran Rp 1.250.000,00 diterima
    25000

The box announces it. Optional: type a chat or contact name in "Only chat / sender
containing" so only that chat is forwarded (recommended, so normal chats with prices in
them don't trigger the box).

Recognised: `Rp 25.000`, `Rp25000`, `Rp. 150,000`, `Rp 1.250.000,00` (cents dropped), or a
message that is only a number. A number without "Rp" inside a sentence is ignored.

Notes
- WhatsApp only posts a notification when that chat is **not open** on screen, and
  WhatsApp notifications must be on (with message preview).
- Each message is forwarded once; messages older than 2 minutes, or from before the
  listener started, are skipped. If the box is out of range, a payment is kept for 60 s
  and sent when it reconnects, then dropped.
- "Fake" sends a payment with a wrong tag: the box shows TIDAK VALID and stays silent.

## Code map

| File | What it does |
|---|---|
| `PaymentListener.kt` | Notification listener: WhatsApp messages → `Bridge.onWhatsAppMessage` |
| `AmountParser.kt` | Finds the rupiah amount in a message |
| `Signer.kt` | Payment payload + HMAC tag (same as firmware / ble_test.py), QRIS CRC check |
| `SoundboxBle.kt` | GATT client: connect, MTU, notifications, one-at-a-time write queue, reconnect |
| `Bridge.kt` | Glue + log; box status handling (ERR:PIN, CRC mismatch, PIN change) |
| `BridgeService.kt` | Foreground service that keeps it running in the background |
| `MainActivity.kt` | The screen |

For a real product, the listener would watch the payment app's notifications (e.g. the
bank / e-wallet that receives the QRIS payments) instead of WhatsApp: change `PACKAGES` in
`PaymentListener.kt` and the parsing in `AmountParser.kt`.
