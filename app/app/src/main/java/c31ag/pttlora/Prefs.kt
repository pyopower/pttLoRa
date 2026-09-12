package c31ag.pttlora

import android.content.Context

/** Ajustes de la estación. Deliberadamente pocos. */
class Prefs(ctx: Context) {

    /* ------------------------------------------------------------------
     *  LOS VALORES DE FABRICA, EN UN SOLO SITIO.
     *
     *  Estaban repartidos por el codigo como numeros sueltos, y eso ya costo
     *  un fallo silencioso: **el SF por defecto era 8 y la red va en 7**. Una
     *  instalacion nueva, en cuanto alguien entrara en Radio y pulsara
     *  Aplicar, le metia SF8 al nodo y lo sacaba de la red — sin error, sin
     *  aviso, y con el sintoma de "de repente no oigo a nadie".
     *
     *  Ademas hace falta poder ENSEÑARLOS: quien toca un ajuste tiene derecho a
     *  saber cual era el de antes, y a volver a el. */
    object PorDefecto {
        const val FREC_KHZ = 439600
        const val SF = 7
        const val ANCHO_KHZ = 250
        const val CR = 5
        const val POTENCIA = 17
        const val CANAL = 1
        const val MODO = Codec2.C2_1200
        const val ENLACE1 = "or.adan.ovh:4461"
        const val ENLACE2 = "urf.adan.ovh:4461"
        const val DATOS_HOST = "or.adan.ovh"
        const val DATOS_PUERTO = 4460

        /** Para enseñarlo al lado de cada campo. */
        fun frecMHz() = "%.3f".format(FREC_KHZ / 1000.0)
    }

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
        get() = p.getString("datos_host", PorDefecto.DATOS_HOST) ?: PorDefecto.DATOS_HOST
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

    /** Aspecto: 0 = como el sistema, 1 = claro, 2 = oscuro. Por defecto sigue
     *  al sistema, que es lo que espera casi todo el mundo. */
    var tema: Int
        get() = p.getInt("tema", 0)
        set(v) { p.edit().putInt("tema", v).apply() }

    /** ¿Se consulta el nombre del operador en radioid.net? Ver `Nombres`.
     *  Encendido de fábrica: lo que se consulta es un indicativo, que ya va en
     *  claro por el aire en cada transmisión. */
    var nombresActivo: Boolean
        get() = p.getBoolean("nombres", true)
        set(v) { p.edit().putBoolean("nombres", v).apply() }

    var nombreNodo: String
        get() = p.getString("nodo", "") ?: ""
        set(v) = p.edit().putString("nodo", v).apply()

    /** ¿Está abierto el panel de abajo (estado, último aviso y Ajustes)? Se
     *  recuerda: quien lo pliega para dejarle sitio a la rueda no quiere
     *  volver a plegarlo en cada arranque. */
    var panelAbierto: Boolean
        get() = p.getBoolean("panel_abierto", true)
        set(v) = p.edit().putBoolean("panel_abierto", v).apply()

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
        get() = p.getInt("potencia", PorDefecto.POTENCIA)
        set(v) = p.edit().putInt("potencia", v.coerceIn(2, 17)).apply()

    /** CUANTO SE ESPERA ANTES DE REPRODUCIR, EN LOTES.
     *
     *  0 = directo: cada lote suena en cuanto llega. Sin reordenar y sin tapar
     *      huecos, que es como funcionaba la app hasta la 0.9.36. Es la salida
     *      de emergencia: si algo del colchón se tuerce, aquí se vuelve a un
     *      comportamiento conocido sin esperar a una versión nueva.
     *  1 = 480 ms. Tapa un lote perdido suelto y deja que la copia de radio
     *      llegue a tiempo de ganarle a la de Internet.
     *  2 = 960 ms. Aguanta además el jitter de un salto de repetidor.
     *
     *  Por defecto **1**: dos lotes fue lo primero que se probó y el segundo de
     *  latencia se nota al hablar. Con uno se conserva casi todo el beneficio
     *  por la mitad de precio. */
    var retrasoLotes: Int
        get() = p.getInt("retraso_lotes", 1)
        set(v) = p.edit().putInt("retraso_lotes", v.coerceIn(0, 2)).apply()

    /** Modo de Codec2 (índice de Codec2.NOMBRES). 1200 por defecto: es el
     *  punto dulce entre inteligibilidad y ocupación del canal. */
    var modo: Int
        get() = p.getInt("modo", PorDefecto.MODO)
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
    /** El canal, en kHz. **439.600 desde la 0.9.27**: 434.400 caía en un tramo
     *  del plan IARU R1 con un máximo de 12 kHz de ancho de banda, y un canal
     *  LoRa son 250.
     *
     *  ⚠️ Y CON MIGRACIÓN, que si no el cambio no sirve de nada: quien ya tenga
     *  la app guarda 434400 en sus ajustes, el valor por defecto nuevo no le
     *  llega nunca, y en cuanto entre en Radio y pulse Guardar **le devuelve el
     *  canal viejo al nodo**. Se reconoce por el valor exacto del antiguo
     *  defecto: si alguien lo había cambiado a mano a otra cosa, se le respeta. */
    var frecuenciaKHz: Int
        get() {
            val v = p.getInt("frecKHz", PorDefecto.FREC_KHZ)
            if (v == 434400) { frecuenciaKHz = 439600; return 439600 }
            return v
        }
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
        get() = p.getInt("sf", PorDefecto.SF)
        set(v) = p.edit().putInt("sf", v.coerceIn(6, 12)).apply()

    var anchoKHz: Int
        get() = p.getInt("bwKHz", PorDefecto.ANCHO_KHZ)
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

    /** GANANCIA DEL MICRÓFONO, índice de AudioEngine.MIC_GANANCIAS.
     *
     *  Por defecto 3 = 1,0 (sin tocar). Existe porque el micrófono de un móvil
     *  suele entregar de sobra y **satura si hablas cerca**, y eso desde fuera
     *  se oye como "el códec va mal": la voz llega rota y uno culpa al códec o
     *  a la radio. Bajarla es lo que arregla la voz de quien habla pegado. */
    var micGanancia: Int
        get() = p.getInt("mic_ganancia", 3)
        set(v) = p.edit().putInt("mic_ganancia", v.coerceIn(0, 6)).apply()

    /** Micrófono CRUDO en vez del de comunicaciones.
     *
     *  `VOICE_COMMUNICATION` trae el procesado del sistema —cancelación de eco,
     *  su propio control de ganancia— y en algunos móviles eso es justo lo que
     *  satura. `MIC` entrega lo que oye la cápsula y deja el control aquí. */
    var micCrudo: Boolean
        get() = p.getBoolean("mic_crudo", false)
        set(v) = p.edit().putBoolean("mic_crudo", v).apply()

    /** Nivelador automático del micrófono. Encendido por defecto: sin él, unos
     *  se oyen bien y a otros hay que adivinarlos. Se puede apagar para tener
     *  el control entero con la ganancia de arriba. */
    var micAgc: Boolean
        get() = p.getBoolean("mic_agc", true)
        set(v) = p.edit().putBoolean("mic_agc", v).apply()

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

        /** Lo que se enseña en el selector de colchón. El orden ES el valor
         *  de `retrasoLotes`: 0 directo, 1 medio, 2 largo. */
        val COLCHONES = arrayOf(
            "directo · sin colchón (0 ms)",
            "medio · 1 lote (480 ms)  (por defecto)",
            "largo · 2 lotes (960 ms)")

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
