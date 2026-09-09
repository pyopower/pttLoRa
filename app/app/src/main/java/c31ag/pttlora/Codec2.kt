package c31ag.pttlora

/**
 * Codec2 por JNI.
 *
 * El modo NO es una preferencia local: viaja en cada transmisión, así que dos
 * estaciones con distinto ajuste se entienden — el receptor abre el códec que
 * diga el emisor. Por eso aquí se crean instancias sueltas y no un singleton.
 */
class Codec2 private constructor(val modo: Int, private var h: Long) {

    val muestrasPorTrama: Int = muestrasPorTrama(h)
    val bytesPorTrama: Int = bytesPorTrama(h)

    /** Tramas de códec que caben en un lote de 480 ms. */
    val tramasPorLote: Int =
        if (muestrasPorTrama > 0) (LOTE_MS * 8) / muestrasPorTrama else 0

    fun codifica(pcm: ShortArray, off: Int, out: ByteArray, outOff: Int): Int =
        if (h == 0L) 0 else codificar(h, pcm, off, out, outOff)

    fun descodifica(bits: ByteArray, inOff: Int, pcm: ShortArray, off: Int): Int =
        if (h == 0L) 0 else descodificar(h, bits, inOff, pcm, off)

    fun cierra() {
        if (h != 0L) { destruir(h); h = 0 }
    }

    companion object {
        /** Audio por paquete de LoRa. Ver el README: es lo que hace que el
         *  sobre de cabecera no se coma el canal. */
        const val LOTE_MS = 480

        // Códigos que viajan en la trama. Coinciden con protocolo.h del nodo.
        const val C2_3200 = 0
        const val C2_2400 = 1
        const val C2_1600 = 2
        const val C2_1300 = 3
        const val C2_1200 = 4
        const val C2_700C = 5

        /** Nombres para la interfaz, en el mismo orden que los códigos. */
        val NOMBRES = arrayOf("3200", "2400", "1600", "1300", "1200", "700C")

        // Valores reales de codec2.h. OJO: NO son consecutivos — entre 1600 y
        // 1300 esta el 1400, y despues del 1200 hay un hueco hasta el 700C.
        // Confundirlos no da error: abre otro codec, con otro tamaño de trama,
        // y el audio sale irreconocible sin que nada se queje.
        //   3200=0  2400=1  1600=2  1400=3  1300=4  1200=5  700C=8
        private val NATIVO = intArrayOf(0, 1, 2, 4, 5, 8)

        @Volatile var disponible = false; private set

        init {
            disponible = try { System.loadLibrary("pttlora"); true }
            catch (e: Throwable) { false }
        }

        /** Devuelve null si la librería nativa no cargó o el modo no existe. */
        fun abre(modo: Int): Codec2? {
            if (!disponible || modo < 0 || modo >= NATIVO.size) return null
            val h = crear(NATIVO[modo])
            if (h == 0L) return null
            val c = Codec2(modo, h)
            return if (c.muestrasPorTrama > 0 && c.bytesPorTrama > 0) c
                   else { c.cierra(); null }
        }

        @JvmStatic private external fun crear(modo: Int): Long
        @JvmStatic private external fun destruir(h: Long)
        @JvmStatic private external fun muestrasPorTrama(h: Long): Int
        @JvmStatic private external fun bytesPorTrama(h: Long): Int
        @JvmStatic private external fun codificar(h: Long, pcm: ShortArray, off: Int,
                                                  out: ByteArray, outOff: Int): Int
        @JvmStatic private external fun descodificar(h: Long, bits: ByteArray, inOff: Int,
                                                     pcm: ShortArray, off: Int): Int
    }
}
