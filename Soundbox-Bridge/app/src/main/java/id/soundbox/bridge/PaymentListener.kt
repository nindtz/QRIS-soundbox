package id.soundbox.bridge

import android.app.Notification
import android.service.notification.NotificationListenerService
import android.service.notification.StatusBarNotification
import androidx.core.app.NotificationCompat

/**
 * Reads WhatsApp notifications and hands new messages to [Bridge].
 *
 * WhatsApp re-posts the same notification with every new message in a chat (MessagingStyle
 * holds the recent messages), so each message is remembered by chat + timestamp + text and
 * forwarded once. Messages from before the listener started, or older than 2 minutes, are skipped.
 */
class PaymentListener : NotificationListenerService() {

    companion object {
        private val PACKAGES = setOf("com.whatsapp", "com.whatsapp.w4b")   // WhatsApp, WhatsApp Business
        private const val MAX_AGE_MS = 2 * 60_000L
        private const val CLOCK_SLACK_MS = 10_000L

        @Volatile var connected = false
            private set
    }

    private val startedAt = System.currentTimeMillis()
    private val seen = object : LinkedHashMap<String, Boolean>(64, 0.75f, false) {
        override fun removeEldestEntry(eldest: MutableMap.MutableEntry<String, Boolean>?) = size > 300
    }

    override fun onListenerConnected() {
        connected = true
        Bridge.log("Notification access active: listening to WhatsApp")
    }

    override fun onListenerDisconnected() {
        connected = false
        Bridge.log("Notification access stopped")
    }

    override fun onNotificationPosted(sbn: StatusBarNotification) {
        if (sbn.packageName !in PACKAGES) return
        val n = sbn.notification
        if (n.flags and Notification.FLAG_GROUP_SUMMARY != 0) return     // "3 messages from 2 chats"
        val prefs = Bridge.prefs
        if (!prefs.forwardEnabled) { Bridge.log("WhatsApp notification ignored: forwarding is switched off"); return }

        val extras = n.extras
        val style = NotificationCompat.MessagingStyle.extractMessagingStyleFromNotification(n)
        val chat = (style?.conversationTitle ?: extras.getCharSequence(Notification.EXTRA_TITLE))
            ?.toString().orEmpty()

        // (sender, text, time)
        val messages: List<Triple<String, String, Long>> =
            if (style != null && style.messages.isNotEmpty()) {
                style.messages.map { m ->
                    Triple(m.person?.name?.toString() ?: chat, m.text?.toString().orEmpty(), m.timestamp)
                }
            } else {
                listOf(Triple(chat, extras.getCharSequence(Notification.EXTRA_TEXT)?.toString().orEmpty(), sbn.postTime))
            }

        val now = System.currentTimeMillis()
        val filter = prefs.chatFilter.trim()
        var fresh = 0
        for ((sender, text, time) in messages) {
            if (text.isBlank()) continue
            val key = "${sbn.key}|$time|$text"
            if (seen.containsKey(key)) continue                // already handled (WhatsApp re-posts)
            seen[key] = true
            fresh++
            val who = sender.ifBlank { chat }
            if (time < startedAt - CLOCK_SLACK_MS || now - time > MAX_AGE_MS) {
                Bridge.log("WhatsApp $who: skipped, message is older than 2 min or from before the app started")
                continue
            }
            if (filter.isNotEmpty() && !chat.contains(filter, ignoreCase = true) &&
                !sender.contains(filter, ignoreCase = true)) {
                Bridge.log("WhatsApp $who: skipped, chat/sender doesn't contain \"$filter\"")
                continue
            }
            Bridge.onWhatsAppMessage(who, text)
        }
        if (fresh == 0 && messages.none { it.second.isNotBlank() })
            Bridge.log("WhatsApp notification without message text (is the preview hidden?)")
    }
}
