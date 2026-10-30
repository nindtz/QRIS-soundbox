package id.soundbox.bridge

import android.app.Application
import android.content.Context
import android.os.Handler
import android.os.Looper
import android.util.Log
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

class App : Application() {
    override fun onCreate() {
        super.onCreate()
        Bridge.init(this)
    }
}

/** Glue between the WhatsApp listener, the BLE client and the screen. Main thread only. */
object Bridge {
    private lateinit var app: Context
    val ble: SoundboxBle by lazy { SoundboxBle(app) }
    val prefs: Prefs by lazy { Prefs(app) }

    private val main = Handler(Looper.getMainLooper())
    private val lines = ArrayDeque<String>()
    private val listeners = mutableSetOf<() -> Unit>()
    private val timeFmt = SimpleDateFormat("HH:mm:ss", Locale.US)

    /** PIN being changed: saved once the box confirms it */
    var pendingPin: String? = null

    fun init(ctx: Context) {
        if (!::app.isInitialized) app = ctx.applicationContext
    }

    // ---------- log + screen updates ----------

    fun log(msg: String) {
        Log.i("SoundboxBridge", msg)
        main.post {
            lines.addLast("${timeFmt.format(Date())}  $msg")
            while (lines.size > 300) lines.removeFirst()
            changed()
        }
    }

    fun logText(): String = lines.joinToString("\n")

    fun addListener(l: () -> Unit) { listeners += l }
    fun removeListener(l: () -> Unit) { listeners -= l }
    fun changed() = main.post { listeners.toList().forEach { it() } }

    // ---------- actions ----------

    fun connect() {
        val addr = prefs.deviceAddress
        if (addr == null) log("No soundbox chosen yet: tap \"Choose soundbox\"")
        else ble.connect(addr)
    }

    fun config(command: String, label: String) {
        connect()
        ble.writeConfig("${prefs.pin}|$command", label)
    }

    fun sendPayment(amount: Long, source: String, badTag: Boolean = false) {
        val qris = prefs.qris
        if (qris.isBlank()) { log("Set the QRIS first (the same one stored on the box)"); return }
        val payload = if (badTag) Signer.badPayment(amount) else Signer.payment(amount, qris, prefs.secret)
        log("$source: ${AmountParser.format(amount)}${if (badTag) " (bad tag)" else ""}")
        connect()
        ble.writePayment(payload)
    }

    /** Called by [PaymentListener] for each new WhatsApp message that passed the chat filter. */
    fun onWhatsAppMessage(sender: String, text: String) {
        val amount = AmountParser.parse(text)
        val preview = text.replace('\n', ' ').take(60)
        if (amount == null) { log("WhatsApp $sender: no amount in \"$preview\""); return }
        sendPayment(amount, "WhatsApp $sender")
    }

    fun onBoxStatus(s: String) {
        log("Box status: $s")
        val pin = pendingPin
        if (pin != null && (s.startsWith("READY") || s.startsWith("SETUP"))) {
            prefs.pin = pin
            pendingPin = null
            log("PIN changed and saved in the app")
        } else if (pin != null && s.startsWith("ERR")) {
            pendingPin = null
        }
        when (s) {
            "ERR:PIN" -> log("Wrong PIN: enter the PIN used for the box's first setup")
            "ERR:WAIT" -> log("Too many wrong PINs: the box refuses settings for 5 minutes")
            "ERR:INVALID" -> log("The box rejected the command (bad QRIS or value)")
        }
        if (s.startsWith("READY:")) {
            val boxCrc = s.split(":").getOrNull(1)?.uppercase()
            val ours = prefs.qris.takeLast(4).uppercase()
            if (prefs.qris.isNotBlank() && boxCrc != null && boxCrc != ours)
                log("Warning: the box has a different QRIS (CRC $boxCrc, app $ours): payments will be rejected")
        }
        changed()
    }
}
