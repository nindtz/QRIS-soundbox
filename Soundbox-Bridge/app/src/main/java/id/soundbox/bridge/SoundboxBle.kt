package id.soundbox.bridge

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.BluetoothStatusCodes
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import java.util.UUID

/**
 * GATT client for the soundbox. Everything runs on the main thread; Android allows only one
 * GATT operation at a time, so writes/reads go through a queue.
 *
 * Robustness rules:
 *  - a request the box doesn't answer means the link is broken: reconnect and retry it
 *    (Android can't cancel a stuck GATT request, so carrying on would only stall the next one)
 *  - a callback only completes the request it belongs to (late answers are ignored)
 *  - after a disconnect, reconnect with autoConnect=true: the Bluetooth chip waits for the box
 *    by itself, which keeps working while the phone sleeps (app timers don't)
 */
@SuppressLint("MissingPermission")   // checked in hasPermission() before any call
class SoundboxBle(private val ctx: Context) {

    companion object {
        val SERVICE: UUID = UUID.fromString("6f1c0001-5b7a-4c1e-9d3a-2a4f7c9b0001")
        val PAY: UUID = UUID.fromString("6f1c0002-5b7a-4c1e-9d3a-2a4f7c9b0001")
        val CFG: UUID = UUID.fromString("6f1c0003-5b7a-4c1e-9d3a-2a4f7c9b0001")
        private val CCCD: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

        private const val MTU = 247
        private const val OP_TIMEOUT_MS = 20_000L        // long enough for a first pairing prompt
        private const val SETTLE_MS = 600L               // let link encryption finish before the first request
        private const val PAY_MAX_AGE_MS = 60_000L       // don't announce payments older than this
        private const val RETRY_MS = 5_000L              // Bluetooth off / auto-connect failed
        private val AUTH_ERRORS = setOf(5, 15, 137)       // insufficient authentication/encryption, auth fail
    }

    enum class State { IDLE, CONNECTING, READY }

    var state = State.IDLE
        private set
    /** Last status from the box's CONFIG characteristic, e.g. "READY:28AF:P" */
    var boxStatus = ""
        private set

    private val main = Handler(Looper.getMainLooper())
    private val manager = ctx.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
    private var gatt: BluetoothGatt? = null
    private var address: String? = null
    private var wanted = false
    private var autoMode = false            // current gatt was opened with autoConnect=true

    private enum class Kind { WRITE, READ, NOTIFY }
    private class Op(
        val kind: Kind, val uuid: UUID, val label: String,
        val data: ByteArray = ByteArray(0), val isPayment: Boolean = false,
    ) {
        val created = SystemClock.elapsedRealtime()
        var attempts = 0
        val maxAttempts = if (isPayment) 3 else 2
    }

    private val queue = ArrayDeque<Op>()
    private var current: Op? = null

    // ---------------- public API ----------------

    fun connect(addr: String) = main.post {
        if (addr == address && wanted && gatt != null) return@post
        close()
        address = addr
        wanted = true
        open(auto = false)
    }

    fun disconnect() = main.post {
        wanted = false
        close()
        queue.clear()
        setState(State.IDLE)
    }

    fun writePayment(payload: String) =
        enqueue(Op(Kind.WRITE, PAY, "payment $payload", payload.toByteArray(Charsets.UTF_8), isPayment = true))

    /** [label] is logged instead of the command, so the PIN doesn't end up in the log */
    fun writeConfig(command: String, label: String) =
        enqueue(Op(Kind.WRITE, CFG, label, command.toByteArray(Charsets.UTF_8)))

    fun readStatus() = enqueue(Op(Kind.READ, CFG, "read status"))

    // ---------------- connection ----------------

    private fun hasPermission(): Boolean =
        Build.VERSION.SDK_INT < 31 ||
            ctx.checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED

    private fun open(auto: Boolean) {
        val addr = address ?: return
        if (gatt != null) return
        if (!hasPermission()) { Bridge.log("Bluetooth permission not granted"); return }
        val adapter = manager.adapter
        if (adapter == null || !adapter.isEnabled) {
            Bridge.log("Bluetooth is off, retrying")
            scheduleOpen(auto = true, RETRY_MS)
            return
        }
        autoMode = auto
        setState(State.CONNECTING)
        Bridge.log(if (auto) "Waiting for the box (reconnects automatically)" else "Connecting to $addr")
        gatt = adapter.getRemoteDevice(addr).connectGatt(ctx, auto, callback, BluetoothDevice.TRANSPORT_LE)
    }

    private var openAuto = true
    private val openRun = Runnable { if (wanted && gatt == null) open(openAuto) }
    private fun scheduleOpen(auto: Boolean, delayMs: Long) {
        main.removeCallbacks(openRun)
        openAuto = auto
        if (wanted) main.postDelayed(openRun, delayMs)
    }

    /** Puts the unfinished request back in front of the queue, if it has attempts left. */
    private fun requeueCurrent() {
        val op = current ?: return
        current = null
        if (op.attempts < op.maxAttempts) queue.addFirst(op)
        else Bridge.log("Gave up after ${op.attempts} tries: ${op.label}")
    }

    private fun close() {
        main.removeCallbacks(openRun)
        main.removeCallbacks(timeoutRun)
        main.removeCallbacks(discoverFallback)
        gatt?.let { it.disconnect(); it.close() }
        gatt = null
        requeueCurrent()
    }

    /** The link is unusable (stuck request etc.): drop it and connect again. */
    private fun restart(reason: String) {
        Bridge.log(reason)
        close()
        setState(State.IDLE)
        scheduleOpen(auto = false, 1_000)
    }

    private val discoverFallback = Runnable { gatt?.discoverServices() }   // if onMtuChanged never comes

    private fun setState(s: State) {
        state = s
        Bridge.changed()
    }

    /** Android caches a device's services; clear it when the cached list looks wrong. */
    private fun refreshCache(g: BluetoothGatt) {
        try {
            g.javaClass.getMethod("refresh").invoke(g)
            Bridge.log("Cleared Android's cached service list for the box")
        } catch (_: Exception) {
        }
    }

    // ---------------- operation queue ----------------

    private fun enqueue(op: Op) = main.post {
        queue.addLast(op)
        if (address == null) Bridge.log("No soundbox chosen yet: tap \"Choose soundbox\"")
        else if (gatt == null) { wanted = true; open(auto = false) }
        next()
    }

    private fun next() {
        if (current != null || state != State.READY) return
        val g = gatt ?: return
        while (true) {
            val op = queue.removeFirstOrNull() ?: return
            if (op.isPayment && SystemClock.elapsedRealtime() - op.created > PAY_MAX_AGE_MS) {
                Bridge.log("Dropped (older than 60 s): ${op.label}")
                continue
            }
            val ch = g.getService(SERVICE)?.getCharacteristic(op.uuid)
            if (ch == null) { Bridge.log("Characteristic missing for ${op.label}"); continue }
            op.attempts++
            current = op
            val started = when (op.kind) {
                Kind.WRITE -> write(g, ch, op.data)
                Kind.READ -> g.readCharacteristic(ch)
                Kind.NOTIFY -> enableNotify(g, ch)
            }
            if (!started) {
                restart("Bluetooth busy, could not send: ${op.label}. Reconnecting")
                return
            }
            main.postDelayed(timeoutRun, OP_TIMEOUT_MS)
            return
        }
    }

    private val timeoutRun = Runnable {
        current?.let { restart("No answer from the box (${it.label}). Reconnecting") }
    }

    private fun finish() {
        main.removeCallbacks(timeoutRun)
        current = null
        next()
    }

    private fun isCurrent(kind: Kind, uuid: UUID) = current?.let { it.kind == kind && it.uuid == uuid } == true

    /** Write finished with [status]; on a pairing/encryption error, retry after a moment. */
    private fun completeWrite(status: Int) {
        val op = current ?: return
        when {
            status == BluetoothGatt.GATT_SUCCESS -> Bridge.log("Sent: ${op.label}")
            status in AUTH_ERRORS && op.attempts < op.maxAttempts -> {
                Bridge.log("Pairing with the box... accept the prompt if one appears")
                main.removeCallbacks(timeoutRun)
                current = null
                queue.addFirst(op)
                main.postDelayed({ next() }, 3_000)
                return
            }
            else -> Bridge.log("Write failed (status $status): ${op.label}")
        }
        finish()
    }

    @Suppress("DEPRECATION")
    private fun write(g: BluetoothGatt, ch: BluetoothGattCharacteristic, data: ByteArray): Boolean =
        if (Build.VERSION.SDK_INT >= 33) {
            g.writeCharacteristic(ch, data, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT) ==
                BluetoothStatusCodes.SUCCESS
        } else {
            ch.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT   // with response; long write if > MTU
            ch.value = data
            g.writeCharacteristic(ch)
        }

    @Suppress("DEPRECATION")
    private fun enableNotify(g: BluetoothGatt, ch: BluetoothGattCharacteristic): Boolean {
        g.setCharacteristicNotification(ch, true)
        val d = ch.getDescriptor(CCCD) ?: return false
        val v = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
        return if (Build.VERSION.SDK_INT >= 33) {
            g.writeDescriptor(d, v) == BluetoothStatusCodes.SUCCESS
        } else {
            d.value = v
            g.writeDescriptor(d)
        }
    }

    private fun onBoxStatus(value: ByteArray?) {
        val s = value?.toString(Charsets.UTF_8)?.trim().orEmpty()
        if (s.isEmpty()) return
        boxStatus = s
        Bridge.onBoxStatus(s)
    }

    // ---------------- callbacks (binder thread -> main) ----------------

    private val callback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            main.post {
                if (g != gatt) { g.close(); return@post }
                if (newState == BluetoothProfile.STATE_CONNECTED && status == BluetoothGatt.GATT_SUCCESS) {
                    Bridge.log("Connected, setting up")
                    if (g.requestMtu(MTU)) main.postDelayed(discoverFallback, 2_000) else g.discoverServices()
                } else {
                    Bridge.log("Disconnected (status $status)")
                    val wasAuto = autoMode
                    close()
                    setState(State.IDLE)
                    // A failed auto-connect returns at once, so wait before trying again
                    if (wasAuto) scheduleOpen(auto = true, RETRY_MS) else open(auto = true)
                }
            }
        }

        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) {
            main.post {
                if (g != gatt) return@post
                main.removeCallbacks(discoverFallback)
                Bridge.log("MTU $mtu")
                g.discoverServices()
            }
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            main.post {
                if (g != gatt) return@post
                main.removeCallbacks(discoverFallback)
                val svc = g.getService(SERVICE)
                if (status != BluetoothGatt.GATT_SUCCESS || svc == null) {
                    Bridge.log("Soundbox service not found (status $status)")
                    refreshCache(g)
                    g.disconnect()
                    return@post
                }
                if (svc.getCharacteristic(PAY) == null || svc.getCharacteristic(CFG) == null)
                    Bridge.log("Box is missing a characteristic: update its firmware")
                main.postDelayed({
                    if (g != gatt) return@postDelayed
                    queue.removeAll { it.kind != Kind.WRITE }          // no duplicate setup requests
                    queue.addFirst(Op(Kind.READ, CFG, "read status"))
                    queue.addFirst(Op(Kind.NOTIFY, CFG, "status notifications"))
                    setState(State.READY)
                    Bridge.log("Ready")
                    next()
                }, SETTLE_MS)
            }
        }

        override fun onCharacteristicWrite(g: BluetoothGatt, ch: BluetoothGattCharacteristic, status: Int) {
            main.post { if (g == gatt && isCurrent(Kind.WRITE, ch.uuid)) completeWrite(status) }
        }

        // Android 13+
        override fun onCharacteristicRead(
            g: BluetoothGatt, ch: BluetoothGattCharacteristic, value: ByteArray, status: Int,
        ) {
            main.post {
                if (g != gatt || !isCurrent(Kind.READ, ch.uuid)) return@post
                if (status == BluetoothGatt.GATT_SUCCESS) onBoxStatus(value)
                else Bridge.log("Read failed (status $status)")
                finish()
            }
        }

        // Android 12 and older
        @Deprecated("Deprecated in Java")
        @Suppress("DEPRECATION")
        override fun onCharacteristicRead(g: BluetoothGatt, ch: BluetoothGattCharacteristic, status: Int) {
            if (Build.VERSION.SDK_INT >= 33) return
            val value = ch.value
            onCharacteristicRead(g, ch, value ?: ByteArray(0), status)
        }

        // Android 13+
        override fun onCharacteristicChanged(g: BluetoothGatt, ch: BluetoothGattCharacteristic, value: ByteArray) {
            main.post { if (g == gatt && ch.uuid == CFG) onBoxStatus(value) }
        }

        // Android 12 and older
        @Deprecated("Deprecated in Java")
        @Suppress("DEPRECATION")
        override fun onCharacteristicChanged(g: BluetoothGatt, ch: BluetoothGattCharacteristic) {
            if (Build.VERSION.SDK_INT >= 33) return
            val value = ch.value ?: return
            main.post { if (g == gatt && ch.uuid == CFG) onBoxStatus(value) }
        }

        override fun onDescriptorWrite(g: BluetoothGatt, d: BluetoothGattDescriptor, status: Int) {
            main.post {
                if (g != gatt || !isCurrent(Kind.NOTIFY, d.characteristic.uuid)) return@post
                if (status != BluetoothGatt.GATT_SUCCESS) Bridge.log("Notifications not enabled (status $status)")
                finish()
            }
        }
    }
}
