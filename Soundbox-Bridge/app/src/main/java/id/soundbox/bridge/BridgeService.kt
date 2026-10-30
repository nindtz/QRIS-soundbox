package id.soundbox.bridge

import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat

/** Foreground service: keeps the app (and its BLE link to the box) alive in the background. */
class BridgeService : Service() {

    companion object {
        private const val CHANNEL = "bridge"
        private const val NOTIF_ID = 1

        fun start(ctx: Context) {
            try {
                ctx.startForegroundService(Intent(ctx, BridgeService::class.java))
            } catch (e: Exception) {
                Bridge.log("Could not start the background service: ${e.message}")
            }
        }

        fun stop(ctx: Context) = ctx.stopService(Intent(ctx, BridgeService::class.java))
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(
            NotificationChannel(CHANNEL, "Bridge running", NotificationManager.IMPORTANCE_LOW)
        )
        val open = PendingIntent.getActivity(
            this, 0, Intent(this, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE
        )
        val notification = NotificationCompat.Builder(this, CHANNEL)
            .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
            .setContentTitle("Soundbox bridge running")
            .setContentText("Forwarding WhatsApp payments to the soundbox")
            .setOngoing(true)
            .setContentIntent(open)
            .build()
        try {
            val type = if (Build.VERSION.SDK_INT >= 29) ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE else 0
            ServiceCompat.startForeground(this, NOTIF_ID, notification, type)
        } catch (e: Exception) {
            Bridge.log("Background service failed: ${e.message}")
            stopSelf()
            return START_NOT_STICKY
        }
        Bridge.log("Bridge running in the background")
        Bridge.connect()
        return START_STICKY
    }

    override fun onDestroy() {
        Bridge.log("Bridge stopped")
        super.onDestroy()
    }
}
