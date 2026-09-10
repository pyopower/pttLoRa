package c31ag.pttlora

import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.os.Build
import android.util.Log
import java.io.InputStream
import java.io.OutputStream
import java.util.UUID
import java.util.concurrent.ArrayBlockingQueue

/**
 * Enlace con un nodo PTT LoRa, hablando KISS.
 *
 * Dos tubos, el mismo protocolo por los dos: **Bluetooth BLE** (un usuario por
 * nodo hoy por hoy) y **WiFi/TCP** (hasta ocho, cada uno con su indicativo). Se
 * elige por lo que haga falta: el Bluetooth no necesita ninguna red, el WiFi
 * permite que un nodo dé servicio a un grupo.
 *
 * ⚠️ **BLE desde la v0.9.16, y hace falta firmware 1.26 o más nuevo en el
 * nodo.** Antes era SPP (`BluetoothSocket`), y se cambió porque SPP no daba más
 * de sí: la pila del ESP32 se reiniciaba sola al quedarse sin memoria, admitía
 * un cliente y no se enteraba de que se había ido, y el emparejamiento de
 * Android abría sesiones fantasma que dejaban a la app fuera. Todo lo
 * específico de BLE vive en [EnlaceBle]; aquí abajo no cambia nada, porque el
 * protocolo KISS es el mismo y sigue viajando por un par de flujos de bytes.
 *
 * Es el mismo protocolo que el nodo saca por USB, así que `tools/nodo.py` de la
 * raspi y esta clase hacen exactamente lo mismo: sirve de referencia cruzada
 * cuando algo no cuadra.
 *
 * NUNCA se escribe en el socket desde el hilo principal. Es la lección que
 * costó un día en la app PTT: el PTT y el keepalive salían del hilo de UI, y
 * `NetworkOnMainThreadException` se comía la conexión con un síntoma que
 * parecía de red. Aquí hay una cola y un único hilo escritor.
 */
class Nodo(private val onEvento: (Int, ByteArray) -> Unit,
           private val onEstado: (Boolean, String) -> Unit,
           private val ctx: android.content.Context? = null) {

    /** Para dejar constancia de por dónde entró (o por qué no). Lo pone el
     *  servicio para que acabe en el registro que el usuario puede copiar. */
    var onDiagnostico: ((String) -> Unit)? = null

    companion object {
        private const val TAG = "pttlora.nodo"

        const val FEND = 0xC0
        const val FESC = 0xDB
        const val TFEND = 0xDC
        const val TFESC = 0xDD

        // ordenes hacia el nodo
        const val CMD_INICIO = 0x01
        const val CMD_VOZ = 0x02
        const val CMD_FIN = 0x03
        const val CMD_CONFIG = 0x04
        const val CMD_ESTADO = 0x05
        const val CMD_WIFI = 0x08
        const val CMD_CONFIRMA = 0x09
        const val CMD_RADIO = 0x0A
        const val CMD_IDENT = 0x0E
        const val CMD_RED = 0x0F
        const val CMD_ENLACE = 0x10
        const val CMD_POS = 0x17
        const val CMD_REINICIA = 0x18
        /** Origen de la posición: viva (la del móvil, caduca y no se guarda)
         *  o fija (la de una celda, se guarda en el nodo). */
        const val POS_VIVA = 0
        const val POS_FIJA = 1

        // eventos que llegan del nodo
        const val EV_INICIO = 0x81
        const val EV_VOZ = 0x82
        const val EV_FIN = 0x83
        const val EV_HOLA = 0x84
        const val EV_ESTADO = 0x85
        const val EV_CANAL = 0x86
        const val EV_EMPAREJA = 0x87
        const val EV_PTT = 0x89
        const val EV_RED = 0x8A
        const val EV_OTA = 0x88
        const val EV_LOG = 0x8F

        /** Estados de EV_PTT. */
        const val PTT_LIBRE = 0
        const val PTT_TUYO = 1
        const val PTT_DE_OTRO = 2

        /** Modos de red del nodo (CMD_RED). */
        const val RED_OFF = 0
        const val RED_CLIENTE = 1
        const val RED_AP = 2

        /** Puertos del nodo. Dos, porque por ellos van dos protocolos
         *  distintos: órdenes en uno, tramas del aire entre nodos en el otro. */
        const val PUERTO_APP = 4460
        const val PUERTO_ENLACE = 4461
        const val PUERTO_BUSCA = 4462

        /** Dirección del punto de acceso que levanta un nodo. La fija el
         *  ESP32 y no es configurable, así que la app puede ofrecerla hecha. */
        const val IP_AP = "192.168.4.1"

        /** Los nodos se llaman así; sirve para filtrar la lista de emparejados. */
        const val PREFIJO = "PTTLoRa"
    }

    /** LA RED WiFi DEL MOVIL, EXPLICITAMENTE.
     *
     *  Sin esto, un socket a `192.168.1.x` puede salir **por los datos
     *  moviles**: Android manda el trafico por su red "por defecto", y esa es
     *  la que tiene Internet — si el WiFi de casa no lo tiene, o el movil
     *  prefiere los datos, el nodo queda inalcanzable y sale un
     *  `SocketException` que parece del nodo y es de encaminamiento. Es el
     *  fallo clasico de cualquier app que habla con un cacharro de la red
     *  local, y le pasa a mas gente con el WiFi propio del nodo (que no da
     *  Internet) que con el de casa.
     *
     *  Se busca una red con transporte WiFi y se ata el socket a ella. Si no
     *  hay ninguna, se sigue como siempre: peor es no intentarlo. */
    private fun redWifi(): android.net.Network? {
        if (android.os.Build.VERSION.SDK_INT < 21) return null
        val c = ctx ?: return null
        return try {
            val cm = c.getSystemService(android.content.Context.CONNECTIVITY_SERVICE)
                     as android.net.ConnectivityManager
            cm.allNetworks.firstOrNull { n ->
                cm.getNetworkCapabilities(n)?.hasTransport(
                    android.net.NetworkCapabilities.TRANSPORT_WIFI) == true
            }
        } catch (e: Exception) { Log.w(TAG, "red wifi: ${e.message}"); null }
    }

    /** Lo que hay que cerrar para soltar el enlace, sea del tipo que sea. */
    private var cerrable: java.io.Closeable? = null
    private var salida: OutputStream? = null
    private var lector: Thread? = null
    private var escritor: Thread? = null
    private val cola = ArrayBlockingQueue<ByteArray>(64)
    @Volatile private var vivo = false
    @Volatile var conectado = false; private set
    /** Cuándo llegó el último byte del nodo. Un enlace que no dice nada en
     *  mucho tiempo está muerto aunque el socket siga abierto — ver el
     *  vigilante del servicio. */
    @Volatile var ultimoDato = 0L; private set

    /** TODOS los emparejados, con los que parecen nodos primero.
     *
     *  No se filtra por nombre: si el movil tiene el nodo guardado con un
     *  nombre antiguo en cache, filtrarlo lo esconderia justo cuando hace
     *  falta encontrarlo.
     *
     *  ⚠️ CON BLE ESTO YA NO ES EL CAMINO, pero se deja porque **sigue dando la
     *  MAC buena**: el ESP32 usa la MISMA direccion para Bluetooth Classic y
     *  para BLE, asi que un nodo emparejado en la epoca del SPP aparece aqui
     *  con la direccion que hay que usar para conectar por BLE. Para un nodo
     *  nuevo no hace falta emparejar nada: se busca con `busca()`. */
    fun emparejados(): List<BluetoothDevice> {
        val ad = BluetoothAdapter.getDefaultAdapter() ?: return emptyList()
        return try {
            ad.bondedDevices.orEmpty().sortedWith(
                compareByDescending<BluetoothDevice> {
                    (it.name ?: "").startsWith(PREFIJO)
                }.thenBy { it.name ?: it.address })
        } catch (e: SecurityException) { emptyList() }   // falta BLUETOOTH_CONNECT
    }

    /** Busca nodos por radio. **Escaneo BLE**, no descubrimiento clásico.
     *
     *  Emparejar no hace falta y nunca hizo: la puerta del nodo es el código de
     *  6 cifras que enseña en su pantalla, que no tiene nada que ver con el
     *  emparejamiento de Android. Verlos juntos era justo lo que despistaba.
     *
     *  Se filtra por el UUID del servicio, así que aquí solo aparecen nodos de
     *  PTT LoRa — se acabó la lista con los auriculares del vecino. Y en BLE el
     *  nombre **sí viene en el anuncio**, así que no hay que esperar segundos a
     *  que Android lo pregunte aparte, que era media complicación de antes.
     *
     *  Devuelve un testigo que hay que pasarle luego a [paraBusqueda].
     *
     *  Dos APIs porque esto llega hasta Android 4.4: el escáner moderno es de
     *  Android 5 en adelante, y por debajo solo está `startLeScan`, que ni
     *  filtra por UUID de servicio de forma fiable — por eso ahí se filtra por
     *  el nombre. */
    fun busca(ctx: android.content.Context,
              alEncontrar: (BluetoothDevice, String?) -> Unit,
              alTerminar: () -> Unit): Any? {
        val ad = BluetoothAdapter.getDefaultAdapter() ?: return null
        if (!ad.isEnabled) { alTerminar(); return null }

        /* El escaneo NO se para solo: se para a los 15 s o cuando el usuario
           cierre el diálogo. Un escaneo BLE olvidado se come la batería, y
           Android 7+ además castiga a quien arranca y para muchas veces
           seguidas dejándolo sin escanear cinco minutos. */
        val ui = android.os.Handler(android.os.Looper.getMainLooper())

        try {
            if (Build.VERSION.SDK_INT >= 21) {
                val esc = ad.bluetoothLeScanner ?: run { alTerminar(); return null }
                val cb = object : android.bluetooth.le.ScanCallback() {
                    override fun onScanResult(tipo: Int,
                                              r: android.bluetooth.le.ScanResult) {
                        val n = try { r.scanRecord?.deviceName ?: r.device.name }
                                catch (e: SecurityException) { null }
                        alEncontrar(r.device, if (n.isNullOrBlank()) null else n)
                    }
                    override fun onScanFailed(codigo: Int) {
                        Log.w(TAG, "escaneo BLE falló: $codigo")
                        alTerminar()
                    }
                }
                val filtro = android.bluetooth.le.ScanFilter.Builder()
                    .setServiceUuid(android.os.ParcelUuid(EnlaceBle.UUID_SERV))
                    .build()
                val ajustes = android.bluetooth.le.ScanSettings.Builder()
                    .setScanMode(android.bluetooth.le.ScanSettings.SCAN_MODE_LOW_LATENCY)
                    .build()
                esc.startScan(listOf(filtro), ajustes, cb)
                ui.postDelayed({ alTerminar() }, 15000)
                return cb
            } else {
                @Suppress("DEPRECATION")
                val cb = BluetoothAdapter.LeScanCallback { dev, _, _ ->
                    val n = try { dev.name } catch (e: SecurityException) { null }
                    // Sin filtro por servicio: en 4.4 se filtra por el nombre.
                    if (n != null && n.startsWith(PREFIJO)) alEncontrar(dev, n)
                }
                @Suppress("DEPRECATION")
                ad.startLeScan(cb)
                ui.postDelayed({ alTerminar() }, 15000)
                return cb
            }
        } catch (e: SecurityException) {
            Log.w(TAG, "sin permiso para buscar: ${e.message}")
            alTerminar()
            return null
        }
    }

    fun paraBusqueda(ctx: android.content.Context, testigo: Any?) {
        if (testigo == null) return
        val ad = BluetoothAdapter.getDefaultAdapter() ?: return
        try {
            if (Build.VERSION.SDK_INT >= 21 &&
                testigo is android.bluetooth.le.ScanCallback) {
                ad.bluetoothLeScanner?.stopScan(testigo)
            } else if (testigo is BluetoothAdapter.LeScanCallback) {
                @Suppress("DEPRECATION")
                ad.stopLeScan(testigo)
            }
        } catch (e: Exception) { Log.w(TAG, "parar escaneo: ${e.message}") }
    }

    /** Busca nodos en la red local, por difusión UDP.
     *
     *  Sin esto hay que saberse la IP del nodo, que es justo lo que no se puede
     *  adivinar — y menos cuando la coge por DHCP y le cambia. Se usa un
     *  broadcast propio y no el descubrimiento de servicios del sistema porque
     *  ese es una lotería en los Android viejos, que son justo el público de
     *  esto.
     *
     *  Bloquea, así que hay que llamarlo desde un hilo. Devuelve IP → texto que
     *  dice el nodo (indicativo, versión, cuántos hay ya y si el canal está
     *  libre). */
    fun buscaEnRed(segundos: Int = 3): LinkedHashMap<String, String> {
        val vistos = LinkedHashMap<String, String>()
        var s: java.net.DatagramSocket? = null
        try {
            s = java.net.DatagramSocket()
            // Por el WiFi, no por los datos moviles: una difusion mandada por
            // la red movil no llega a ninguna parte. Ver `redWifi`.
            redWifi()?.let { r -> try { r.bindSocket(s) }
                                  catch (e: Exception) { Log.w(TAG, "bind udp: ${e.message}") } }
            s.broadcast = true
            s.soTimeout = 400
            val pregunta = "PTTLORA?".toByteArray(Charsets.US_ASCII)
            /* ⚠️ Y SOBRE TODO, A LA DIFUSIÓN DE LA RED EN LA QUE ESTAMOS.
             *
             * Aquí sólo iban la general (`255.255.255.255`) y la del punto de
             * acceso del propio nodo (`192.168.4.x`). Faltaba la que hace falta
             * en el caso normal —el nodo colgado del router de casa—, y **la
             * general la filtran de largo tanto Android como muchos routers**:
             * encontrar el nodo dependía de que ese broadcast colara. Medido el
             * 10-sep-2026 en una LAN 192.168.1.x: por `192.168.1.255`
             * contestaron los dos nodos, y era el único camino fiable.
             *
             * La dirección se saca del propio interfaz, no se compone a mano:
             * suponer /24 falla en cuanto alguien tiene otra máscara. */
            val destinos = LinkedHashSet<String>()
            try {
                for (ni in java.net.NetworkInterface.getNetworkInterfaces()) {
                    if (!ni.isUp || ni.isLoopback) continue
                    for (ia in ni.interfaceAddresses) {
                        ia.broadcast?.hostAddress?.let { destinos.add(it) }
                    }
                }
            } catch (e: Exception) { Log.w(TAG, "difusiones locales: ${e.message}") }
            destinos.add("255.255.255.255")
            destinos.add("192.168.4.255")      // el nodo en modo punto de acceso
            for (destino in destinos) {
                try {
                    s.send(java.net.DatagramPacket(
                        pregunta, pregunta.size,
                        java.net.InetAddress.getByName(destino), PUERTO_BUSCA))
                } catch (e: Exception) { Log.w(TAG, "difusion $destino: ${e.message}") }
            }
            val hasta = System.currentTimeMillis() + segundos * 1000L
            val buf = ByteArray(128)
            while (System.currentTimeMillis() < hasta) {
                val p = java.net.DatagramPacket(buf, buf.size)
                try { s.receive(p) } catch (e: Exception) { continue }
                val txt = String(p.data, 0, p.length, Charsets.US_ASCII)
                if (txt.startsWith("PTTLORA ")) {
                    vistos[p.address.hostAddress ?: continue] = txt.removePrefix("PTTLORA ")
                }
            }
        } catch (e: Exception) {
            Log.w(TAG, "busqueda en red: ${e.message}")
        } finally {
            try { s?.close() } catch (_: Exception) {}
        }
        return vistos
    }

    /** Por dónde entró el enlace Bluetooth. Se enseña en la pantalla y sirve
     *  para dar soporte: si un móvil solo entra por uno de los caminos, hay que
     *  saber por cuál. */
    @Volatile var comoEntro: String = ""; private set

    /** El enlace BLE vivo, para poder cerrarlo de verdad. */
    private var ble: EnlaceBle? = null

    /**
     * Conecta con el nodo **por BLE**.
     *
     * ## Lo que había aquí antes, y por qué ya no está
     *
     * Una escalera de cuatro caminos para abrir un `BluetoothSocket` de SPP:
     * inseguro por SDP, inseguro por canal 1, y sus dos versiones seguras. Todo
     * aquello existía por una sola razón: el servidor SPP del ESP32 arranca sin
     * exigir nada (`ESP_SPP_SEC_NONE`), así que Android abría el socket, pedía
     * cifrar, y el enlace se caía con un
     * `read failed, socket might closed or timeout, read ret: -1` que no se
     * parecía en nada a su causa. Y los caminos "seguros" hacían daño de
     * verdad: le pedían a Android que emparejara, y al usuario le salía una
     * ventana del SISTEMA pidiendo un PIN **que ningún nodo estaba mostrando**.
     *
     * Con BLE no hay nada de eso. No hay canales, ni SDP, ni socket seguro o
     * inseguro: se abre el GATT, se descubre el servicio y se habla. La puerta
     * sigue siendo el código de 6 cifras de la pantalla del nodo, que es donde
     * el usuario quería que estuviera.
     *
     * Toda la mecánica fea está en [EnlaceBle]; aquí solo se pide el tubo.
     */
    fun conecta(mac: String) {
        arranca("nodo-ble") {
            val e = EnlaceBle(ctx ?: throw IllegalStateException("sin contexto"),
                              { linea -> onDiagnostico?.invoke(linea) })
            ble = e
            val tubo = e.abre(mac)
            comoEntro = "BLE"
            /* El nombre se pide DESPUÉS de conectar y **lanza SecurityException
               en Android 12+ sin BLUETOOTH_CONNECT**. Sin proteger, el enlace
               quedaba abierto en el nodo y la app se iba al catch sin mandar un
               solo byte: el nodo esperaba y soltaba la ranura por "cliente
               mudo", y en el móvil salía un "Sin enlace" que no decía nada. Un
               nombre bonito no vale una conexión. */
            val comoSeLlama = try {
                android.bluetooth.BluetoothAdapter.getDefaultAdapter()
                    ?.getRemoteDevice(mac)?.name ?: mac
            } catch (e2: Exception) { mac }
            tubo to "$comoSeLlama · BLE"
        }
    }

    /** Por WiFi. Un nodo admite hasta ocho a la vez, y cada uno emite con su
     *  propio indicativo: por eso se manda IDENT nada más entrar. */
    fun conectaTcp(host: String, puerto: Int = PUERTO_APP) {
        arranca("nodo-tcp") {
            val red = redWifi()
            val s = java.net.Socket()
            // Atado al WiFi si lo hay: ver `redWifi`.
            if (red != null) try { red.bindSocket(s) }
                             catch (e: Exception) { Log.w(TAG, "bind: ${e.message}") }
            // Plazo corto: si el nodo no está, hay que decirlo, no colgarse.
            s.connect(java.net.InetSocketAddress(host, puerto), 6000)
            // La voz no espera al algoritmo de Nagle.
            s.tcpNoDelay = true
            Triple(s.getInputStream(), s.getOutputStream(), s as java.io.Closeable) to
                "$host:$puerto"
        }
    }

    /** Lo común a los dos tubos: abrir en un hilo aparte, arrancar el escritor
     *  y quedarse leyendo. Lo que cambia es solo cómo se abre. */
    private fun arranca(nombreHilo: String,
                        abre: () -> Pair<Triple<InputStream, OutputStream,
                                                java.io.Closeable>, String>) {
        desconecta()
        vivo = true
        Thread({
            try {
                val (tubo, comoSeLlama) = abre()
                val (ent, sal, cierra) = tubo
                cerrable = cierra
                salida = sal
                conectado = true
                ultimoDato = System.currentTimeMillis()
                /* El escritor, ANTES de avisar: quien reciba el aviso va a
                   encolar ordenes inmediatamente (la identificacion, la
                   configuracion...) y tiene que haber quien las saque. */
                escritor = Thread({ bucleEscritura() }, "nodo-tx").apply { start() }
                onEstado(true, comoSeLlama)
                bucleLectura(ent)
            } catch (e: Exception) {
                Log.w(TAG, "conexion: ${e.message}", e)
                // Con el tipo de excepcion: "SecurityException" o
                // "IOException" dicen cosas muy distintas, y sin eso la
                // pantalla solo puede repetir "sin enlace".
                onEstado(false, "${e.javaClass.simpleName}: ${e.message ?: "-"}")
            } finally {
                cierraTodo()
            }
        }, nombreHilo).also { lector = it }.start()
    }

    fun desconecta() {
        vivo = false
        cierraTodo()
    }

    private fun cierraTodo() {
        if (conectado) { conectado = false; onEstado(false, "desconectado") }
        try { cerrable?.close() } catch (_: Exception) {}
        /* El GATT se cierra APARTE del flujo. `BluetoothGatt.close()` libera un
           recurso del sistema, y Android admite un número limitado de clientes
           GATT a la vez: si no se cierra, tras unas cuantas reconexiones deja
           de conectar y no dice por qué. */
        try { ble?.cierra() } catch (_: Exception) {}
        ble = null
        cerrable = null
        salida = null
        cola.clear()
    }

    // ---------------------------------------------------------------- envío --
    fun manda(tipo: Int, datos: ByteArray = ByteArray(0)) {
        if (!conectado) return
        val out = java.io.ByteArrayOutputStream(datos.size + 8)
        out.write(FEND); out.write(tipo)
        for (b in datos) {
            val v = b.toInt() and 0xFF
            when (v) {
                FEND -> { out.write(FESC); out.write(TFEND) }
                FESC -> { out.write(FESC); out.write(TFESC) }
                else -> out.write(v)
            }
        }
        out.write(FEND)
        // Si la cola se llena es que el enlace no da abasto: se tira lo más
        // viejo, que en audio siempre es lo correcto.
        if (!cola.offer(out.toByteArray())) {
            cola.poll()
            cola.offer(out.toByteArray())
        }
    }

    /** Escrituras CON respuesta mientras dure algo que no puede perder ni un
     *  byte (la actualización de firmware). Para la voz da igual perder un
     *  lote; para un firmware, no. Solo tiene efecto por BLE: por WiFi el TCP
     *  ya es fiable de por sí. */
    fun fiable(si: Boolean) { ble?.conRespuesta = si }

    /** Ajustes del nodo. **El indicativo es opcional a propósito.**
     *
     *  Mandarlo en cada conexión —que es lo que se hacía— significa que
     *  cualquier móvil que se conecte REBAUTIZA el nodo: su identificación,
     *  su baliza y hasta su nombre Bluetooth, que cambia en el siguiente
     *  arranque. Con dos nodos del mismo dueño acababan los dos llamándose
     *  igual, y entonces comparten el hash de origen: el descarte de
     *  duplicados de la malla ya no los distingue y la supresión de repetición
     *  se confunde. Además deja al usuario buscando un nombre que ya no
     *  existe.
     *
     *  Lo que dice quién habla en cada transmisión es `IDENT`, que va por
     *  cliente y se manda siempre. El indicativo DEL NODO solo se toca cuando
     *  el usuario lo pide (guardar los ajustes) o cuando el nodo no tiene
     *  ninguno. El protocolo ya lo permite: en `CMD_CONFIG` el indicativo es
     *  opcional y se distingue por ser ASCII imprimible. */
    fun configura(canal: Int, saltos: Int, potencia: Int, perfil: Int,
                  indicativo: String? = null) {
        val ind = indicativo?.uppercase()?.take(12)?.toByteArray(Charsets.US_ASCII)
                  ?: ByteArray(0)
        manda(CMD_CONFIG, byteArrayOf(canal.toByte(), saltos.toByte(),
                                      potencia.toByte(), perfil.toByte()) + ind)
    }

    /** Autoriza esta conexión con el código que muestra la pantalla del nodo. */
    fun autoriza(codigo: String) {
        manda(CMD_CONFIRMA, codigo.trim().toByteArray(Charsets.US_ASCII))
    }

    /** Ajustes de radio. Los limites NO se comprueban aqui: el nodo conoce su
     *  hardware y rechaza lo que no aguanta, cosa que ademas protege al nodo de
     *  cualquier cliente, no solo del nuestro. */
    fun radio(kHz: Int, bwKHz: Int, sf: Int, cr: Int, dBm: Int) {
        val b = ByteArray(9)
        b[0] = (kHz ushr 24).toByte(); b[1] = (kHz ushr 16).toByte()
        b[2] = (kHz ushr 8).toByte();  b[3] = kHz.toByte()
        b[4] = (bwKHz ushr 8).toByte(); b[5] = bwKHz.toByte()
        b[6] = sf.toByte(); b[7] = cr.toByte(); b[8] = dBm.toByte()
        manda(CMD_RADIO, b)
    }

    /** Latido. El nodo suelta la ranura Bluetooth si el cliente lleva mucho
     *  callado: sin esto, un móvil que se va de alcance dejaría el nodo
     *  ocupado por un fantasma hasta reiniciarlo. */
    fun latido() = manda(CMD_ESTADO)

    /** Manda las credenciales del WiFi al nodo. Tiene efecto al reiniciarlo. */
    fun configuraWifi(ssid: String, clave: String) {
        if (ssid.isBlank()) { manda(CMD_WIFI); return }
        manda(CMD_WIFI, ssid.toByteArray(Charsets.UTF_8) + byteArrayOf(0) +
                        clave.toByteArray(Charsets.UTF_8))
    }

    /** Le dice al nodo con qué indicativo emite ESTE cliente.
     *
     *  Es lo que hace posible que varios se cuelguen del mismo nodo: sin esto,
     *  el nodo pondría el suyo, y en banda de aficionado no se emite bajo
     *  indicativo ajeno. Se manda nada más conectar y al cambiarlo. */
    /** Le pasa al nodo dónde está, en diezmillonésimas de grado y con signo.
     *  Little-endian a mano: el orden de bytes es parte del protocolo y no
     *  puede depender de la máquina que lo compile. */
    fun posicion(latE7: Int, lonE7: Int, fija: Boolean = false) {
        val d = ByteArray(9)
        d[0] = (if (fija) POS_FIJA else POS_VIVA).toByte()
        for (i in 0..3) {
            d[1 + i] = ((latE7 shr (8 * i)) and 0xFF).toByte()
            d[5 + i] = ((lonE7 shr (8 * i)) and 0xFF).toByte()
        }
        manda(CMD_POS, d)
    }

    /** Que la olvide y deje de publicarla. */
    fun olvidaPosicion() = manda(CMD_POS, ByteArray(0))

    /** Reiniciar el nodo. El nodo contesta y se reinicia 300 ms después, así
     *  que se pierde el enlace a propósito: la app reconecta sola. */
    fun reinicia() = manda(CMD_REINICIA, ByteArray(0))

    fun identifica(indicativo: String) {
        manda(CMD_IDENT,
              indicativo.uppercase().take(12).toByteArray(Charsets.US_ASCII))
    }

    /** Modo de red del nodo: apagado, cliente de una red, o punto de acceso
     *  propio. Tiene efecto al reiniciarlo.
     *
     *  El punto de acceso propio es lo que permite dar servicio a un grupo sin
     *  infraestructura ninguna, y por eso el nodo EXIGE clave: uno abierto
     *  dejaría su mando a quien pasara por la calle. */
    fun configuraRed(modo: Int, ssid: String, clave: String) {
        manda(CMD_RED, byteArrayOf(modo.toByte()) +
                       ssid.toByteArray(Charsets.UTF_8) + byteArrayOf(0) +
                       clave.toByteArray(Charsets.UTF_8))
    }

    /** Añade un enlace con otro nodo por Internet. Sin host, los suelta todos. */
    fun enlaza(host: String, puerto: Int = PUERTO_ENLACE) {
        if (host.isBlank()) { manda(CMD_ENLACE); return }
        manda(CMD_ENLACE, byteArrayOf((puerto ushr 8).toByte(), puerto.toByte()) +
                          host.toByteArray(Charsets.UTF_8))
    }

    /** MUERTE SILENCIOSA, NO.
     *
     *  Este hilo se limitaba a `break` cuando la escritura fallaba. Parece
     *  inofensivo y no lo es: el lector se queda bloqueado en `read()` —que en
     *  RFCOMM puede no volver NUNCA—, nadie avisa de que el enlace se ha roto,
     *  la pantalla sigue diciendo "conectado" y el PTT no hace nada porque lo
     *  que se encola ya no lo saca nadie. Y como `enlazado` sigue en true, la
     *  reconexión automática tampoco entra.
     *  Sintoma real: se reinicio el nodo, el movil siguio marcando enlace y
     *  todo lo que se hablaba se perdia sin un solo mensaje de error.
     *  Al fallar la escritura se CIERRA el socket: eso despierta al lector y
     *  dispara el desmontaje normal, que avisa y deja que se reconecte solo. */
    private fun bucleEscritura() {
        val s = salida ?: return
        while (vivo && conectado) {
            val t = try { cola.take() } catch (e: InterruptedException) { break }
            try { s.write(t); s.flush() } catch (e: Exception) {
                Log.w(TAG, "escritura rota: ${e.javaClass.simpleName}: ${e.message}")
                try { cerrable?.close() } catch (_: Exception) {}
                break
            }
        }
    }

    // -------------------------------------------------------------- lectura --
    private fun bucleLectura(ent: InputStream) {
        val buf = ByteArray(1024)
        val trama = java.io.ByteArrayOutputStream(512)
        var dentro = false
        var escape = false
        while (vivo) {
            val n = try { ent.read(buf) } catch (e: Exception) { -1 }
            if (n <= 0) break
            ultimoDato = System.currentTimeMillis()
            for (i in 0 until n) {
                val c = buf[i].toInt() and 0xFF
                if (c == FEND) {
                    if (dentro && trama.size() >= 1) {
                        val d = trama.toByteArray()
                        try { onEvento(d[0].toInt() and 0xFF, d.copyOfRange(1, d.size)) }
                        catch (e: Exception) { Log.w(TAG, "evento: ${e.message}") }
                    }
                    dentro = true; escape = false; trama.reset()
                    continue
                }
                if (!dentro) continue
                if (c == FESC) { escape = true; continue }
                var v = c
                if (escape) { v = if (c == TFEND) FEND else FESC; escape = false }
                if (trama.size() < 512) trama.write(v)
            }
        }
    }
}
