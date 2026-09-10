package c31ag.pttlora

import android.content.Context
import org.json.JSONObject
import java.io.File
import java.net.URL
import java.util.concurrent.ConcurrentHashMap
import javax.net.ssl.HttpsURLConnection
import kotlin.concurrent.thread

/**
 * De indicativo a NOMBRE, contra radioid.net.
 *
 * POR QUE
 * -------
 * En una rueda con varias personas, un nombre se reconoce mucho más rápido que
 * un distintivo. `EA3XYZ · Marc` se lee de un vistazo; `EA3XYZ` hay que
 * traducirlo mentalmente. La idea está copiada de la app de RadioVampiros, que
 * hace lo mismo con IDs de DMR — allí hace más falta todavía, porque lo que
 * viaja es un número.
 *
 * CON CACHÉ EN DISCO, y eso no es un detalle: **esta app se usa donde no hay
 * cobertura**. Se consulta una vez, con red, y a partir de ahí el nombre sale
 * en el monte igual que en casa. Sin red y sin caché, simplemente se queda el
 * indicativo a secas, que es lo que había antes.
 *
 * Lo que se manda fuera es un indicativo, que es público por definición —va en
 * cada transmisión, en claro, por el aire— así que no revela nada que no esté
 * ya emitiéndose. Aun así se puede apagar: ver `Prefs.nombresActivo`.
 */
object Nombres {
    private val cache = ConcurrentHashMap<String, String>()   // indicativo -> nombre ("" = no existe)
    private val pedidos = ConcurrentHashMap<String, Boolean>()
    private var fichero: File? = null
    /** Se llama cuando llega un nombre nuevo, para repintar. */
    @Volatile var alLlegar: (() -> Unit)? = null

    fun arranca(ctx: Context) {
        if (fichero != null) return
        val f = File(ctx.filesDir, "nombres.json")
        fichero = f
        if (!f.exists()) return
        try {
            val j = JSONObject(f.readText())
            for (k in j.keys()) cache[k] = j.getString(k)
        } catch (_: Exception) { /* caché corrupta: se rehace sola */ }
    }

    private fun guarda() {
        val f = fichero ?: return
        try {
            val j = JSONObject()
            for ((k, v) in cache) j.put(k, v)
            f.writeText(j.toString())
        } catch (_: Exception) {}
    }

    /**
     * El nombre si se conoce, o null. **Nunca bloquea**: si no está, dispara la
     * consulta en segundo plano y devuelve null; cuando llegue se avisa por
     * `alLlegar` y quien pinte volverá a preguntar.
     */
    fun de(indicativo: String, activo: Boolean): String? {
        val ind = indicativo.trim().uppercase().substringBefore(' ')
        if (ind.isEmpty() || ind == "NOCALL") return null
        cache[ind]?.let { return it.ifEmpty { null } }
        if (!activo) return null
        if (pedidos.putIfAbsent(ind, true) != null) return null
        thread(isDaemon = true) {
            var nombre = ""
            try {
                val u = URL("https://radioid.net/api/dmr/user/?callsign=$ind")
                val c = u.openConnection() as HttpsURLConnection
                c.connectTimeout = 6000; c.readTimeout = 6000
                c.setRequestProperty("User-Agent", "pttlora")
                val t = c.inputStream.bufferedReader().use { it.readText() }
                val r = JSONObject(t).optJSONArray("results")
                if (r != null && r.length() > 0) {
                    val o = r.getJSONObject(0)
                    // Sólo el nombre de pila: en una lista estrecha, el apellido
                    // no cabe y tampoco añade nada para reconocer a alguien.
                    nombre = o.optString("fname", "").trim().split(" ").firstOrNull().orEmpty()
                }
            } catch (_: Exception) {
                // Sin red o servicio caído: NO se cachea el fallo, para que se
                // vuelva a intentar cuando haya cobertura.
                pedidos.remove(ind)
                return@thread
            }
            cache[ind] = nombre       // "" también se cachea: no existe, no insistir
            guarda()
            if (nombre.isNotEmpty()) alLlegar?.invoke()
        }
        return null
    }

    fun olvida() {
        cache.clear(); pedidos.clear(); guarda()
    }
}
