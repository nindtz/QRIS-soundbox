package id.soundbox.bridge

/**
 * Finds the amount in a message, for testing with WhatsApp:
 *   "Rp 25.000"  "Rp25000"  "Pembayaran Rp 1.250.000,00 diterima"  "Rp. 150,000"
 *   or a message that is only a number: "25000"
 * Indonesian style: '.' or ',' as thousands separator, optional ",00" / ".00" cents (dropped).
 */
object AmountParser {
    private val RP = Regex(
        """(?i)(?<![A-Za-z])Rp\.?\s*([0-9]{1,3}(?:[.,][0-9]{3})+|[0-9]+)(?:[.,][0-9]{1,2})?(?![0-9])"""
    )
    private val ONLY_NUMBER = Regex("""^\s*([0-9]{1,9})\s*$""")

    const val MAX = 999_999_999L          // the box can say up to 999.999.999

    fun parse(text: String): Long? {
        val digits = RP.find(text)?.groupValues?.get(1)
            ?: ONLY_NUMBER.find(text)?.groupValues?.get(1)
            ?: return null
        val v = digits.filter { it.isDigit() }.toLongOrNull() ?: return null
        return if (v in 1..MAX) v else null
    }

    fun format(v: Long): String = "Rp " + "%,d".format(java.util.Locale.US, v).replace(',', '.')
}
