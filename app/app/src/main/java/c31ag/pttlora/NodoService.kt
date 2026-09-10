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
    /** UNA TRANSMISION. **Las balizas no cuentan**: una baliza no es alguien
     *  hablando, y mezclarlas tapa justo lo que se busca mirar en un QSO con
     *  varios. Para las balizas está el registro.
     *
     *  Es una fila POR TRANSMISION y no por persona: lo que hace falta ver es
     *  el ORDEN real de lo que ha pasado —quién entró detrás de quién y cuánto
     *  duró cada uno—, y con una sola fila por indicativo eso se pierde en
     *  cuanto alguien habla dos veces.
     *
     *  Y LAS PROPIAS TAMBIEN VAN DENTRO. Sin ellas la lista miente sobre el
     *  orden: se ve a quién oíste, pero no cuándo entraste tú entre medias, que
     *  es justo lo que hace falta para saber por dónde va la rueda. */
    class Hablante(val indicativo: String, var cuando: Long, var porRf: Boolean,
                   var rssi: Int, var segundos: Double = 0.0,
                   var yo: Boolean = false)

    /** Las últimas transmisiones, la más reciente primero. */
    private val rueda = ArrayList<Hablante>()
    /** Cuántas caben en la caja antes de que empiecen a caer por abajo. */
    private val MAX_RUEDA = 20

    fun ultimosHablantes(): List<Hablante> = synchronized(rueda) { ArrayList(rueda) }

    /** La fila de la transmision que se esta oyendo AHORA, para poder
     *  escribirle la duracion cuando acabe. */
    private var filaEnCurso: Hablante? = null

    private fun apuntaHablante(ind: String, rssi: Int) {
        if (ind.isBlank()) return
        val h = Hablante(ind, System.currentTimeMillis(), porRadio(rssi), rssi)
        synchronized(rueda) {
            rueda.add(0, h)
            // Una caja que se va llenando; las mas viejas caen por abajo.
            while (rueda.size > MAX_RUEDA) rueda.removeAt(rueda.size - 1)
        }
        filaEnCurso = h
        observador?.onRueda()
    }

    /** Se acabo de oir a alguien: se le pone la duracion a su fila. */
    private fun cierraFila(seg: Double, porRf: Boolean, rssi: Int) {
        val h = filaEnCurso ?: return
        filaEnCurso = null
        synchronized(rueda) {
            h.segundos = seg
            h.porRf = porRf
            if (rssi < 126) h.rssi = rssi
        }
        observador?.onRueda()
    }

    /** Y LO MIO TAMBIEN VA A LA LISTA. Ver la nota de `Hablante`. */
    private fun apuntaLoMio(seg: Double, porRf: Boolean) {
        val ind = prefs.indicativo.trim().ifBlank { "yo" }
        synchronized(rueda) {
            rueda.add(0, Hablante(ind, System.currentTimeMillis() - (seg * 1000).toLong(),
                                  porRf, 127, seg, true))
            while (rueda.size > MAX_RUEDA) rueda.removeAt(rueda.size - 1)
        }
        observador?.onRueda()
    }

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
        /** Ha hablado alguien: hay que repintar la rueda. */
        fun onRueda()
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
    /* ── DE DONDE VIENE UNA TRAMA ──
     * El `rssi` es una medida de radio salvo dos valores imposibles por el
     * aire, que son marcas (ver protocolo.h):
     *   127 = por el enlace de Internet
     *   126 = eco local: otro cliente de MI MISMO nodo
     * Separarlos importa: hablando con alguien colgado del mismo nodo, esto
     * decia 🌐 y apuntaba la voz como «tapada por Internet» sin que hubiera
     * Internet de por medio — y con esas cuentas es con lo que se juzga si la
     * cobertura de radio esta bien. */
    private fun porInternet(rssi: Int) = rssi == 127
    private fun mismoNodo(rssi: Int) = rssi == 126
    /** ⚠️ Lo unico que cuenta como RADIO. Un `!porInternet()` a secas daria por
     *  buena la marca de eco local y haria creer que la antena esta oyendo. */
    private fun porRadio(rssi: Int) = rssi < 126

    /* ------------------------------------------------------------------
     *  ¿ESTÁ VIVA LA RADIO?
     *
     *  Hace falta saberlo, y el 9-sep se vio por qué: alguien se alejó de la
     *  celda, la perdió a las 15:47 —última trama a −88 dBm— y **siguió viendo
     *  llegar todo por el camino de datos**, así que parecía que la red iba
     *  bien. Cinco minutos después transmitió once segundos que **no oyó
     *  nadie**, porque con un nodo conectado la voz sale sólo por RF. Y la app
     *  no dijo ni una palabra.
     *
     *  La señal de que la radio funciona es que **entre algo por la antena**:
     *  cualquier trama con RSSI que no sea 127. Las celdas balizan cada minuto,
     *  así que dos minutos y medio de silencio ya no es un hueco, es que no
     *  llega. */
    @Volatile private var ultimoRf = 0L
    /** Lo último que se avisó, para no repetir la línea cada dos segundos.
     *  Arranca en `true` para que el primer aviso sea el de la pérdida. */
    private var radioAvisada = true
    private val RADIO_MUDA_MS = 150_000L

    /** true si por la antena ha entrado algo hace poco. */
    fun hayRadio(): Boolean {
        val t = ultimoRf
        return t > 0L && System.currentTimeMillis() - t < RADIO_MUDA_MS
    }

    /* ══════════════════ DOS CAMINOS, UNA SOLA VOZ ══════════════════
     *
     * LA RADIO ES LA BASE Y INTERNET ES UN COMODIN. Eso no significa elegir un
     * camino y despreciar el otro: significa que **la red tiene que funcionar
     * sin Internet**, y que cuando lo hay sirve para unir celdas y para tapar
     * lo que la radio no trajo. Ni un lote mas.
     *
     * Hasta la 0.9.35 se arbitraba por COPIA ENTERA: se elegia la de radio y se
     * tiraba la de Internet completa. Funcionaba, pero desperdiciaba justo lo
     * que el comodin puede dar — si por radio llegan los lotes 1-4 y el 5 se
     * pierde en una sombra, el 5 estaba en la otra copia y se tiraba con ella.
     *
     * Ahora se compone lote a lote, y hay un hecho que lo hace facil: **las dos
     * copias son la MISMA trama**. El reflector no re-sella nada, asi que el
     * lote 5 por radio y el 5 por Internet llevan identicos `src`, `stream` y
     * `seq`, y el audio Codec2 es el mismo. Juntarlos no es mezclar dos voces:
     * es rellenar una ranura.
     *
     * ⚠️ EL PRECIO ES ESPERAR, y no hay forma de evitarlo: por Internet llega
     * antes, asi que **no se puede saber que el lote 5 de radio va a faltar
     * hasta que ya es tarde**. Por eso se reproduce con `RETRASO_LOTES` de
     * retraso: un lote solo se da por perdido cuando ya han llegado dos
     * posteriores. Dos lotes son ~960 ms, que ademas cubren el jitter del
     * repetidor. Es latencia añadida a una voz que ya llevaba 0,6-1 s de
     * agrupamiento — en un PTT se paga sin dolor.
     *
     * ⚠️ Y LA TRAMPA QUE ESTO PODRIA TRAER, que es la del 10-sep otra vez: si
     * el audio se compone de los dos caminos, **se oye perfecto con la radio
     * muerta y nadie se entera**. Por eso cada ranura recuerda si llego por la
     * antena, y al cerrar la transmision se dice cuantos lotes fueron de radio
     * y cuantos los tapo Internet. El audio se compone; la cuenta, no. */
    private class Ranura(var datos: ByteArray, var porRf: Boolean, var rssi: Int)

    /** Lotes a la espera de que les llegue su turno, por numero de secuencia. */
    private val ventana = HashMap<Int, Ranura>()
    /** El seq que toca reproducir. El INICIO va con seq 0, la voz empieza en 1. */
    private var vSiguiente = 1
    /** El mayor seq recibido: con el se sabe cuando un hueco es definitivo. */
    private var vMayor = 0
    /** Cuando llego el ultimo lote, para poder cerrar si se pierde el FIN. */
    @Volatile private var vUltimo = 0L
    /** Cuando se vio el FIN, o 0 si no ha llegado. Ver el cierre por reloj. */
    @Volatile private var finVisto = 0L
    /** Margen que se le da a los rezagados de radio despues del FIN. Un lote
     *  son 480 ms; con 400 ms se recoge al que venia por el aire sin alargar
     *  la cola de forma perceptible. */
    private val MARGEN_FIN = 400L
    /** Lo pone el usuario en los ajustes; 0 = directo, sin colchón. Se lee en
     *  cada transmisión y no se cachea: cambiarlo tiene efecto en la siguiente
     *  sin reiniciar nada. */
    private val retraso: Int get() = prefs.retrasoLotes

    /* Cuentas de la transmision que se esta oyendo. Separadas a proposito: la
       primera dice cuanto se oyo, la segunda cuanto de eso lo trajo la RADIO. */
    private var rxPorRf = 0
    private var rxTapados = 0
    private var rxLocal = 0
    private var rxHuecos = 0
    /** Ya se ha dicho en pantalla que esta entrando por la antena. */
    private var rxAvisadoRf = false

    /** El seq viaja en un byte y da la vuelta cada 256 lotes (~2 min de
     *  cháchara). Se coloca cerca del que toca: hasta 55 por delante es futuro,
     *  y mas que eso es un lote atrasado, no uno del año que viene. */
    private fun seqAbsoluto(seq: Int): Int {
        val d = ((seq - (vSiguiente and 0xFF)) + 256) % 256
        return if (d > 200) vSiguiente - (256 - d) else vSiguiente + d
    }

    /** Empieza una transmision: la ventana se vacia, venga de donde venga. */
    private fun abreVentana() {
        synchronized(ventana) {
            ventana.clear(); vSiguiente = 1; vMayor = 0; vUltimo = System.currentTimeMillis()
        }
        finVisto = 0L
        rxPorRf = 0; rxTapados = 0; rxLocal = 0; rxHuecos = 0; rxAvisadoRf = false
    }

    /** Mete un lote en su ranura. **La copia de RADIO asciende a la que ya
     *  hubiera**: llega mas tarde, pero es la que trae la medida de señal y la
     *  que demuestra que la antena esta oyendo. Por eso el retraso de dos lotes
     *  no es solo para rellenar huecos — tambien le da tiempo a la radio a
     *  llegar antes de que se cuente la ranura. */
    private fun encaja(p: ByteArray) {
        val rssi = p[0].toInt()
        val porRf = porRadio(rssi)
        synchronized(ventana) {
            val s = seqAbsoluto(p[6].toInt() and 0xFF)
            if (s < vSiguiente) return          // tarde: su turno ya paso
            if (s > vMayor) vMayor = s
            vUltimo = System.currentTimeMillis()
            val vieja = ventana[s]
            if (vieja == null) {
                ventana[s] = Ranura(p.copyOf(), porRf, rssi)
            } else if (porRf && !vieja.porRf) {
                vieja.datos = p.copyOf(); vieja.porRf = true; vieja.rssi = rssi
            }
        }
    }

    /** Un solo hilo decodificando. `bombea` la llama el hilo que lee del nodo,
     *  pero tambien el del latido cuando cierra por silencio una transmision a
     *  la que se le perdio el FIN: dos hilos dentro del mismo decoder de
     *  Codec2 —que es codigo nativo y no es reentrante— es un cuelgue esperando
     *  a pasar. El orden de los cerrojos es siempre este y luego `ventana`. */
    private val cerrojoRx = Any()

    /** Suelta lo que ya se puede dar por completo. Con `forzar` vacia entera,
     *  que es lo que toca en el FIN. */
    private fun bombea(forzar: Boolean) = synchronized(cerrojoRx) {
        while (true) {
            var r: Ranura? = null
            var hueco = false
            synchronized(ventana) {
                if (vSiguiente > vMayor) return
                if (!forzar && vMayor < vSiguiente + retraso) return
                r = ventana.remove(vSiguiente)
                hueco = (r == null)
                vSiguiente++
            }
            val lote = r
            if (lote == null) {
                /* Ni la radio ni Internet lo trajeron. No es lo mismo que un
                   lote tapado por el comodin, y por eso se cuentan aparte. */
                rxHuecos++
                /* ⚠️ EN EL VACIADO FINAL NO SE TAPA NADA. Al llegar el FIN se
                   suelta lo que quede, y los seq que nunca llegaron se cuentan
                   como huecos — pero ahi no falta nada: **es que la
                   transmision se ha acabado**. Rellenarlos metia medio segundo
                   de arrastre al final de cada frase. */
                if (hueco && !forzar) tapaHueco()
            } else {
                if (lote.porRf) {
                    rxPorRf++
                    if (lote.rssi < 126) {
                        if (rxRssiMin >= 126 || lote.rssi < rxRssiMin) rxRssiMin = lote.rssi
                        if (rxRssiMax >= 126 || lote.rssi > rxRssiMax) rxRssiMax = lote.rssi
                    }
                    /* El INICIO puede haber llegado por Internet —llega antes—
                       y haber pintado 🌐 aunque la voz venga por la antena. En
                       cuanto entra el primer lote de radio se corrige, que el
                       icono es lo unico que dice si hay cobertura. */
                    if (!rxAvisadoRf) {
                        rxAvisadoRf = true
                        rxQuien?.let { observador?.onQuienHabla(it, lote.rssi) }
                    }
                } else if (mismoNodo(lote.rssi)) {
                    /* Otro cliente de mi propio nodo. Ni radio ni Internet:
                       no dice nada de la cobertura y por eso va aparte. */
                    rxLocal++
                } else {
                    rxTapados++
                }
                rxLotes++
                reproduceLote(lote.datos)
            }
        }
    }

    /** La última trama que sonó, para poder tapar un hueco con ella. */
    private var ultimoPcm: ShortArray? = null

    /** UN LOTE QUE NO LLEGO NO PUEDE SONAR A CORTE SECO.
     *
     *  Dejar el hueco en silencio suena peor de lo que es: medio segundo de
     *  nada en mitad de una palabra se oye como si el sistema fallara, y ademas
     *  deja al altavoz sin datos, que es cuando aparecen los chasquidos. Lo que
     *  hace cualquier codec de voz ante una perdida es continuar con lo ultimo
     *  que tenia y desvanecerlo: no inventa voz, pero no rompe la frase. */
    private fun tapaHueco() {
        val ult = ultimoPcm ?: return
        val a = audio ?: return
        var g = 0.6f
        /* ⚠️ CORTO. Un relleno largo no disimula: se oye.
           Aqui se repetia la ultima trama `tramasPorLote` veces —480 ms a 40 ms
           por trama—, y eso no suena a voz sino a un zumbido que se apaga y una
           frase que se reanuda despues. Lo que hacen los codecs de voz es tapar
           el borde, no el hueco entero: dos o tres tramas bastan para que no
           haya chasquido, y el resto es mejor en silencio. */
        for (i in 0 until PLC_TRAMAS) {
            val copia = ShortArray(ult.size)
            for (j in ult.indices) copia[j] = (ult[j] * g).toInt().toShort()
            a.play(copia)
            g *= 0.45f
        }
    }

    /** Cuántas tramas se repiten para tapar el borde de un hueco. Tres a 40 ms
     *  son 120 ms: quita el chasquido y no se nota como arrastre. */
    private val PLC_TRAMAS = 3

    /** ¿Es mi propia voz dando la vuelta? Mi transmision vuelve por el otro
     *  camino con el `src` del nodo por el que paso, pero con MI indicativo:
     *  por eso se ataja por quien habla y no por origen. Sin mayusculas y sin
     *  espacios de sobra, que el nodo lo puede haber recortado a
     *  MAX_INDICATIVO. */
    private fun esMiVoz(quien: String) =
        quien.equals(prefs.indicativo.trim(), ignoreCase = true)

    /** Como se enseña el origen de algo que ha llegado. Es la mitad de lo que
     *  el usuario quiere ver: no basta con oírlo, hay que saber **por dónde**
     *  vino — si empieza a entrar todo por Internet teniendo al otro cerca, es
     *  que la radio ha dejado de llegar, y ese aviso no existía. */
    private fun comoLlego(rssi: Int) =
        if (porInternet(rssi)) "🌐 Internet"
        else if (mismoNodo(rssi)) "👥 mismo nodo"
        else "📻 RF $rssi dBm"
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
                /* 200 ms y no dos segundos: quien cierra una recepcion es
                   este bucle (ver mas abajo), y con dos segundos de resolucion
                   el margen de 400 ms tras el FIN se convertia en hasta dos
                   segundos y medio de cola. Todo lo demas que hay aqui va por
                   marca de tiempo, no por vueltas, asi que acelerarlo no cambia
                   ninguna cadencia: solo afina el cierre. */
                try { Thread.sleep(200) } catch (e: InterruptedException) { return@Thread }
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
                    /* AVISAR CUANDO SE PIERDE Y CUANDO VUELVE LA RADIO.
                       Sin esto, quedarse sin cobertura de RF es invisible: se
                       sigue oyendo a todo el mundo por el camino de datos y
                       parece que va bien, mientras lo que tú dices no lo oye
                       nadie. Es el aviso más importante que da esta app. */
                    /* QUIEN CIERRA UNA RECEPCION ES EL RELOJ.
                       Dos motivos para cerrar, y ninguno es "ha llegado el FIN":
                        · con FIN: se espera `MARGEN_FIN` a los rezagados —por
                          Internet el FIN adelanta a los ultimos lotes de radio,
                          y esos lotes son voz que si no se pierde—;
                        · sin FIN: el FIN se pierde como cualquier otra trama
                          (esa cicatriz ya esta en el firmware, que cierra por
                          silencio a los 5 s), asi que 2 s sin un lote tambien
                          cierran. Dos segundos son cuatro lotes: no se confunde
                          con un hueco de cobertura. */
                    if (rxQuien != null) {
                        val porFin = finVisto > 0L && ahora - finVisto > MARGEN_FIN
                        val porSilencio = vUltimo > 0L && ahora - vUltimo > 2000
                        if (porFin || porSilencio) {
                            bombea(true)
                            cierraRx()
                        }
                    }
                    /* ⚠️ VIGIA DEL PTT TRABADO. Un micrófono que se cierra
                       mal, o un enlace que se cae en mitad de la pulsación,
                       podían dejar la transmisión abierta para siempre: el FIN
                       sin mandar y el canal ocupado para toda la red. Que el
                       cierre dependa de que algo haya salido bien no vale; esto
                       es el suelo. Un segundo y medio es de sobra para una cola
                       de 250 ms. */
                    if (cerrando && tCerrando > 0L && ahora - tCerrando > 1500) {
                        apunta("⚠️ el cierre no llegó: se fuerza")
                        cierraTx()
                    }

                    /* El micro, listo ANTES de que se pulse. Ver
                       `AudioEngine.preparaMicro`: montarlo dentro del PTT
                       costaba las primeras palabras. */
                    if (audio == null) preparaMicroYa()

                    val radio = hayRadio()
                    if (radio != radioAvisada) {
                        radioAvisada = radio
                        if (radio) {
                            apunta("📻 vuelve la radio")
                            observador?.onLog("vuelve la radio")
                        } else {
                            apunta("⚠️ SIN RADIO: no entra nada por la antena. " +
                                   if (prefs.datosActivo)
                                       "Se sigue hablando por Internet."
                                   else
                                       "Lo que digas NO LO OYE NADIE.")
                            observador?.onLog("sin radio")
                        }
                    }
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
            /* ⚠️ EL ESTADO QUE SE PINTA ES EL DE LOS DOS CAMINOS, NO EL DE ESTE.
               Aquí se anunciaba solo el resultado del enlace de RADIO, así que
               con el nodo fuera de alcance cada reintento escribía «sin enlace»
               aunque el camino de datos estuviera perfectamente en pie — y el
               otro callback escribía «solo datos» un momento después. El
               indicador iba y venía sin que nada cambiara de verdad, y desde
               fuera parecía que el modo red entraba y salía solo.
               Perder la radio NO es quedarse sin enlace mientras haya comodín:
               eso es justo lo que el comodín viene a evitar. */
            val hayAlgo = ok || datosEnlazado
            observador?.onEnlace(hayAlgo,
                if (ok) detalle
                else if (datosEnlazado) "solo datos · $detalle"
                else detalle)
            notifica(if (ok) "Enlazado con $detalle"
                     else if (datosEnlazado) "Solo datos" else detalle)
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
                                 if (enlazado) "radio"
                                 else if (ok) "solo datos"
                                 else "sin enlace")
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
    /* POR DONDE HA SALIDO LO QUE ACABO DE DECIR. Se apunta al vuelo porque es
     * lo unico que contesta la pregunta que de verdad importa al soltar el PTT:
     * ¿me ha oido alguien por la antena, o he salido solo por el comodin? */
    @Volatile private var txPorRf = false
    @Volatile private var txPorNet = false

    private fun mandaPorTodos(tipo: Int, datos: ByteArray = ByteArray(0)) {
        if (enlazado) { nodo?.manda(tipo, datos); txPorRf = true }
        /* Y POR DATOS TAMBIÉN CUANDO LA RADIO NO LLEGA.
         *
         * Esto faltaba, y costó once segundos de voz al vacío. La regla es «por
         * Internet si no pasa por RF», y **un nodo conectado pero fuera de
         * alcance es exactamente eso**: se emite por la antena y no lo oye
         * nadie. Antes sólo se miraba si había nodo, que es otra cosa.
         *
         * Cuando la radio está muda se manda por LOS DOS y no sólo por datos:
         * que no oigamos a la celda no demuestra que no nos oiga nadie —puede
         * haber alguien cerca— y aquí es preferible duplicar que perder la
         * transmisión. Mientras la radio va bien, esto no se dispara y sigue
         * saliendo sólo por RF. */
        if (prefs.datosActivo && !hayRadio()) {
            nodoDatos?.manda(tipo, datos)
            txPorNet = true
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

    /** Reinicia el nodo enlazado. Tarda ~20 s en volver; la reconexión es
     *  automática, así que no hay que hacer nada más. */
    fun reiniciaNodo() {
        apunta("reiniciando el nodo…")
        nodo?.reinicia()
    }

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
        /* Cualquier cosa que entre por la antena mantiene viva la radio. Se
           mira ANTES de descartar duplicados: la copia de RF puede ser
           justamente la que se tire por repetida, y aun así demuestra que la
           antena está oyendo. */
        if (p.size >= 1 && (tipo == Nodo.EV_INICIO || tipo == Nodo.EV_VOZ ||
                            tipo == Nodo.EV_HOLA) && porRadio(p[0].toInt())) {
            ultimoRf = System.currentTimeMillis()
        }
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
                /* UN SOLO INICIO POR TRANSMISION, venga por donde venga. Las
                   dos copias son la misma trama —mismo `src`, `stream` y
                   `seq`—, asi que aqui el dedupe SI puede ser ciego al camino:
                   lo que hay que evitar es reabrir la transmision, no elegir
                   copia. Quien reparte radio e Internet es la ventana, lote a
                   lote. */
                if (!nuevo("I:%02x%02x%02x:%d".format(
                        p[2], p[3], p[4], p[5].toInt() and 0xFF))) return
                /* ⚠️ EL INDICATIVO SE CORTA EN EL PRIMER CERO. Desde el
                   firmware v1.35, detrás puede venir `\0` + 8 bytes con la
                   POSICIÓN de quien habla (la de una persona sale aquí, no en
                   la baliza). Sin este corte el indicativo se lee con la cola
                   binaria pegada, y eso no es sólo una etiqueta fea: **el
                   filtro del eco compara el indicativo con el mío**, dejaría de
                   reconocerse y volvería a oírse uno mismo. */
                val bruto = String(p, 7, p.size - 7, Charsets.US_ASCII)
                val ind = bruto.substringBefore('\u0000').trim()
                if (esMiVoz(ind)) return          // mi propio eco
                abreVentana()
                reproduce(tipo, p)
            }
            Nodo.EV_VOZ -> if (p.size > 9) {
                /* Aqui NO se descarta por duplicado: las dos copias del mismo
                   lote hacen falta, porque de las dos sale una sola ranura y la
                   de radio asciende a la de Internet. Ver `encaja`. */
                /* Sin INICIO no se abre nada. El INICIO llega por los DOS
                   caminos, asi que perderlo entero es raro; y abrir a ciegas
                   costaria el filtro del eco —que compara el indicativo, y el
                   indicativo solo viaja en el INICIO—, o sea oirse uno mismo. */
                if (rxQuien == null) return
                encaja(p)
                bombea(false)
            }
            Nodo.EV_FIN -> {
                /* ⚠️ EL FIN NO CIERRA LA RECEPCION, SOLO ANOTA QUE NO HABRA MAS.
                   Vaciar aqui mismo se comia el final de cada frase, y por una
                   razon de fondo: **el FIN viaja por los dos caminos y por
                   Internet llega ANTES que los ultimos lotes de radio**. Al
                   vaciar de golpe, esos lotes llegaban cuando su turno ya habia
                   pasado y se tiraban. El cierre lo manda el reloj, no el FIN:
                   se le da un margen a los rezagados y luego se cierra. */
                if (p.size >= 4 && !nuevo("F:%02x%02x%02x:%d".format(
                        p[0], p[1], p[2], p[3].toInt() and 0xFF))) return
                if (rxQuien == null) return
                finVisto = System.currentTimeMillis()
                bombea(false)      // lo que ya se pueda dar por completo, ya
            }
            Nodo.EV_HOLA -> if (p.size >= 9) {
                /* ⚠️ UNA BALIZA SOLO SIGNIFICA ALGO POR RADIO, y por eso la que
                   viene por Internet **no se enseña**.
                   Una baliza dice "estoy aqui y me oyes": por la linea eso no
                   demuestra ni un dB, y la lista de nodos a la vista es
                   justamente la de quien te oye. Enseñarlas mezcladas convierte
                   la unica herramienta que hay para juzgar la cobertura en un
                   listado de quien tiene Internet. El firmware v1.40 ya no las
                   entrega, pero por el camino de datos (4460) siguen llegando y
                   ademas hay que cubrir los nodos sin actualizar. */
                if (!porRadio(p[0].toInt())) return
                /* UNA BALIZA, UNA LÍNEA. Aun por radio llegan repetida y
                   original; la ventana de 8 s las junta sin tocar las de
                   verdad, que van cada minuto. */
                if (!nuevo("H:%02x%02x%02x".format(p[2], p[3], p[4]))) return
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
                /* La batería sólo se enseña si el nodo la sabe. Un 0 significa
                   «no tengo de dónde leerla» y enseñarlo como «batería 0%»
                   asusta sin motivo. */
                val bat = p[7].toInt() and 0xFF
                val pila = if (bat > 0) " · batería $bat%" else ""
                apunta("baliza $quien · $papel$pila · 📻 ${p[0].toInt()} dBm")
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
    /** Deja el códec y el micro montados por adelantado, para que la
     *  pulsación no tenga que esperar a nada. */
    /** Vuelca los ajustes del micrófono al motor. Se llama antes de cada
     *  pulsación: así un cambio en Ajustes tiene efecto en la siguiente sin
     *  reiniciar nada, que es lo que hace falta para poder ajustar la ganancia
     *  probando y escuchando. */
    private fun aplicaMicro() {
        val a = audio ?: return
        a.micGain = AudioEngine.MIC_GANANCIAS.getOrElse(prefs.micGanancia) { 1.0f }
        a.agc = prefs.micAgc
        // La fuente sólo se puede cambiar al abrir el micro, no en caliente.
        if (a.micCrudo != prefs.micCrudo) {
            a.micCrudo = prefs.micCrudo
            a.sueltaMicro()          // se reabrirá con la fuente nueva
        }
    }

    private fun preparaMicroYa() {
        val modo = prefs.modo
        if (encoder?.modo != modo) {
            encoder?.cierra()
            encoder = Codec2.abre(modo)
        }
        val e = encoder ?: return
        if (audio == null || audio?.FRAME != e.muestrasPorTrama) {
            audio?.release()
            audio = AudioEngine(this, e.muestrasPorTrama, ::tramaCapturada)
        }
        audio?.preparaMicro()
    }

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
    /* ─────────────── ABRIR Y CERRAR LA RECEPCION ───────────────
     * Los lotes no pasan por aqui: van a la ventana y salen por `bombea`. */
    private fun reproduce(tipo: Int, p: ByteArray) {
        when (tipo) {
            Nodo.EV_INICIO -> {
                val rssi = p[0].toInt()
                val modo = p[6].toInt() and 0xFF
                val bruto = String(p, 7, p.size - 7, Charsets.US_ASCII)
                val ind = bruto.substringBefore('\u0000').trim()
                abreDecoder(modo)
                ultimoQueHabla = ind
                apuntaHablante(ind, rssi)
                observador?.onQuienHabla(ind, rssi)
                apunta("▼ %s · %s".format(ind, comoLlego(rssi)))
                /* Empieza otro: lo que quedara de la transmision anterior sobra
                   —si alguien pisa, se oye al que entra, no una mezcla—. Y este
                   es el sitio, no el cierre: ver la nota de `cierraRx`. */
                audio?.resetPlayback()
                preparaAudioRx()
                rxQuien = ind; rxLotes = 0; rxModo = modo; rxT0 = System.currentTimeMillis()
                rxRssiMin = rssi; rxRssiMax = rssi
            }
            Nodo.EV_FIN -> cierraRx()
        }
    }

    /** Cierra la recepcion y deja escrito de que vivio esta transmision.
     *
     *  ⚠️ ESTA LINEA ES LA MITAD DEL ARREGLO. Si el audio se compone de los dos
     *  caminos, se oye igual de bien con la radio muerta — y entonces nadie se
     *  entera de que la cobertura se ha caido, que es como empezo todo el
     *  10-sep. El audio se compone; la cuenta, NO: aqui se dice cuantos lotes
     *  trajo la antena y cuantos los tapo Internet. */
    private fun cierraRx() = synchronized(cerrojoRx) {
        /* ⚠️ AQUI NO SE VACIA LA COLA DEL ALTAVOZ. Estaba, y se comia el final
           de cada frase: al llegar el FIN, `bombea(true)` acaba de soltar los
           ultimos dos o tres lotes —los que la ventana tenia retenidos— y
           borrarlos justo despues los tira sin sonar. Antes no se notaba
           porque sin ventana cada lote ya habia sonado al llegar.
           Vaciar la cola tiene sentido al ABRIR una recepcion nueva, para que
           no se mezcle con la anterior, y ahi es donde esta ahora. */
        rxQuien?.let { q ->
            val seg = (System.currentTimeMillis() - rxT0) / 1000.0
            val señal = if (rxLocal > 0 && rxPorRf == 0) "sin salir del nodo"
                        else if (rxRssiMin >= 126 || rxPorRf == 0) "sin radio"
                        else if (rxRssiMin == rxRssiMax) "${rxRssiMax} dBm"
                        else "${rxRssiMax}..${rxRssiMin} dBm"
            val tapados = if (rxTapados > 0) " · 🌐 $rxTapados tapados" else ""
            val local = if (rxLocal > 0) " · 👥 $rxLocal del nodo" else ""
            val huecos = if (rxHuecos > 0) " · $rxHuecos perdidos" else ""
            /* Con todo del mismo nodo, hablar de radio no tiene sentido: no ha
               salido al aire, así que no se anuncia una cobertura que no se ha
               puesto a prueba. */
            val cuenta = if (rxLocal > 0 && rxPorRf == 0 && rxTapados == 0)
                             "👥 $rxLocal del nodo$huecos"
                         else "📻 $rxPorRf$tapados$local$huecos"
            apunta(("◀ $q · $rxLotes lotes ($cuenta) · %.1f s · " +
                    "$señal · ${Codec2.NOMBRES.getOrElse(rxModo) { "?" }}").format(seg))
            cierraFila(seg, rxPorRf > 0, rxRssiMin)
        }
        rxQuien = null
        finVisto = 0L
        synchronized(ventana) { ventana.clear(); vSiguiente = 1; vMayor = 0 }
    }

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
            ultimoPcm = pcm.copyOf()      // por si el siguiente lote no llega
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
        /* ⚠️ PRIMERO EL MICRO Y LUEGO EL INICIO, no al revés.
           Estaba puesto `transmitiendo = true` y el INICIO ANTES de abrir la
           captura: si el micro no arrancaba, la red se quedaba con una
           transmisión abierta que no iba a llevar ni una trama de voz, y el
           canal ocupado para todos. Si no hay micro no hay nada que transmitir,
           así que no se abre nada. */
        aplicaMicro()
        audio?.reiniciaRecorte()
        if (audio?.startCapture() != true) {
            observador?.onLog("no se pudo abrir el micrófono")
            apunta("⚠️ el micrófono no abre: no se transmite")
            return
        }
        transmitiendo = true
        t0Tx = System.currentTimeMillis()
        txPorRf = false; txPorNet = false
        mandaPorTodos(Nodo.CMD_INICIO, byteArrayOf(modo.toByte()))
        Vibra.buzz(this, 30)
        observador?.onTx(true)
        notifica("Transmitiendo")
    }

    /** Cuanto se sigue leyendo el micro despues de soltar. No es grabar de
     *  mas: es terminar de leer lo que YA se habia grabado y estaba en el
     *  buffer. Ver `AudioEngine.stopCapture`. */
    private val COLA_PTT_MS = 250

    /** Cuando se pidio el cierre, para poder forzarlo si no llega. */
    @Volatile private var tCerrando = 0L

    fun sueltaPtt() {
        if (!transmitiendo) return
        transmitiendo = false
        cerrando = true                    // las tramas de la cola aun cuentan
        tCerrando = System.currentTimeMillis()
        audio?.stopCapture(COLA_PTT_MS) { cierraTx() }
        observador?.onTx(false)
        notifica(if (enlazado) "Enlazado"
                 else if (datosEnlazado) "Solo datos"
                 else "Sin enlace")
    }

    /** El cierre de verdad, ya con la cola del micro vaciada. Lo llama el hilo
     *  de captura, no quien suelta el PTT: asi nadie espera. */
    private fun cierraTx() {
        if (!cerrando) return              // ya se cerro (o lo forzo el vigia)
        cerrando = false
        tCerrando = 0L
        // Lo que quede a medias se manda igual: cortar una sílaba por no
        // completar el lote se nota más que medio lote corto.
        if (loteTramas > 0) mandaLote()
        mandaPorTodos(Nodo.CMD_FIN)
        /* ⚠️ Y AQUI SE DICE POR DONDE HA SALIDO.
           El aviso de «SIN RADIO» se escribe solo cuando CAMBIA el estado —para
           no llenar el registro—, asi que a los cinco minutos de perder la
           cobertura ya no hay nada en pantalla que lo recuerde y cada
           transmision parece normal. Esa es exactamente la ceguera que costo
           once segundos de voz al vacio el 9-sep. En recepcion ya se ve
           (`📻 22 · 🌐 2 tapados`); en transmision faltaba, y es donde mas
           importa: en recepcion te enteras de que no oyes, pero que no te oigan
           no lo notas nunca. */
        val via = when {
            txPorRf && txPorNet -> "📻+🌐 sin radio: también por Internet"
            txPorNet -> "🌐 sólo Internet"
            txPorRf -> "📻 radio"
            else -> "⚠️ por ningún sitio"
        }
        val seg = (System.currentTimeMillis() - t0Tx) / 1000.0
        apunta("▶ tú · %.1f s · %s · %s".format(
                   seg, Codec2.NOMBRES.getOrElse(prefs.modo) { "?" }, via))
        /* ⚠️ Y SI EL MICRO HA SATURADO, SE DICE. Es la diferencia entre "el
           códec suena mal" y "te estás comiendo el micro", que desde fuera se
           oyen igual — y hasta ahora no había forma de distinguirlas: uno
           acababa culpando al códec, a la radio o al colchón. */
        val rec = audio?.porcentajeRecorte() ?: 0.0
        if (rec >= 0.5) {
            apunta("⚠️ micrófono SATURANDO (%.1f%% recortado): sepárate un palmo "
                   .format(rec) + "o baja la ganancia en Ajustes → Micrófono")
            observador?.onLog("micrófono saturando (%.1f%%)".format(rec))
        }
        apuntaLoMio(seg, txPorRf)
    }

    /** true mientras se vacia la cola del micro tras soltar: la transmision
     *  ya no acepta PTT, pero esas tramas son voz de verdad y van dentro. */
    @Volatile private var cerrando = false

    private fun tramaCapturada(pcm: ShortArray) {
        val e = encoder ?: return
        if (!transmitiendo && !cerrando) return
        if (transmitiendo && System.currentTimeMillis() - t0Tx > TOT_S * 1000L) {
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
            /* ⚠️ ICONO PROPIO, y hace falta. Estaba puesto
               `stat_sys_speakerphone`, que es el icono DEL SISTEMA para el
               altavoz del telefono: en la barra de estado se confundia con una
               llamada en curso. Este es el CHIRP de LoRa —la rampa de
               frecuencia que la radio emite de verdad— y no se parece a nada
               que lleve un movil. */
            .setSmallIcon(R.drawable.ic_stat_lora)
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
