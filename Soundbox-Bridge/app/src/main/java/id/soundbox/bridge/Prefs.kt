package id.soundbox.bridge

import android.content.Context

class Prefs(ctx: Context) {
    private val p = ctx.applicationContext.getSharedPreferences("bridge", Context.MODE_PRIVATE)

    private fun str(key: String, def: String = "") = p.getString(key, def) ?: def
    private fun put(key: String, v: String?) = p.edit().putString(key, v).apply()

    /** Bluetooth address of the chosen soundbox */
    var deviceAddress: String?
        get() = p.getString("addr", null)
        set(v) = put("addr", v)

    /** Must be exactly the QRIS stored on the box: payments are signed with it */
    var qris: String
        get() = str("qris")
        set(v) = put("qris", v)

    /** Settings PIN of the box (set by the first setup) */
    var pin: String
        get() = str("pin", "123456")
        set(v) = put("pin", v)

    /** AUTH_SECRET from the firmware (empty by default) */
    var secret: String
        get() = str("secret")
        set(v) = put("secret", v)

    /** Only forward WhatsApp messages whose chat or sender name contains this (empty = all) */
    var chatFilter: String
        get() = str("filter")
        set(v) = put("filter", v)

    var forwardEnabled: Boolean
        get() = p.getBoolean("forward", true)
        set(v) = p.edit().putBoolean("forward", v).apply()
}
