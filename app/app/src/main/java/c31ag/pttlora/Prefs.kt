package c31ag.pttlora

import android.content.Context

/** Ajustes de la estación. Deliberadamente pocos. */
class Prefs(ctx: Context) {

    private val p = ctx.getSharedPreferences("pttlora", Context.MODE_PRIVATE)

    /** Indicativo. Sin él no se transmite: es identificación de estación, no
     *  un apodo. Viaja en claro en cada INICIO y en la baliza. */
    var indicativo: String
        get() = p.getString("indicativo", "") ?: ""
        set(v) = p.edit().putString("indicativo", v.uppercase().take(12)).apply()

    /** MAC del nodo Bluetooth elegido. */
    var mac: String
        get() = p.getString("mac", "") ?: ""
        set(v) = p.edit().putString("mac", v).apply()

    /** Por dónde se habla con el nodo.
     *
     *  Bluetooth: un usuario por nodo, sin necesidad de red ninguna.
     *  WiFi: hasta ocho a la vez, cada uno con su indicativo — el nodo puede
     *  levantar su propio punto de acceso, así que tampoco hace falta
     *  infraestructura. Se elige según el caso, no hay uno mejor. */
    var porWifi: Boolean
        get() = p.getBoolean("porWifi", false)
        set(v) = p.edit().putBoolean("porWifi", v).apply()

    /** Dirección del nodo cuando se habla por WiFi. Si el nodo es su propio
     *  punto de acceso, siempre es 192.168.4.1 (la fija el ESP32). */
    var host: String
        get() = p.getString("host", Nodo.IP_AP) ?: Nodo.IP_AP
        set(v) = p.edit().putString("host", v.trim()).apply()

    var puerto: Int

        get() = p.getInt("puerto", Nodo.PUERTO_APP)
        set(v) = p.edit().putInt("puerto", v.coerceIn(1, 65535)).apply()

    /** CAMINO DE DATOS: el segundo tubo, por Internet.
     *
     *  No sustituye a la radio — la complementa. La radio es la base y esto es
     *  el salvavidas: cuando el nodo no alcanza a nadie, lo que se diga sigue
     *  llegando por aquí. Se puede apagar porque hay quien no quiere que su voz
     *  salga a Internet, y eso es una decisión suya, no nuestra.
     *
     *  Al otro lado hay un `nododatos.py`, que para la app es un nodo más:
     *  habla el mismo protocolo de cliente por TCP. */
    var datosActivo: Boolean
        get() = p.getBoolean("datos_activo", false)
        set(v) { p.edit().putBoolean("datos_activo", v).apply() }

    var datosHost: String
        get() = p.getString("datos_host", "or.adan.ovh") ?: "or.adan.ovh"
        set(v) { p.edit().putString("datos_host", v.trim()).apply() }

    var datosPuerto: Int
        get() = p.getInt("datos_puerto", 4460)
        set(v) { p.edit().putInt("datos_puerto", v).apply() }

    /** ¿Se le pasa al nodo la posición del móvil, para que la baliza la lleve?
     *
     *  **Apagada de fábrica, y a propósito.** La posición por RF va EN CLARO y
     *  la repite media red: no es un dato que se pueda activar por descuido.
     *  Encenderla es una decisión explícita, y apagarla vuelve a ser de verdad
     *  —se le manda al nodo que la olvide, no basta con dejar de refrescarla—.
     *
     *  Una CELDA es otra cosa: su posición se teclea una vez y se guarda en el
     *  propio nodo (`pos` fija en NVS), porque quien pone infraestructura de la
     *  que otros dependen dice dónde está. Esto de aquí es la del móvil. */
    var posActiva: Boolean
        get() = p.getBoolean("pos_activa", false)
        set(v) { p.edit().putBoolean("pos_activa", v).apply() }

    var nombreNodo: String
        get() = p.getString("nodo", "") ?: ""
        set(v) = p.edit().putString("nodo", v).apply()

    var canal: Int
        get() = p.getInt("canal", 1)
        set(v) = p.edit().putInt("canal", v.coerceIn(0, 255)).apply()

    /** Saltos de la malla. **No poner 3 por costumbre**: mientras un nodo
     *  repite está sordo, y con dos estaciones repetir solo hace perder lotes.
     *  El nodo se calla solo si no hay a quién repetir, pero el valor debe
     *  seguir siendo el que la red necesita. */
    var saltos: Int
        get() = p.getInt("saltos", 3)
        set(v) = p.edit().putInt("saltos", v.coerceIn(0, 7)).apply()

    /** dBm. 17 es el máximo de estas placas; 2 para pruebas de mesa. */
    var potencia: Int
        get() = p.getInt("potencia", 17)
        set(v) = p.edit().putInt("potencia", v.coerceIn(2, 17)).apply()

    /** Modo de Codec2 (índice de Codec2.NOMBRES). 1200 por defecto: es el
     *  punto dulce entre inteligibilidad y ocupación del canal. */
    var modo: Int
        get() = p.getInt("modo", Codec2.C2_1200)
        set(v) = p.edit().putInt("modo", v).apply()

    /** Papel del nodo. Por defecto **automático**: hace de puente y de
     *  repetidor, y se calla la repetición solo si no hay a quién repetir.
     *  Que funcione bien sin tocar nada es el requisito, no un extra. */
    var perfil: Int
        get() = p.getInt("perfil", PERFIL_AUTO)
        set(v) = p.edit().putInt("perfil", v.coerceIn(0, 2)).apply()

    /** Frecuencia en kHz. 434.400 por convivencia con el LoRa APRS de 433.775
     *  y lejos de la basura ISM de 433.92. Es provisional: antes de proponerlo
     *  como canal de comunidad hay que contrastarlo con el bandplan de la IARU. */
    var frecuenciaKHz: Int
        get() = p.getInt("frecKHz", 434400)
        set(v) = p.edit().putInt("frecKHz", v).apply()

    /** SPREADING FACTOR y ANCHO DE BANDA. Estaban fijos en el código (7 y 250)
     *  y eso hacía imposible cambiarlos de verdad: el nodo los aceptaba, pero
     *  la app se los volvía a poner en la siguiente conexión.
     *
     *  **Son parámetros de CANAL, no de estación**: dos nodos con SF distinto
     *  no se oyen peor, no se oyen. Cambiarlos obliga a cambiarlos en TODOS.
     *  Cada punto de SF son unos 3 dB de sensibilidad —del orden de un 40 % más
     *  de alcance— a cambio del doble de tiempo en el aire, así que también son
     *  la mitad de sitio para repetidores. */
    var sf: Int
        get() = p.getInt("sf", 8)
        set(v) = p.edit().putInt("sf", v.coerceIn(6, 12)).apply()

    var anchoKHz: Int
        get() = p.getInt("bwKHz", 250)
        set(v) = p.edit().putInt("bwKHz", v).apply()

    /** Canal lógico dentro de la misma frecuencia: separa grupos sin necesidad
     *  de mover la radio. */
    var canalLogico: Int
        get() = p.getInt("canalLog", 1)
        set(v) = p.edit().putInt("canalLog", v.coerceIn(0, 255)).apply()

    /** WiFi del nodo. Solo sirve para poder actualizarle el firmware sin
     *  bajarlo del sitio donde esté: **nunca es imprescindible**. Ojo, el
     *  ESP32 es solo 2,4 GHz — una red de 5 GHz no le vale. */
    var wifiSsid: String
        get() = p.getString("wifiSsid", "") ?: ""
        set(v) = p.edit().putString("wifiSsid", v).apply()

    var wifiClave: String
        get() = p.getString("wifiClave", "") ?: ""
        set(v) = p.edit().putString("wifiClave", v).apply()

    /** Enlaces del nodo con otros nodos por Internet. El segundo es el
     *  respaldo del primero, no un segundo camino: el nodo solo lo usa si el
     *  principal no responde. */
    var enlace1: String
        get() = p.getString("enlace1", "") ?: ""
        set(v) = p.edit().putString("enlace1", v.trim()).apply()

    var enlace2: String
        get() = p.getString("enlace2", "") ?: ""
        set(v) = p.edit().putString("enlace2", v.trim()).apply()

    /** Ganancia de reproducción, índice de AudioEngine.GANANCIAS. */
    var ganancia: Int
        get() = p.getInt("ganancia", 1)
        set(v) = p.edit().putInt("ganancia", v).apply()

    val listo: Boolean get() = indicativo.isNotBlank() &&
        (if (porWifi) host.isNotBlank() else mac.isNotBlank())

    companion object {
        const val PERFIL_AUTO = 0
        const val PERFIL_FIJO = 1
        const val PERFIL_SOLO = 2

        /** Frecuencias sugeridas, en kHz. Separadas 250 kHz para que no se
         *  pisen con BW de 250: dos canales mas juntos se estorbarian. */
        val CANALES = intArrayOf(434400, 434650, 434150, 433400)

        val PERFILES = arrayOf(
            "Automático (recomendado)",
            "Repetidor fijo",
            "Solo mi radio")

        val PERFILES_AYUDA = arrayOf(
            "Hace de radio para el móvil y además repite a los demás, " +
            "pero solo cuando hay alguien a quien repetir.",
            "Para un nodo instalado en alto sin móvil: repite siempre, " +
            "aunque solo oiga a una estación. El Bluetooth se apaga a los " +
            "5 minutos de encenderlo para ahorrar; reinícialo si necesitas " +
            "volver a configurarlo.",
            "Solo mi radio: no repite a nadie.")
    }
}
