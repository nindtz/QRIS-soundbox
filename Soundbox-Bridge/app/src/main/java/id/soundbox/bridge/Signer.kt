package id.soundbox.bridge

import java.security.MessageDigest
import java.security.SecureRandom
import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec

/**
 * Same payload as the firmware (qris.h) and ble_test.py:
 *   "<id>;<amount>;<tag>"
 *   tag = first 8 bytes (16 hex) of HMAC-SHA256(key, "<id>;<amount>")
 *   key = SHA-256(QRIS + AUTH_SECRET)
 */
object Signer {
    private val rng = SecureRandom()

    private fun ByteArray.hex(): String = joinToString("") { "%02x".format(it.toInt() and 0xFF) }

    private fun randomHex(bytes: Int): String = ByteArray(bytes).also { rng.nextBytes(it) }.hex()

    fun tag(qris: String, secret: String, msg: String): String {
        val key = MessageDigest.getInstance("SHA-256").digest((qris + secret).toByteArray(Charsets.UTF_8))
        val mac = Mac.getInstance("HmacSHA256")
        mac.init(SecretKeySpec(key, "HmacSHA256"))
        return mac.doFinal(msg.toByteArray(Charsets.UTF_8)).hex().substring(0, 16)
    }

    /** A signed payment with a fresh id (the box ignores an id it has already seen). */
    fun payment(amount: Long, qris: String, secret: String): String {
        val msg = "${randomHex(4)};$amount"
        return "$msg;${tag(qris, secret, msg)}"
    }

    /** Deliberately wrong tag: the box should show TIDAK VALID and stay silent. */
    fun badPayment(amount: Long): String = "${randomHex(4)};$amount;${randomHex(8)}"

    /** CRC-16/CCITT-FALSE over everything except the last 4 characters, like the firmware. */
    fun crc16(data: ByteArray): Int {
        var crc = 0xFFFF
        for (b in data) {
            crc = crc xor ((b.toInt() and 0xFF) shl 8)
            repeat(8) {
                crc = if (crc and 0x8000 != 0) ((crc shl 1) xor 0x1021) and 0xFFFF else (crc shl 1) and 0xFFFF
            }
        }
        return crc
    }

    fun qrisCrcOk(qris: String): Boolean {
        if (qris.length < 12) return false
        val body = qris.dropLast(4).toByteArray(Charsets.US_ASCII)
        return "%04X".format(crc16(body)) == qris.takeLast(4).uppercase()
    }

    fun pinOk(pin: String): Boolean = pin.length in 4..16 && pin.all { it in '0'..'9' }
}
