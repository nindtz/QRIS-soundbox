package id.soundbox.bridge

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothManager
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.ComponentName
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.os.ParcelUuid
import android.provider.Settings
import android.service.notification.NotificationListenerService
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.EditText
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.NotificationManagerCompat
import com.google.android.material.materialswitch.MaterialSwitch

class MainActivity : AppCompatActivity() {

    private val prefs get() = Bridge.prefs

    private lateinit var txtState: TextView
    private lateinit var txtNotif: TextView
    private lateinit var txtLog: TextView
    private lateinit var scroll: ScrollView
    private lateinit var edtQris: EditText
    private lateinit var edtPin: EditText
    private lateinit var edtSecret: EditText
    private lateinit var edtNewPin: EditText
    private lateinit var edtFilter: EditText
    private lateinit var edtAmount: EditText
    private lateinit var swForward: MaterialSwitch

    private val render: () -> Unit = { renderState() }

    // ---------- permissions ----------

    private var afterPermissions: (() -> Unit)? = null
    private val permissionLauncher =
        registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) {
            if (bluetoothGranted()) afterPermissions?.invoke()
            else toast("Bluetooth permission is needed to talk to the soundbox")
            afterPermissions = null
        }

    private fun has(p: String) = checkSelfPermission(p) == PackageManager.PERMISSION_GRANTED

    private fun bluetoothGranted(): Boolean =
        if (Build.VERSION.SDK_INT >= 31) has(Manifest.permission.BLUETOOTH_SCAN) && has(Manifest.permission.BLUETOOTH_CONNECT)
        else has(Manifest.permission.ACCESS_FINE_LOCATION)

    private fun withPermissions(action: () -> Unit) {
        val wanted = buildList {
            if (Build.VERSION.SDK_INT >= 31) {
                add(Manifest.permission.BLUETOOTH_SCAN)
                add(Manifest.permission.BLUETOOTH_CONNECT)
            } else {
                add(Manifest.permission.ACCESS_FINE_LOCATION)
            }
            if (Build.VERSION.SDK_INT >= 33) add(Manifest.permission.POST_NOTIFICATIONS)
        }.filterNot { has(it) }
        if (wanted.isEmpty()) action()
        else { afterPermissions = action; permissionLauncher.launch(wanted.toTypedArray()) }
    }

    // ---------- lifecycle ----------

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        txtState = findViewById(R.id.txtState)
        txtNotif = findViewById(R.id.txtNotif)
        txtLog = findViewById(R.id.txtLog)
        scroll = findViewById(R.id.scroll)
        edtQris = findViewById(R.id.edtQris)
        edtPin = findViewById(R.id.edtPin)
        edtSecret = findViewById(R.id.edtSecret)
        edtNewPin = findViewById(R.id.edtNewPin)
        edtFilter = findViewById(R.id.edtFilter)
        edtAmount = findViewById(R.id.edtAmount)
        swForward = findViewById(R.id.swForward)

        edtQris.setText(prefs.qris)
        edtPin.setText(prefs.pin)
        edtSecret.setText(prefs.secret)
        edtFilter.setText(prefs.chatFilter)
        swForward.isChecked = prefs.forwardEnabled
        swForward.setOnCheckedChangeListener { _, on -> prefs.forwardEnabled = on }

        button(R.id.btnScan) { withPermissions { scanForBox() } }
        button(R.id.btnService) {
            withPermissions {
                if (prefs.deviceAddress == null) toast("Choose the soundbox first")
                else BridgeService.start(this)
            }
        }

        button(R.id.btnSendQris) {
            val q = prefs.qris
            when {
                !Signer.qrisCrcOk(q) -> toast("This QRIS has a wrong CRC: copy the full string")
                ("${prefs.pin}|$q").length > 512 -> toast("QRIS too long")
                else -> withPermissions { Bridge.config(q, "send QRIS") }
            }
        }
        button(R.id.btnStatus) { withPermissions { Bridge.connect(); Bridge.ble.readStatus() } }
        button(R.id.btnPortrait) { withPermissions { Bridge.config("ORIENT:P", "portrait") } }
        button(R.id.btnLandscape) { withPermissions { Bridge.config("ORIENT:L", "landscape") } }
        button(R.id.btnChangePin) {
            val newPin = edtNewPin.text.toString().trim()
            if (!Signer.pinOk(newPin)) { toast("The new PIN must be 4-16 digits"); return@button }
            withPermissions {
                Bridge.pendingPin = newPin
                Bridge.config("PIN:$newPin", "change PIN")
                edtNewPin.setText("")
            }
        }
        button(R.id.btnReset) {
            AlertDialog.Builder(this)
                .setTitle("Reset the box?")
                .setMessage("It forgets its QRIS and PIN and goes back to the setup screen.")
                .setPositiveButton("Reset") { _, _ -> withPermissions { Bridge.config("RESET", "reset box") } }
                .setNegativeButton("Cancel", null)
                .show()
        }

        button(R.id.btnNotif) { startActivity(Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS)) }

        button(R.id.btnPay) { sendTest(bad = false) }
        button(R.id.btnPayBad) { sendTest(bad = true) }
    }

    override fun onStart() {
        super.onStart()
        Bridge.addListener(render)
        renderState()
    }

    override fun onStop() {
        Bridge.removeListener(render)
        super.onStop()
    }

    override fun onResume() {
        super.onResume()
        // After an install/update from Android Studio, Android sometimes leaves the listener
        // unbound even though access is on: ask it to reconnect.
        if (notificationAccessOn() && !PaymentListener.connected) {
            Bridge.log("Reconnecting the WhatsApp listener...")
            NotificationListenerService.requestRebind(ComponentName(this, PaymentListener::class.java))
        }
    }

    private fun notificationAccessOn() =
        NotificationManagerCompat.getEnabledListenerPackages(this).contains(packageName)

    override fun onPause() {
        saveFields()
        super.onPause()
    }

    // ---------- helpers ----------

    /** Every button saves the text fields first, so actions use what is on screen. */
    private fun button(id: Int, action: () -> Unit) {
        findViewById<Button>(id).setOnClickListener { saveFields(); action() }
    }

    private fun saveFields() {
        prefs.qris = edtQris.text.toString().replace("\n", "").trim()
        val pin = edtPin.text.toString().trim()
        if (Signer.pinOk(pin)) prefs.pin = pin
        prefs.secret = edtSecret.text.toString()
        prefs.chatFilter = edtFilter.text.toString().trim()
    }

    private fun sendTest(bad: Boolean) {
        val amount = edtAmount.text.toString().toLongOrNull()
        if (amount == null || amount !in 1..AmountParser.MAX) { toast("Enter an amount, e.g. 25000"); return }
        withPermissions { Bridge.sendPayment(amount, "Test", badTag = bad) }
    }

    private fun toast(s: String) = Toast.makeText(this, s, Toast.LENGTH_SHORT).show()

    private fun renderState() {
        val ble = Bridge.ble
        val addr = prefs.deviceAddress
        val conn = when (ble.state) {
            SoundboxBle.State.IDLE -> "not connected"
            SoundboxBle.State.CONNECTING -> "connecting..."
            SoundboxBle.State.READY -> "connected"
        }
        txtState.text = if (addr == null) "No soundbox chosen"
        else "$addr  -  $conn" + (if (ble.boxStatus.isNotEmpty()) "\nStatus: ${ble.boxStatus}" else "")

        txtNotif.text = when {
            !notificationAccessOn() -> "Notification access: off (needed to read WhatsApp)"
            PaymentListener.connected -> "Notification access: on, listening to WhatsApp"
            else -> "Notification access: on, but not connected: turn it off and on again"
        }

        val atBottom = !scroll.canScrollVertically(1)
        txtLog.text = Bridge.logText()
        if (atBottom) scroll.post { scroll.fullScroll(ScrollView.FOCUS_DOWN) }
    }

    // ---------- scan ----------

    @SuppressLint("MissingPermission")   // granted by withPermissions()
    private fun scanForBox() {
        val adapter = getSystemService(BluetoothManager::class.java).adapter
        val scanner = adapter?.bluetoothLeScanner
        if (adapter == null || !adapter.isEnabled || scanner == null) { toast("Turn Bluetooth on"); return }

        val found = LinkedHashMap<String, String>()
        val list = ArrayAdapter<String>(this, android.R.layout.simple_list_item_1)
        val callback = object : ScanCallback() {
            override fun onScanResult(callbackType: Int, r: ScanResult) {
                val a = r.device.address
                if (a in found) return
                val name = r.scanRecord?.deviceName ?: "QRIS-Soundbox"
                found[a] = "$name\n$a   ${r.rssi} dBm"
                list.add(found[a])
            }

            override fun onScanFailed(errorCode: Int) = toast("Scan failed ($errorCode)")
        }
        val filter = ScanFilter.Builder().setServiceUuid(ParcelUuid(SoundboxBle.SERVICE)).build()
        val settings = ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build()
        scanner.startScan(listOf(filter), settings, callback)

        AlertDialog.Builder(this)
            .setTitle("Searching for soundboxes...")
            .setAdapter(list) { _, which ->
                val addr = found.keys.elementAt(which)
                prefs.deviceAddress = addr
                Bridge.log("Chosen soundbox $addr")
                Bridge.connect()
                renderState()
            }
            .setNegativeButton("Cancel", null)
            .setOnDismissListener { runCatching { scanner.stopScan(callback) } }
            .show()
    }
}
