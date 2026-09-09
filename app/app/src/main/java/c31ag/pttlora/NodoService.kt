package c31ag.pttlora

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.location.Location
import android.location.LocationListener
import android.location.LocationManager
import android.os.Build
import android.os.Bundle
import android.os.IBinder
import android.os.PowerManager
import android.util.Log

/**
 * Servicio en primer plano: mantiene el enlace con el nodo, el audio y el PTT.
 *
 * Va en un servicio y no en la Activity porque el uso real es con la pantalla
 * apagada y el móvil en el bolsillo. Es la misma decisión que en la app PTT, y
 * por el mismo motivo.
 */
class NodoService : Service() {

    companion object {
        private const val TAG = "pttlora.servicio"
        const val CANAL_NOTIF = "pttlora"
        const val ACCION_PTT = "c31ag.pttlora.PTT"
        const val EXTRA_PULSADO = "pulsado"

        /** Corte de transmisión propio, en segundos. En un canal simplex y
         *  compartido, un PTT trabado deja el canal inservible para todos: el
         *  TOT no es una comodidad, es disciplina de canal. */
        const val TOT_S = 60

        @Volatile var instancia: NodoService? = null; private set
    }

    /** Lo que la Activity necesita pintar. */
    interface Observador {
        /** El nodo pide el código que muestra en su pantalla. */
        fun onPideCodigo()
        fun onEnlace(conectado: Boolean, detalle: String)
        fun onCanal(ocupado: Boolean)
        fun onQuienHabla(indicativo: String?, rssi: Int)
        /** Arbitraje del micrófono: PTT_LIBRE / PTT_TUYO / PTT_DE_OTRO. */
        fun onPtt(estado: Int, quien: String?)
        fun onTx(transmitiendo: Boolean)
        fun onLog(texto: String)
    }

    @Volatile var observador: Observador? = null

    /* ═══════════ EL SEGUNDO CAMINO: DATOS POR INTERNET ═══════════
     *
     * La radio es la base; esto es el salvavidas. Cuando el nodo no alcanza a
     * nadie, lo que se diga sigue llegando por aquí — y al revés.
     *
     * Al otro lado hay un `nododatos.py`, que para la app **es un nodo más**:
     * habla el mismo protocolo de cliente por TCP. Por eso aquí no hay ningún
     * protocolo nuevo, solo un segundo `Nodo` y la disciplina de no contar dos
     * veces lo que llega por los dos sitios.
     *
     * El ORIGEN de cada cosa se sabe sin campo nuevo: el firmware marca con
     * **RSSI 127** lo que no vino del aire (127 dBm es imposible por radio).
     * Ver `del_enlace` en main.cpp. */
    @Volatile var nodoDatos: Nodo? = null
    @Volatile var datosEnlazado = false; private set

    /** Lo ya visto, para no entregar dos veces lo que llega por los dos
     *  caminos. Misma clave que usa el nodo: origen + stream + secuencia.
     *  Se limita el tamaño porque esto no se vacía solo y una conversación
     *  larga lo llenaría sin parar. */
    private val vistos = object : LinkedHashMap<String, Long>(64, 0.75f, false) {
        override fun removeEldestEntry(e: MutableMap.MutableEntry<String, Long>?) = size > 256
    }

    /** true si es NUEVO (hay que entregarlo); false si ya llegó por el otro
     *  camino. El plazo cubre de sobra la diferencia entre los dos: Internet
     *  suele adelantar a la radio por décimas, no por segundos. */
    private fun nuevo(clave: String): Boolean {
        val ahora = System.currentTimeMillis()
        synchronized(vistos) {
            val v = vistos[clave]
            if (v != null && ahora - v < 8000) return false
            vistos[clave] = ahora
        }
        return true
    }

    /** ¿Vino por Internet en vez de por radio? Ver la nota de arriba. */
    private fun porInternet(rssi: Int) = rssi == 127

    /* ------------------------------------------------------------------
     *  EL DUPLICADO QUE `vistos` NO PUEDE VER, Y EL ECO
     *
     *  `vistos` descarta por (origen, stream, secuencia), que es lo que hace el
     *  firmware, y funciona mientras las dos copias vengan del MISMO nodo.
     *  Con el camino de datos abierto no es el caso, y ese es el fallo de
     *  fondo: **la misma voz entra en la red con dos `src` distintos**, porque
     *  cada camino la sella con la identidad de SU nodo — el de radio con la
     *  MAC de la placa, el de datos con el hash del indicativo. Para `vistos`
     *  son dos transmisiones diferentes de dos estaciones diferentes.
     *
     *  De ahi salen las dos cosas que se oyeron el 8-sep-2026:
     *    * **eco de uno mismo**: lo que sale por radio da la vuelta
     *      (nodo → celda → reflector → nodo de datos) y vuelve a la app, con el
     *      `src` de la celda. La app no lo reconoce como suyo y lo reproduce.
     *    * **al otro se le oye dos veces**, una por camino y descolocadas.
     *
     *  Lo que SI es unico en las dos copias es QUIEN HABLA: el indicativo va en
     *  el INICIO y lo pone el que habla (CMD_IDENT), no el nodo. Ahi es donde
     *  se juntan las dos copias, y por ahi se elige.
     *
     *  ⚠️ Y SE ELIGE LA DE RADIO, SIEMPRE QUE HAYA. No la primera que llegue.
     *
     *  Esto fue un error de la 0.9.21 y merece quedar escrito: se dejo ganar a
     *  la que llegase antes, y por Internet **siempre llega antes** —es un
     *  atajo por fibra frente a un canal de 250 kHz con repeticion por medio—,
     *  de modo que en la practica se escuchaba por Internet SIEMPRE y la radio
     *  quedaba de adorno. Con eso el sistema parece funcionar justo cuando ha
     *  dejado de funcionar: la cobertura de RF se cae y nadie se entera.
     *
     *  La regla es la del proyecto entero: **la base es la radio LoRa, e
     *  Internet es un comodin para cuando la radio no llega**. En muchos sitios
     *  y en muchos cacharros no habra mas que LoRa, y el sistema tiene que
     *  estar pensado para ese caso, no para el otro.
     *
     *  Asi que por cada indicativo se llevan las DOS copias vivas y se
     *  reproduce la de RF; la de Internet solo entra si no hay copia de radio,
     *  o si la de radio se calla (CHARLA_MS) a mitad de transmision — que es
     *  exactamente el comodin haciendo su trabajo, y ademas se ve en pantalla,
     *  porque el origen de cada lote sale marcado 📻 o 🌐. */
    private class Turno(var rf: String? = null, var net: String? = null,
                        var tRf: Long = 0L, var tNet: Long = 0L,
                        var elegida: String? = null)

    /** Indicativo -> las dos copias que le estan llegando y cual se oye. */
    private val turnos = HashMap<String, Turno>()

    /** Stream (src+stream) -> de quien es. La voz y el fin NO llevan
     *  indicativo, solo el INICIO, asi que hay que recordarlo. */
    private val deQuien = object : LinkedHashMap<String, String>(16, 0.75f, false) {
        override fun removeEldestEntry(e: MutableMap.MutableEntry<String, String>?) = size > 64
    }

    /** Cuanto aguanta un camino callado antes de cederle el turno al otro. El
     *  FIN se pierde como cualquier trama —esa cicatriz ya esta en el
     *  firmware—, asi que no se puede depender de el: si el que iba ganando
     *  deja de llegar, el otro tiene que poder tomar el relevo. Tres segundos
     *  son seis lotes de 480 ms: de sobra para no confundir un hueco con una
     *  caida, y poco para que un corte de radio no deje la frase entera muda. */
    private val CHARLA_MS = 3000L

    private fun clave(p: ByteArray) =
        "%02x%02x%02x:%d".format(p[2], p[3], p[4], p[5].toInt() and 0xFF)

    /** RF MANDA. Internet solo si no hay radio o si la radio se ha callado. */
    private fun elige(t: Turno, ahora: Long) {
        val rfVivo = t.rf != null && ahora - t.tRf < CHARLA_MS
        t.elegida = if (rfVivo) t.rf else t.net
    }

    /** INICIO: apunta la copia y dice si es la que hay que reproducir. */
    private fun aceptaInicio(k: String, quien: String, porRf: Boolean): Boolean {
        /* YO NO. Mi voz vuelve por el otro camino con el `src` del nodo por el
           que dio la vuelta, pero con MI indicativo: por eso esto ataja el eco
           venga por donde venga. Sin mayusculas y sin espacios de sobra, que el
           nodo lo puede haber recortado a MAX_INDICATIVO. */
        if (quien.equals(prefs.indicativo.trim(), ignoreCase = true)) return false
        synchronized(turnos) {
            val ahora = System.currentTimeMillis()
            deQuien[k] = quien
            val t = turnos.getOrPut(quien) { Turno() }
            if (porRf) { t.rf = k; t.tRf = ahora } else { t.net = k; t.tNet = ahora }
            elige(t, ahora)
            return t.elegida == k
        }
    }

    /** Un lote: refresca su camino, vuelve a elegir y dice si se reproduce. */
    private fun aceptaLote(k: String): Boolean {
        synchronized(turnos) {
            /* Sin INICIO no se sabe de quien es, y entonces no se puede
               comparar con nada: se reproduce. Perder voz por prudencia seria
               peor que oir un lote de mas. */
            val quien = deQuien[k] ?: return true
            val t = turnos[quien] ?: return true
            val ahora = System.currentTimeMillis()
            if (k == t.rf) t.tRf = ahora else if (k == t.net) t.tNet = ahora
            elige(t, ahora)
            return t.elegida == k
        }
    }

    /** FIN: solo cierra la reproduccion el de la copia que se estaba oyendo.
     *  El de la otra cerraria el audio del bueno a mitad de frase. */
    private fun aceptaFin(k: String): Boolean {
        synchronized(turnos) {
            val quien = deQuien[k] ?: return true
            val t = turnos[quien] ?: return true
            if (t.elegida != k) return false
            turnos.remove(quien)      // se acabo: la siguiente empieza limpia
            return true
        }
    }

    /** Como se enseña el origen de algo que ha llegado. Es la mitad de lo que
     *  el usuario quiere ver: no basta con oírlo, hay que saber **por dónde**
     *  vino — si empieza a entrar todo por Internet teniendo al otro cerca, es
     *  que la radio ha dejado de llegar, y ese aviso no existía. */
    private fun comoLlego(rssi: Int) =
        if (porInternet(rssi)) "🌐 Internet" else "📻 RF $rssi dBm"
    /** Quién tiene el micrófono en el nodo, si no somos nosotros. */
    @Volatile var pttDeOtro: String? = null; private set
    @Volatile private var conectando = false
    /** Se apaga solo cuando el usuario suelta el enlace a proposito: si no, un
     *  "desconectar" volveria a conectarse solo y no habria forma de pararlo. */
    @Volatile private var reconectar = true
    @Volatile private var ultimoIntento = 0L
    @Volatile private var ultimoLatido = 0L
    /** Lo pone la pantalla de ajustes al guardar: la próxima vez que el nodo
     *  diga su estado, se le aplica el indicativo. */
    /** REGISTRO DE ACTIVIDAD.
     *
     *  Sirve para dos cosas que la pantalla principal no puede dar: ver **qué
     *  ha entrado** cuando no estabas mirando (una recepción dura segundos y
     *  el aviso se va), y **diagnosticar** — con quién, con cuánta señal,
     *  cuántos lotes llegaron de los que se mandaron. Un "no se oye bien" con
     *  el registro delante se convierte en "llegan 9 de 13 lotes a -112 dBm",
     *  que ya es una frase con la que se puede trabajar.
     *
     *  Vive en el servicio y no en la pantalla porque el uso normal es con el
     *  móvil en el bolsillo: lo interesante pasa mientras la pantalla no está.
     *  300 líneas, que son horas de uso normal y no llegan a 30 kB. */
    private val registro = ArrayDeque<String>()
    private val reloj = java.text.SimpleDateFormat("HH:mm:ss", java.util.Locale.US)

    @Synchronized fun apunta(texto: String) {
        registro.addLast("${reloj.format(java.util.Date())}  $texto")
        while (registro.size > 300) registro.removeFirst()
    }

    @Synchronized fun registro(): List<String> = registro.toList()
    @Synchronized fun limpiaRegistro() { registro.clear() }

    /* Lo que se va acumulando de la transmisión en curso, para poder resumirla
       en UNA línea al terminar en vez de soltar veinte. */
    private var rxQuien: String? = null
    private var rxLotes = 0
    private var rxRssiMin = 0
    private var rxRssiMax = 0
    private var rxModo = -1
    private var rxT0 = 0L

    @Volatile var ponIndicativo = false
    /** El que dice tener el nodo, para poder enseñarlo tal cual. */
    @Volatile var indicativoDelNodo = ""; private set
    /** Por que no hay enlace. Sin esto la pantalla solo puede decir "sin
     *  enlace", que no ayuda ni al usuario ni a quien le da soporte. */
    @Volatile var ultimoFallo = ""; private set

    private lateinit var prefs: Prefs
    private var nodo: Nodo? = null
    private var audio: AudioEngine? = null
    private var wake: PowerManager.WakeLock? = null

    private var encoder: Codec2? = null
    private var decoder: Codec2? = null
    private var decoderModo = -1

    @Volatile var transmitiendo = false; private set
    @Volatile var canalOcupado = false; private set
    @Volatile var enlazado = false; private set
    @Volatile var pideCodigo = false; private set
    /** [fmin, fmax, pmin, pmax] tal y como los publica el nodo en su estado. */
    @Volatile var limitesHw: FloatArray? = null; private set

    /** Banda permitida por el nodo, en MHz (`banda=430-440MHz` en su estado).
     *  No es lo mismo que `limitesHw`: el chip llega de 420 a 520 y la banda de
     *  aficionados es 430-440. Manda esta, y el que decide es el nodo — la app
     *  solo evita el viaje de ida y vuelta para que el aviso salga al escribir
     *  y no despues. Nula mientras no hayamos hablado con el nodo. */
    @Volatile var limitesBanda: FloatArray? = null; private set
    var ultimoQueHabla: String? = null; private set

    private var lote: ByteArray = ByteArray(0)
    private var loteTramas = 0
    private var t0Tx = 0L

    override fun onCreate() {
        super.onCreate()
        instancia = this
        prefs = Prefs(this)
        arrancaNotificacion()
        if (prefs.posActiva) posicion(true)
        /* UN SOLO HILO para las dos cosas que mantienen el enlace en pie:
           el latido y la reconexion. Las dos son "cada pocos segundos, mira
           como esta el enlace", y separarlas solo dara ocasion de que una se
           quede parada sin que se note.

           LATIDO CADA 10 s, y esto es una leccion cara: el nodo suelta la
           ranura Bluetooth de un cliente callado, y **el latido era de 30 s
           contra un plazo de 12 s en el nodo**. O sea que la app se
           desconectaba SOLA a los pocos segundos de conectar, siempre, y
           parecia un fallo del Bluetooth de Android. Ahora el nodo espera 90 s
           al cliente que ya ha hablado (firmware 1.10) y aqui se late cada 10:
           casi un orden de magnitud de margen, que es lo que hay que dejar
           entre un plazo y lo que lo alimenta.

           RECONEXION: sin ella, cualquier corte —salir de cobertura, apagar y
           encender el nodo, un reinicio por OTA— dejaba la app muerta hasta que
           el usuario entrara en los ajustes a elegir el nodo otra vez. Para el
           pilar de la app eso no vale: se reintenta sola, con espera creciente
           de 3 a 20 s para no freir la bateria si el nodo no esta. */
        Thread({
            var espera = 3000L
            while (true) {
                try { Thread.sleep(2000) } catch (e: InterruptedException) { return@Thread }
                val ahora = System.currentTimeMillis()
                if (enlazado) {
                    espera = 3000L
                    if (!transmitiendo && ahora - ultimoLatido >= 10000) {
                        ultimoLatido = ahora
                        nodo?.latido()
                    }
                    /* LA POSICIÓN TAMBIÉN HAY QUE REFRESCARLA AUNQUE NO SE
                       MUEVA UNO. En el nodo, una posición viva CADUCA a la
                       media hora —para que un nodo que perdió el móvil deje de
                       jurar dónde está—, pero el móvil quieto no manda nada:
                       las actualizaciones de ubicación llegan por movimiento
                       (25 m) o cada 30 s, y con el filtro de distancia un
                       teléfono en una mesa puede pasarse horas callado.
                       Resultado: te caes del mapa sin moverte del sitio, que es
                       exactamente lo contrario de lo que la caducidad busca.
                       Se reenvía la última cada 10 minutos: tres veces dentro
                       de la ventana de media hora, así que aguanta perder dos. */
                    if (prefs.posActiva && ahora - ultimoPosEnviada >= 600000) {
                        ultimaPos?.let { (la, lo) ->
                            ultimoPosEnviada = ahora
                            nodo?.posicion(la, lo, false)
                        }
                    }
                    /* ENLACE MUDO = ENLACE MUERTO.
                       El latido de cada 10 s hace que el nodo conteste su
                       estado, así que 45 s sin recibir NADA no es un rato
                       tranquilo: es un socket que sigue abierto contra un nodo
                       que ya no está. Pasó de verdad —el nodo se reinició y la
                       app siguió marcando enlace, tragándose todo lo que se
                       hablaba— y desde fuera es indistinguible de que la radio
                       no llegue. Mejor cortar y volver a conectar. */
                    val mudo = nodo?.ultimoDato ?: 0L
                    if (mudo > 0 && ahora - mudo > 45000) {
                        observador?.onLog("el nodo lleva 45 s sin decir nada: reconecto")
                        apunta("enlace mudo 45 s: se rehace")
                        conecta(true)
                    }
                } else if (!conectando && reconectar && hayDestino() &&
                           ahora - ultimoIntento >= espera) {
                    ultimoIntento = ahora
                    espera = (espera * 3 / 2).coerceAtMost(20000L)
                    Log.i(TAG, "sin enlace: reintentando")
                    observador?.onLog("reintentando conectar…")
                    conecta()
                }

                /* El camino de DATOS se vigila aparte, y ese "aparte" es todo
                   el sentido de tenerlo: si dependiera del de radio, no serian
                   dos caminos, serian dos formas de caerse a la vez. */
                if (prefs.datosActivo && nodoDatos?.conectado != true)
                    conectaDatos(true)
            }
        }, "enlace").apply { isDaemon = true }.start()
        wake = (getSystemService(Context.POWER_SERVICE) as PowerManager)
            .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "pttlora:enlace")
            .apply { setReferenceCounted(false); acquire() }
        conecta()
        conectaDatos()
    }

    /* ------------------------------------------------------- la posición --
     *
     *  El móvil sabe dónde está y el nodo no; esto es sólo pasárselo, para que
     *  la baliza lo lleve. Quien la publica por la antena es el NODO, no la
     *  app, y por eso apagarlo no es dejar de refrescar: hay que decirle que la
     *  olvide (`olvidaPosicion`), o seguiría balizando la última media hora.
     *
     *  ⚠️ Va por el camino de RADIO. Es lo mismo que con la voz: la posición es
     *  una baliza más y viaja por la antena; a Internet cruza en la celda, que
     *  es la pasarela. Mandarla también por el camino de datos la publicaría en
     *  el reflector aunque la radio no llegue a ningún sitio, y entonces el
     *  mapa enseñaría cobertura que no existe.
     *
     *  Sin Play Services a propósito: `LocationManager` es del sistema, va en
     *  Android 4.4 igual que en el 15 y no añade una dependencia a un firmware
     *  de radio. El GPS a secas basta — aquí no se persigue precisión de
     *  centímetros, se persigue saber en qué ladera está alguien. */
    private var oyenteGps: LocationListener? = null

    /** Cada cuánto se le refresca al nodo. La baliza sale cada minuto, así que
     *  más a menudo sería mandar datos que nadie va a emitir. */
    private val POS_CADA_MS = 30000L
    private val POS_CADA_M = 25f

    fun posicion(activa: Boolean) {
        if (!activa) {
            paraGps()
            // Que la OLVIDE. Sin esto seguiría balizando la última.
            nodo?.olvidaPosicion()
            apunta("posición apagada")
            return
        }
        if (oyenteGps != null) return
        if (Build.VERSION.SDK_INT >= 23 &&
            checkSelfPermission(android.Manifest.permission.ACCESS_FINE_LOCATION)
                != PackageManager.PERMISSION_GRANTED &&
            checkSelfPermission(android.Manifest.permission.ACCESS_COARSE_LOCATION)
                != PackageManager.PERMISSION_GRANTED) {
            apunta("posición: falta el permiso de ubicación")
            return
        }
        val lm = getSystemService(Context.LOCATION_SERVICE) as? LocationManager ?: return
        /* ⚠️ EL PROVEEDOR FUSIONADO PRIMERO, y esto no es un adorno: el 9-sep la
           posición no salía nunca y la causa era ésta. `GPS_PROVIDER` a secas no
           fija bajo techo —puede tardar minutos o no fijar—, y `NETWORK_PROVIDER`
           en los Android modernos muchas veces **ni existe** como proveedor del
           sistema. Resultado: ni un arreglo, nada que mandar, y el nodo con
           `pos=no` sin ninguna pista de por qué.
           `FUSED_PROVIDER` (Android 12+) es del propio LocationManager, no de
           Play Services, y da posición dentro de casa por wifi y sensores. */
        val proveedores = ArrayList<String>()
        if (Build.VERSION.SDK_INT >= 31) proveedores.add(LocationManager.FUSED_PROVIDER)
        proveedores.add(LocationManager.GPS_PROVIDER)
        proveedores.add(LocationManager.NETWORK_PROVIDER)
        val o = object : LocationListener {
            override fun onLocationChanged(l: Location) = mandaPosicion(l)
            // Los tres de abajo son abstractos en Android 4.4 y no se pueden
            // omitir aunque en las versiones nuevas ya tengan cuerpo por
            // defecto: sin ellos no compila contra minSdk 19.
            override fun onStatusChanged(p: String?, e: Int, x: Bundle?) {}
            override fun onProviderEnabled(p: String) {}
            override fun onProviderDisabled(p: String) {}
        }
        oyenteGps = o
        try {
            var alguno = false
            for (prov in proveedores) {
                if (!lm.allProviders.contains(prov)) continue
                if (!lm.isProviderEnabled(prov)) continue
                lm.requestLocationUpdates(prov, POS_CADA_MS, POS_CADA_M, o)
                alguno = true
            }
            if (!alguno) {
                apunta("posición: ningún proveedor de ubicación activo " +
                       "(¿localización apagada en el móvil?)")
            }
            /* La última conocida, YA. Un arranque en frío del GPS son minutos
               con el cielo despejado: sin esto, encender la posición parece que
               no hace nada. */
            var ultima: Location? = null
            for (prov in proveedores) {
                if (!lm.allProviders.contains(prov)) continue
                ultima = ultima ?: lm.getLastKnownLocation(prov)
            }
            if (ultima != null) mandaPosicion(ultima)
            else apunta("posición encendida · esperando a que el móvil sepa dónde está")
        } catch (e: SecurityException) {
            oyenteGps = null
            apunta("posición: Android no da la ubicación")
        }
    }

    private fun paraGps() {
        val o = oyenteGps ?: return
        oyenteGps = null
        try {
            (getSystemService(Context.LOCATION_SERVICE) as? LocationManager)
                ?.removeUpdates(o)
        } catch (_: Exception) {}
    }

    /** La última que se le mandó, para poder repetirla al reconectar: la
     *  posición viva NO se guarda en el nodo (escribir la flash cada medio
     *  minuto se la carga), así que un nodo que se reinicia se queda sin ella
     *  hasta el siguiente arreglo del GPS, que pueden ser minutos. */
    private var ultimaPos: Pair<Int, Int>? = null
    /** Cuándo se le mandó por última vez, para poder refrescarla sin depender
     *  de que el GPS tenga algo nuevo que decir. Ver el latido. */
    @Volatile private var ultimoPosEnviada = 0L

    /** Lo que se le ha mandado al nodo, en texto, para poder enseñarlo. Sin
     *  esto, «Posición: encendida» no distingue entre estar funcionando y no
     *  haber mandado nunca nada — que es exactamente la confusión del 9-sep. */
    @Volatile var posTexto: String = "sin arreglo todavía"
        private set

    private fun mandaPosicion(l: Location) {
        if (!prefs.posActiva) return
        val la = Math.round(l.latitude * 1e7).toInt()
        val lo = Math.round(l.longitude * 1e7).toInt()
        val primera = ultimaPos == null
        ultimaPos = Pair(la, lo)
        posTexto = "%.5f, %.5f (%s, ±%.0f m)".format(
            l.latitude, l.longitude, l.provider ?: "?", l.accuracy)
        if (nodo == null || !enlazado) {
            posTexto += " · sin nodo al que mandarla"
            return
        }
        nodo?.posicion(la, lo, false)
        ultimoPosEnviada = System.currentTimeMillis()
        // Se apunta la primera y luego solo de vez en cuando: cada 30 s en el
        // registro seria ruido, pero no ver NINGUNA es peor.
        if (primera) apunta("📍 posición al nodo: $posTexto")
    }

    override fun onDestroy() {
        paraGps()
        sueltaPtt()
        nodoDatos?.desconecta()
        nodo?.desconecta()
        audio?.release()
        encoder?.cierra(); decoder?.cierra()
        try { wake?.release() } catch (_: Exception) {}
        instancia = null
        super.onDestroy()
    }

    override fun onBind(i: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, id: Int): Int {
        if (intent?.action == ACCION_PTT)
            pulsacion(intent.getBooleanExtra(EXTRA_PULSADO, false))
        return START_STICKY
    }

    // ------------------------------------------------------------- enlace ---
    /* `forzar` = tirar lo que haya y volver a conectar (se usa al cambiar los
       ajustes). Sin forzar, si ya hay enlace no se toca NADA.
       Esto no es una optimizacion: el servicio se conecta solo al crearse, y la
       pantalla de ajustes tambien pedia conectar al guardar. Eran DOS objetos
       con DOS sockets contra un nodo que **solo admite uno**: el primero se
       quedaba la ranura (el nodo decia "conectado") y el segundo — que es el
       que mira la interfaz — se quedaba fuera, con un "Sin enlace" que no
       explicaba nada. */
    fun conecta(forzar: Boolean = false) {
        reconectar = true
        if (!forzar && (nodo?.conectado == true || conectando)) return
        if (forzar) { nodo?.desconecta(); nodo = null }
        conectando = true
        val porWifi = prefs.porWifi
        val destino = if (porWifi) prefs.host else prefs.mac
        if (destino.isBlank()) {
            conectando = false
            observador?.onEnlace(false,
                if (porWifi) "sin dirección del nodo" else "sin nodo elegido")
            return
        }
        val n = Nodo(::evento, { ok, detalle ->
            enlazado = ok
            conectando = false
            ultimoFallo = if (ok) "" else detalle
            if (ok) {
                /* Y la posición otra vez: el nodo la olvida al reiniciarse
                   (ver `ultimaPos`), así que sin esto un nodo que se reinicia
                   deja de aparecer en el mapa sin que nadie lo note. */
                if (prefs.posActiva) ultimaPos?.let { (la, lo) ->
                    nodo?.posicion(la, lo, false)
                }
                /* El indicativo se manda SIEMPRE al conectar, y por DOS
                   caminos distintos que no son lo mismo:
                   - IDENT dice con qué indicativo emite ESTE cliente. Es lo que
                     permite que varios se cuelguen del mismo nodo sin emitir
                     todos bajo el mismo distintivo.
                   - CONFIG le pone al NODO el suyo, que es el que usa para su
                     baliza de identificación cada diez minutos.
                   Por WiFi importa el primero; por Bluetooth, donde solo hay un
                   usuario, los dos acaban coincidiendo. */
                /* SOLO `IDENT`: quien soy yo. NADA MAS.
                 *
                 * Conectarse a un nodo NO es motivo para reconfigurarlo, y
                 * hacerlo tiene consecuencias que no se ven venir: el SF y el
                 * ancho de banda son parametros de CANAL, asi que un movil que
                 * llegaba con otros ajustes dejaba al nodo incomunicado con el
                 * resto de la red — paso de verdad, con un movil poniendo SF7
                 * en cada conexion mientras la red estaba en SF8, y desde
                 * fuera parecia que la radio no funcionaba.
                 * Lo mismo vale para el indicativo del nodo, el canal, los
                 * saltos y el perfil: son del NODO, no de quien se conecta.
                 * Todo eso se manda solo cuando el usuario lo pide
                 * expresamente (Guardar en los ajustes, o Aplicar en Radio). */
                nodo?.identifica(prefs.indicativo)
            } else {
                canalOcupado = false
                pttDeOtro = null
                /* Y SE OLVIDA EL CODIGO PENDIENTE.
                   El codigo lo genera el nodo para UNA sesion: si el enlace se
                   cae, ese numero ya no existe. Dejar la bandera puesta hacia
                   que la pantalla siguiera pidiendo —y reabriendo el dialogo
                   cada segundo— un codigo que no salia en ninguna pantalla
                   porque ya no habia sesion a la que pertenecer. */
                pideCodigo = false
            }
            apunta(if (ok) "enlazado · $detalle" else "sin enlace · $detalle")
            observador?.onEnlace(ok, detalle)
            notifica(if (ok) "Enlazado con $detalle" else detalle)
        }, this)
        n.onDiagnostico = { apunta("bluetooth: $it") }
        nodo = n
        if (porWifi) n.conectaTcp(destino, prefs.puerto) else n.conecta(destino)
    }

    /** Abre (o cierra) el camino de datos segun los ajustes.
     *
     *  Es INDEPENDIENTE del de radio a proposito: cada uno se cae y se levanta
     *  por su cuenta, que es justo lo que hace que uno sirva de red del otro.
     *  Si dependieran el uno del otro no habria redundancia, habria dos formas
     *  de fallar a la vez. */
    fun conectaDatos(forzar: Boolean = false) {
        if (!prefs.datosActivo) {
            nodoDatos?.desconecta(); nodoDatos = null; datosEnlazado = false
            return
        }
        if (!forzar && nodoDatos?.conectado == true) return
        nodoDatos?.desconecta()
        val n = Nodo(::evento, { ok, detalle ->
            datosEnlazado = ok
            apunta(if (ok) "🌐 datos · $detalle" else "🌐 datos sin enlace · $detalle")
            if (ok) nodoDatos?.identifica(prefs.indicativo)
            observador?.onEnlace(enlazado || datosEnlazado,
                                 if (enlazado) "radio" else "solo datos")
        }, this)
        nodoDatos = n
        n.conectaTcp(prefs.datosHost, prefs.datosPuerto)
    }

    /** POR RADIO. Por Internet SOLO si no hay radio.
     *
     *  Hasta la 0.9.21 se mandaba por los dos a la vez y se llamaba
     *  redundancia. No lo era, y ademas sobraba:
     *
     *  **la celda ya es la pasarela.** Lo que sale por la antena y alcanza una
     *  celda entra en el reflector por su enlace, y de ahi le llega a quien
     *  escuche por Internet. Mandarlo ademas por el camino de datos no añade un
     *  oyente: duplica en la red lo que la celda ya estaba haciendo, y le pone
     *  al que recibe el trabajo de tirar una de las dos copias.
     *
     *  Y lo importante es lo otro: **la base del sistema es la radio LoRa**.
     *  Internet es un comodin para cuando la radio no llega, no un camino
     *  paralelo permanente — en muchos sitios y en muchos cacharros no habra
     *  mas que LoRa. Un sistema que manda siempre por los dos acaba
     *  funcionando por el que nunca falla, y entonces nadie se entera de que la
     *  radio dejo de cubrir.
     *
     *  De ahi la regla, en TX igual que en RX: si sale por RF, va por RF. El
     *  camino de datos entra cuando NO hay nodo —el movil solo, que es para lo
     *  que se hizo en la 0.9.20— y ahi es lo unico que hay.
     *
     *  ⚠️ Queda un caso a medias, y esta a proposito sin cubrir: un nodo SIN
     *  CELDA A LA VISTA emite por radio, pero esa voz no llega a Internet
     *  porque no hay quien la puentee. El nodo sabe si tiene celda encima
     *  (`modo=cliente(hay celda)` en el estado), asi que se puede decidir con
     *  ese dato; se deja para cuando haya una segunda celda y se pueda probar
     *  de verdad. */
    private fun mandaPorTodos(tipo: Int, datos: ByteArray = ByteArray(0)) {
        if (enlazado) {
            nodo?.manda(tipo, datos)
        } else if (prefs.datosActivo) {
            nodoDatos?.manda(tipo, datos)
        }
    }

    /** Si hay a donde conectarse. Sin esto la reconexion daria vueltas en
     *  vacio (y lo diria por pantalla) cuando aun no se ha elegido nodo. */
    private fun hayDestino(): Boolean =
        if (prefs.porWifi) prefs.host.isNotBlank() else prefs.mac.isNotBlank()

    /** Soltar el enlace a mano: no se vuelve a conectar hasta que se pida. */
    fun desconecta() { reconectar = false; nodo?.desconecta() }

    /** Volver a intentarlo YA, sin esperar a la siguiente vuelta. */
    fun reintenta() {
        reconectar = true
        ultimoIntento = 0L
        conecta(true)
    }

    fun autoriza(codigo: String) { nodo?.autoriza(codigo) }

    /** Reenvía los ajustes de radio (frecuencia, canal y potencia).
     *  Aquí SÍ va el indicativo: se llega por «Guardar» en los ajustes, que es
     *  el usuario diciendo expresamente qué quiere en el nodo. */
    fun mandaRadio() {
        nodo?.configura(prefs.canalLogico, prefs.saltos,
                        prefs.potencia, prefs.perfil, prefs.indicativo)
        apunta("ajustes aplicados al nodo: sf${prefs.sf}/${prefs.anchoKHz}, " +
               "%.3f MHz, ${prefs.potencia} dBm".format(prefs.frecuenciaKHz / 1000.0))
        nodo?.radio(prefs.frecuenciaKHz, prefs.anchoKHz, prefs.sf, 5, prefs.potencia)
    }

    /** Manda al nodo las credenciales del WiFi que el usuario haya puesto. */
    fun mandaWifi() {
        nodo?.configuraWifi(prefs.wifiSsid, prefs.wifiClave)
    }

    /** Modo de red del nodo. Tiene efecto al reiniciarlo. */
    fun mandaRed(modo: Int, ssid: String, clave: String) {
        nodo?.configuraRed(modo, ssid, clave)
    }

    /** Enlace con otro nodo por Internet; host vacío los suelta todos. */
    fun mandaEnlace(host: String, puerto: Int) {
        nodo?.enlaza(host, puerto)
    }

    /** Actualización de firmware en curso, si la hay. Mientras dura, TODOS los
     *  eventos se le pasan también a ella: los acuses del nodo llegan por el
     *  mismo camino que todo lo demás. */
    @Volatile var ota: Ota? = null

    /** Arranca una actualización del firmware del nodo. Bloquea, así que se
     *  llama desde un hilo aparte. Ver [Ota]. */
    fun actualizaFirmware(binario: ByteArray,
                          alAvanzar: (Int, String) -> Unit): String? {
        val n = nodo ?: return "sin enlace con el nodo"
        val o = Ota(n, alAvanzar)
        ota = o
        return try {
            n.fiable(true)          // el firmware no puede perder un solo byte
            o.sube(binario)
        } finally {
            n.fiable(false)
            ota = null
        }
    }

    // ------------------------------------------------------------ eventos ---
    private fun evento(tipo: Int, p: ByteArray) {
        ota?.alEvento(tipo, p)
        when (tipo) {
            Nodo.EV_CANAL -> {
                canalOcupado = p.isNotEmpty() && p[0].toInt() != 0
                observador?.onCanal(canalOcupado)
                if (!canalOcupado) {
                    ultimoQueHabla = null
                    observador?.onQuienHabla(null, 0)
                }
            }
            Nodo.EV_INICIO -> if (p.size >= 8) {
                val rssi = p[0].toInt()
                /* DESCARTE DEL DUPLICADO. Con los dos caminos abiertos, lo
                   mismo llega dos veces: gana el que llegue antes (casi
                   siempre Internet, que es atajo) y el otro se tira. Clave:
                   origen + stream, igual que hace el nodo. */
                if (!nuevo("I:%02x%02x%02x:%d".format(
                        p[2], p[3], p[4], p[5].toInt() and 0xFF))) return
                val modo = p[6].toInt() and 0xFF
                /* ⚠️ EL INDICATIVO SE CORTA EN EL PRIMER CERO. Desde el
                   firmware v1.35, detrás puede venir `\0` + 8 bytes con la
                   POSICIÓN de quien habla (la de una persona sale aquí, no en
                   la baliza). Sin este corte el indicativo se lee con la cola
                   binaria pegada, y eso no es sólo una etiqueta fea: **el
                   filtro del eco compara el indicativo con el mío**, dejaría de
                   reconocerse y volvería a oírse uno mismo. */
                val bruto = String(p, 7, p.size - 7, Charsets.US_ASCII)
                val ind = bruto.substringBefore('\u0000').trim()
                /* Y AQUI EL SEGUNDO FILTRO, el que `vistos` no puede hacer: por
                   QUIEN HABLA. Tira mi propio eco, y de las dos copias se
                   queda con LA DE RADIO. Ver la nota larga de `Turno`. */
                if (!aceptaInicio(clave(p), ind, !porInternet(rssi))) return
                abreDecoder(modo)
                ultimoQueHabla = ind
                observador?.onQuienHabla(ind, rssi)
                apunta("▼ %s · %s".format(ind, comoLlego(rssi)))
                preparaAudioRx()
                rxQuien = ind; rxLotes = 0; rxModo = modo; rxT0 = System.currentTimeMillis()
                rxRssiMin = rssi; rxRssiMax = rssi
            }
            Nodo.EV_VOZ -> if (p.size > 9) {
                /* El duplicado se tira ANTES de decodificar: pasarlo dos veces
                   por el códec no solo gasta, es que **se oiría dos veces**.
                   Clave con la secuencia, que es lo que distingue un lote de
                   otro dentro de la misma transmisión. */
                if (!nuevo("V:%02x%02x%02x:%d:%d".format(
                        p[2], p[3], p[4], p[5].toInt() and 0xFF,
                        p[6].toInt() and 0xFF))) return
                if (!aceptaLote(clave(p))) return
                val r = p[0].toInt()
                if (rxQuien != null) {
                    rxLotes++
                    // 127 no es una medida: es la marca de que no vino por la
                    // antena (otro cliente del mismo nodo, o el enlace).
                    if (r != 127) {
                        if (rxRssiMin == 127 || r < rxRssiMin) rxRssiMin = r
                        if (rxRssiMax == 127 || r > rxRssiMax) rxRssiMax = r
                    }
                }
                reproduceLote(p)
            }
            Nodo.EV_FIN -> {
                /* El FIN de la copia que NO se esta oyendo se tira: si no,
                   cerraria la reproduccion de la buena a mitad de frase. */
                if (p.size >= 4 && !aceptaFin("%02x%02x%02x:%d".format(
                        p[0], p[1], p[2], p[3].toInt() and 0xFF))) return
                audio?.resetPlayback()
                rxQuien?.let { q ->
                    val seg = (System.currentTimeMillis() - rxT0) / 1000.0
                    val señal = if (rxRssiMin == 127) "por red"
                                else if (rxRssiMin == rxRssiMax) "${rxRssiMax} dBm"
                                else "${rxRssiMax}..${rxRssiMin} dBm"
                    apunta("◀ $q · $rxLotes lotes · %.1f s · $señal · ${Codec2.NOMBRES.getOrElse(rxModo) { "?" }}".format(seg))
                }
                rxQuien = null
            }
            Nodo.EV_HOLA -> if (p.size >= 9) {
                /* [rssi][snr][src×3][stream][flags][batería][indicativo]\0[nombre]
                   Estaba desplazado un byte y el indicativo salía con el byte
                   de batería pegado delante.

                   Desde el firmware v1.30, detrás del indicativo va un cero y
                   el NOMBRE del nodo. Hace falta porque **todos los nodos de un
                   operador llevan SU indicativo** —es lo legalmente correcto, y
                   además esta app se lo pone sola—, así que sin el nombre la
                   lista eran tres balizas idénticas de `C31AG`. El `\0` puede
                   no venir (firmware viejo), y por eso se parte y se mira. */
                val cola = String(p, 8, p.size - 8, Charsets.US_ASCII).split('\u0000')
                val ind = cola[0].trim()
                val nom = cola.getOrNull(1)?.trim().orEmpty()
                val quien = if (nom.isNotEmpty()) "$nom ($ind)" else ind
                val f = p[6].toInt()
                val papel = when {
                    // La CELDA primero: es infraestructura, y saber cuál lo es
                    // importa más que cualquier otra cosa de esta línea.
                    (f and 0x04) != 0 -> if ((f and 0x02) != 0) "celda+móvil" else "celda"
                    (f and 0x03) == 3 -> "repetidor+móvil"
                    (f and 0x01) != 0 -> "repetidor"
                    (f and 0x02) != 0 -> "con móvil"
                    else -> "nodo"
                }
                val bat = p[7].toInt() and 0xFF
                apunta("baliza $quien · $papel · batería $bat% · ${p[0].toInt()} dBm")
                observador?.onLog("baliza de $quien (${p[0].toInt()} dBm)")
            }
            /* Con varios usuarios en el mismo nodo, el micrófono es de uno.
               El nodo dice de quién, para que se pueda enseñar en vez de dejar
               un PTT que no responde y no se sabe por qué. */
            Nodo.EV_PTT -> if (p.isNotEmpty()) {
                pttDeOtro = if (p[0].toInt() == Nodo.PTT_DE_OTRO)
                    String(p, 1, p.size - 1, Charsets.US_ASCII).trim() else null
                observador?.onPtt(p[0].toInt(), pttDeOtro)
                if (pttDeOtro != null && transmitiendo) {
                    // Nos lo han negado a media pulsación: no seguir mandando
                    // voz que el nodo va a tirar.
                    pulsacion(false)
                    observador?.onLog("habla $pttDeOtro")
                }
            }
            Nodo.EV_EMPAREJA -> {
                // El nodo no nos conoce: enseña un código en SU pantalla y
                // hasta que no se lo devolvamos ignora todo lo demás.
                pideCodigo = true
                observador?.onPideCodigo()
            }
            Nodo.EV_ESTADO, Nodo.EV_LOG -> {
                val t = String(p, Charsets.US_ASCII)
                // hw=420-520MHz/2-17dBm — los limites los dice el hardware.
                Regex("hw=([\\d.]+)-([\\d.]+)MHz/(\\d+)-(\\d+)dBm")
                    .find(t)?.let { m ->
                        limitesHw = floatArrayOf(
                            m.groupValues[1].toFloat(), m.groupValues[2].toFloat(),
                            m.groupValues[3].toFloat(), m.groupValues[4].toFloat())
                    }
                Regex("banda=([\\d.]+)-([\\d.]+)MHz")
                    .find(t)?.let { m ->
                        limitesBanda = floatArrayOf(
                            m.groupValues[1].toFloat(), m.groupValues[2].toFloat())
                    }
                if (t.startsWith("autorizado")) pideCodigo = false
                // El estado completo no: son 400 caracteres cada pocos
                // segundos y taparian lo que importa. Los avisos del nodo si.
                if (!t.startsWith("v")) apunta("nodo: $t")
                /* El indicativo del nodo es el segundo campo del estado
                   ("v1.13 C31AG-7 perfil=..."). Se le pone el nuestro en dos
                   casos y solo en dos: si no tiene ninguno (NOCALL, un nodo
                   recien estrenado, que si no no puede ni identificarse), o si
                   el usuario acaba de guardar los ajustes. Fuera de ahi, el
                   nodo se queda con el suyo — ver `Nodo.configura`. */
                if (t.startsWith("v")) {
                    val campos = t.split(' ')
                    val suyo = if (campos.size > 1) campos[1] else ""
                    indicativoDelNodo = suyo
                    val sinPoner = suyo.isBlank() || suyo == "NOCALL"
                    if ((sinPoner || ponIndicativo) && prefs.indicativo.isNotBlank()) {
                        ponIndicativo = false
                        if (suyo != prefs.indicativo.uppercase())
                            nodo?.configura(prefs.canalLogico, prefs.saltos,
                                            prefs.potencia, prefs.perfil,
                                            prefs.indicativo)
                    }
                }
                observador?.onLog(t)
            }
        }
    }

    /** El motor de audio se dimensiona con la trama del codec que toque, y el
     *  modo lo decide QUIEN HABLA, no nosotros: puede cambiar entre una
     *  transmision y la siguiente. */
    private fun preparaAudioRx() {
        val d = decoder ?: return
        if (audio == null || audio?.FRAME != d.muestrasPorTrama) {
            audio?.release()
            audio = AudioEngine(this, d.muestrasPorTrama, ::tramaCapturada)
        }
    }

    private fun abreDecoder(modo: Int) {
        if (decoderModo == modo && decoder != null) return
        decoder?.cierra()
        decoder = Codec2.abre(modo)
        decoderModo = if (decoder != null) modo else -1
        if (decoder == null) observador?.onLog("modo de códec $modo no soportado")
    }

    /** EV_VOZ = [rssi][snr][src×3][stream][seq][modo][n][datos] */
    private fun reproduceLote(p: ByteArray) {
        val modo = p[7].toInt() and 0xFF
        val n = p[8].toInt() and 0xFF
        abreDecoder(modo)
        val d = decoder ?: return
        val a = audio ?: return
        val bits = d.bytesPorTrama
        val pcm = ShortArray(d.muestrasPorTrama)
        var off = 9
        var i = 0
        while (i < n && off + bits <= p.size) {
            d.descodifica(p, off, pcm, 0)
            a.play(pcm.copyOf())
            off += bits
            i++
        }
    }

    // ---------------------------------------------------------------- PTT ---
    /** Único punto de entrada del PTT: pantalla, tecla física y manos libres
     *  pasan todos por aquí. Tener una sola lógica evita que cada camino se
     *  comporte distinto, que es lo que pasó en la app PTT. */
    fun pulsacion(abajo: Boolean) {
        if (abajo) tomaPtt() else sueltaPtt()
    }

    /** ¿Se puede hablar? Basta con que UNO de los dos caminos esté en pie.
     *
     *  Antes exigía el nodo de radio, y eso dejaba fuera un caso que resultó
     *  ser el interesante: **un móvil sin nodo**. Hay teléfonos que no se
     *  entienden con el BLE del ESP32 —los viejos, sobre todo— y hay gente que
     *  quiere escuchar y hablar desde el sofá sin sacar la radio. Con el camino
     *  de datos activo eso ya funciona: entra por Internet, sale por la antena
     *  de la celda, y el resto de la red no nota la diferencia.
     *
     *  No es un modo aparte ni hay que configurarlo: es el mismo camino de
     *  datos que da la redundancia, usado cuando el otro no está. */
    private fun hayPorDondeHablar() = enlazado || datosEnlazado

    fun tomaPtt() {
        if (transmitiendo || !hayPorDondeHablar()) return
        if (canalOcupado) {
            observador?.onLog("canal ocupado" +
                (ultimoQueHabla?.let { " ($it)" } ?: ""))
            Vibra.buzz(this, 30)
            return
        }
        val modo = prefs.modo
        if (encoder?.modo != modo) {
            encoder?.cierra()
            encoder = Codec2.abre(modo)
        }
        val e = encoder
        if (e == null) { observador?.onLog("sin códec: no se puede transmitir"); return }

        if (audio == null || audio?.FRAME != e.muestrasPorTrama) {
            audio?.release()
            audio = AudioEngine(this, e.muestrasPorTrama, ::tramaCapturada)
        }
        lote = ByteArray(e.bytesPorTrama * e.tramasPorLote)
        loteTramas = 0
        transmitiendo = true
        t0Tx = System.currentTimeMillis()
        mandaPorTodos(Nodo.CMD_INICIO, byteArrayOf(modo.toByte()))
        audio?.startCapture()
        Vibra.buzz(this, 30)
        observador?.onTx(true)
        notifica("Transmitiendo")
    }

    fun sueltaPtt() {
        if (!transmitiendo) return
        transmitiendo = false
        audio?.stopCapture()
        // Lo que quede a medias se manda igual: cortar una sílaba por no
        // completar el lote se nota más que medio lote corto.
        if (loteTramas > 0) mandaLote()
        mandaPorTodos(Nodo.CMD_FIN)
        apunta("▶ tú · %.1f s · %s".format((System.currentTimeMillis() - t0Tx) / 1000.0,
                                           Codec2.NOMBRES.getOrElse(prefs.modo) { "?" }))
        observador?.onTx(false)
        notifica(if (enlazado) "Enlazado"
                 else if (datosEnlazado) "Solo datos"
                 else "Sin enlace")
    }

    private fun tramaCapturada(pcm: ShortArray) {
        val e = encoder ?: return
        if (!transmitiendo) return
        if (System.currentTimeMillis() - t0Tx > TOT_S * 1000L) {
            observador?.onLog("corte por tiempo (${TOT_S}s)")
            sueltaPtt()
            return
        }
        e.codifica(pcm, 0, lote, loteTramas * e.bytesPorTrama)
        loteTramas++
        if (loteTramas >= e.tramasPorLote) mandaLote()
    }

    private fun mandaLote() {
        val e = encoder ?: return
        val n = loteTramas
        if (n <= 0) return
        val datos = ByteArray(2 + n * e.bytesPorTrama)
        datos[0] = e.modo.toByte()
        datos[1] = n.toByte()
        System.arraycopy(lote, 0, datos, 2, n * e.bytesPorTrama)
        mandaPorTodos(Nodo.CMD_VOZ, datos)
        loteTramas = 0
    }

    // -------------------------------------------------------- notificacion ---
    private fun arrancaNotificacion() {
        if (Build.VERSION.SDK_INT >= 26) {
            val nm = getSystemService(NotificationManager::class.java)
            nm.createNotificationChannel(NotificationChannel(CANAL_NOTIF, "Enlace",
                NotificationManager.IMPORTANCE_LOW))
        }
        startForeground(1, construye("Arrancando"))
    }

    private fun construye(texto: String): Notification {
        val pi = PendingIntent.getActivity(this, 0,
            Intent(this, MainActivity::class.java),
            if (Build.VERSION.SDK_INT >= 23) PendingIntent.FLAG_IMMUTABLE else 0)
        @Suppress("DEPRECATION")
        val b = if (Build.VERSION.SDK_INT >= 26)
                    Notification.Builder(this, CANAL_NOTIF)
                else Notification.Builder(this)
        return b.setContentTitle("PTT LoRa")
            .setContentText(texto)
            /* NO la flecha de subida (`stat_sys_upload`): en la barra de estado es
               **el mismo icono animado que usan las apps de subir o sincronizar
               ficheros**, y el usuario no tiene forma de distinguir si esto esta
               en el aire o su movil esta mandando fotos a algun sitio. Un
               altavoz dice lo que esto es. */
            .setSmallIcon(android.R.drawable.stat_sys_speakerphone)
            .setOngoing(true)
            .setContentIntent(pi)
            .build()
    }

    private fun notifica(texto: String) {
        try {
            (getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager)
                .notify(1, construye(texto))
        } catch (e: Exception) { Log.w("pttlora", "notif: ${e.message}") }
    }
}
