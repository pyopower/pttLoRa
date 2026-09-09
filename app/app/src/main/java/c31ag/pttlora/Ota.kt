package c31ag.pttlora

import android.util.Log
import java.security.MessageDigest
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.TimeUnit

/**
 * Actualiza el firmware del nodo **desde el móvil**, por BLE o por WiFi.
 *
 * ## Para qué
 *
 * Para no depender de un cable. Un nodo cliente lo lleva alguien en el bolsillo
 * y una celda puede estar en un tejado: en los dos casos, tener que enchufar un
 * USB para cambiar el firmware es lo que hace que no se actualice nunca.
 *
 * Con el MTU de 517 que negocia un móvil moderno, un firmware de 1,1 MB tarda
 * unos dos o tres minutos. Desde la Raspberry, que se queda en MTU 23, tardaba
 * media hora — así que **este es el camino bueno**.
 *
 * ## Las tres lecciones que están metidas aquí desde el principio
 *
 * Las tres costaron una mañana entera el 8-sep-2026 y las tres son invisibles
 * si no se conocen:
 *
 *  1. **Solo vale el acuse que corresponde al trozo que acabas de mandar**
 *     (`siguiente > idx`). Si se acepta cualquier acuse, basta UN reenvío para
 *     que esto no se recupere nunca: el nodo acaba acusando los dos envíos, y a
 *     partir de ahí se manda el trozo N y se lee el acuse del N-1, se manda el
 *     N+1 y se lee el del N... **siempre un paso por detrás, sin avanzar y sin
 *     dar ningún error.** El síntoma es "0%, 0 reenvíos" durante diez minutos.
 *
 *  2. **El plazo del acuse tiene que ser largo.** Borrar un sector de flash del
 *     ESP32 tarda casi dos segundos (medido: 1,78 s en el primer trozo). Con
 *     tres segundos de plazo, un borrado lento provoca un reenvío — y por lo de
 *     arriba, un reenvío lo rompe todo. Diez segundos.
 *
 *  3. **Se escribe CON respuesta.** Escribir sin respuesta no tiene control de
 *     flujo: lo que no cabe se tira y nadie se entera. En audio eso es perder
 *     480 ms; en una actualización, corromperla.
 */
class Ota(private val nodo: Nodo,
          private val alAvanzar: (Int, String) -> Unit) {

    companion object {
        private const val TAG = "pttlora.ota"

        const val CMD_OTA_INI = 0x0B
        const val CMD_OTA_DAT = 0x0C
        const val CMD_OTA_FIN = 0x0D
        const val EV_OTA = 0x88

        /** 512 B por trozo. Cabe de sobra en el buffer del nodo (1100 B) aun
         *  con todos los bytes escapados, y con MTU 517 sale en dos o tres
         *  paquetes BLE. */
        const val TROZO = 512
        /** Ver la lección 2. */
        const val PLAZO_ACUSE_MS = 10_000L
    }

    /** Acuses que llegan del nodo: (porcentaje, próximo trozo esperado). */
    private val acuses = ArrayBlockingQueue<Pair<Int, Int>>(64)
    private val avisos = ArrayBlockingQueue<String>(32)
    @Volatile private var cancelado = false

    /** Lo llama el servicio con cada evento del nodo mientras dura la subida. */
    fun alEvento(tipo: Int, datos: ByteArray) {
        when (tipo) {
            EV_OTA -> if (datos.size >= 3) {
                val pct = datos[0].toInt() and 0xFF
                val sig = ((datos[1].toInt() and 0xFF) shl 8) or (datos[2].toInt() and 0xFF)
                acuses.offer(pct to sig)
            }
            Nodo.EV_LOG -> avisos.offer(String(datos, Charsets.UTF_8))
        }
    }

    fun cancela() { cancelado = true }

    /**
     * Sube el firmware. **Bloquea**: llamar desde un hilo aparte.
     * Devuelve null si fue bien, o el motivo del fallo.
     */
    fun sube(binario: ByteArray): String? {
        if (binario.size < 65536) return "eso no parece un firmware (${binario.size} B)"
        val md5 = MessageDigest.getInstance("MD5").digest(binario)
            .joinToString("") { "%02x".format(it) }
        Log.i(TAG, "firmware ${binario.size} B, md5 $md5")

        acuses.clear(); avisos.clear()
        cancelado = false

        // Arranque: tamaño (4 B, big endian) + md5 en ASCII.
        val cab = byteArrayOf(
            (binario.size ushr 24).toByte(), (binario.size ushr 16).toByte(),
            (binario.size ushr 8).toByte(), binario.size.toByte()) +
            md5.toByteArray(Charsets.US_ASCII)
        alAvanzar(0, "Preparando el nodo…")
        nodo.manda(CMD_OTA_INI, cab)

        /* El nodo contesta con avisos de texto, y hay que ESPERAR AL BUENO, no
           quedarse con el primero: antes de "iniciada" puede mandar otros (por
           ejemplo "habia una actualizacion a medias: se descarta"), que son
           informativos y no un fallo. Quedarse con el primero daba un "el nodo
           no aceptó" mentiroso. */
        var arrancada = false
        var ultimo = ""
        val limite = System.currentTimeMillis() + 10_000
        while (System.currentTimeMillis() < limite) {
            val a = avisos.poll(500, TimeUnit.MILLISECONDS) ?: continue
            ultimo = a
            Log.i(TAG, "nodo: $a")
            if (a.contains("iniciada")) { arrancada = true; break }
            // Lo que sí es un no rotundo: ver CMD_OTA_INI en el firmware.
            if (a.contains("no cabe") || a.contains("mal formado") ||
                a.contains("mientras se transmite")) return "el nodo no aceptó: $a"
        }
        if (!arrancada)
            return if (ultimo.isBlank()) "el nodo no contestó al empezar"
                   else "el nodo no aceptó: $ultimo"

        val trozos = (binario.indices step TROZO).map {
            binario.copyOfRange(it, minOf(it + TROZO, binario.size))
        }
        var idx = 0
        var reenvios = 0
        val t0 = System.currentTimeMillis()

        while (idx < trozos.size) {
            if (cancelado) return "cancelado"
            nodo.manda(CMD_OTA_DAT,
                byteArrayOf((idx ushr 8).toByte(), idx.toByte()) + trozos[idx])

            /* LECCIÓN 1: solo vale el acuse de ESTE trozo. Los atrasados se
               tiran sin contemplaciones; aceptarlos es lo que desincroniza el
               protocolo para siempre. */
            var siguiente = -1
            val hasta = System.currentTimeMillis() + PLAZO_ACUSE_MS
            while (System.currentTimeMillis() < hasta) {
                val a = acuses.poll(500, TimeUnit.MILLISECONDS) ?: continue
                if (a.second <= idx) continue          // atrasado
                siguiente = a.second
                val seg = (System.currentTimeMillis() - t0) / 1000.0
                val kbs = if (seg > 0) idx * TROZO / 1024.0 / seg else 0.0
                alAvanzar(a.first, "%d%% · %.0f kB/s".format(a.first, kbs))
                break
            }
            if (siguiente < 0) {
                reenvios++
                if (reenvios > 20) return "el nodo dejó de contestar en el trozo $idx"
                continue                                // el mismo otra vez
            }
            idx = siguiente
        }

        alAvanzar(100, "Verificando…")
        nodo.manda(CMD_OTA_FIN)
        /* El nodo comprueba el md5 y se reinicia. Que se reinicie es la señal de
           que fue bien: el enlace se cae, y la app reconecta sola. */
        /* Dos respuestas posibles, y las dos son literales del firmware
           (`CMD_OTA_FIN` en main.cpp):
             "actualizacion correcta; reiniciando"     -> bien
             "actualizacion rechazada: error N"        -> el md5 no cuadra, y el
                                                          firmware viejo sigue
                                                          intacto.
           Se buscan esas dos y no se adivina por palabras sueltas. */
        val limiteFin = System.currentTimeMillis() + 25_000
        while (System.currentTimeMillis() < limiteFin) {
            val a = avisos.poll(500, TimeUnit.MILLISECONDS) ?: continue
            Log.i(TAG, "fin: $a")
            if (a.contains("correcta")) return null
            if (a.contains("rechazada")) return "el nodo la rechazó: $a"
        }
        /* Sin respuesta puede significar que salió bien: el nodo se reinicia
           400 ms después de decirlo y el enlace se cae, así que el aviso puede
           perderse por el camino. No se da por fallida. */
        return null
    }
}
