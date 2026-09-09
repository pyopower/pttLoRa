package c31ag.pttlora

import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothProfile
import android.os.Build
import android.util.Log
import java.io.InputStream
import java.io.OutputStream
import java.util.UUID
import java.util.concurrent.CountDownLatch
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit

/**
 * El tubo hacia el nodo por **Bluetooth de baja energía (BLE)**, presentado
 * como un par de flujos de bytes.
 *
 * ## Por qué esta clase existe
 *
 * Hasta la v0.9.15 la app hablaba con el nodo por **SPP** (`BluetoothSocket`),
 * que ya es un flujo de bytes y no necesitaba nada de esto. Se cambió porque
 * SPP no daba más de sí: la pila del ESP32 se reiniciaba sola al quedarse sin
 * memoria escribiendo, admitía **un solo cliente** y no se enteraba de que se
 * había ido, y el emparejamiento de Android abría sesiones fantasma que dejaban
 * a la app fuera. Una noche entera de averías encadenadas. El firmware v1.26
 * pasó a BLE y esto es su otra mitad.
 *
 * ## La idea: que no se note aguas arriba
 *
 * **El protocolo KISS no cambia.** Lo único que cambia es el tubo, así que esta
 * clase se disfraza de lo que había antes: entrega un `InputStream` y un
 * `OutputStream`, y `Nodo` sigue con su hilo lector, su hilo escritor y su cola
 * exactamente igual que con el socket. Todo lo feo de BLE —trocear al MTU,
 * esperar a que cada escritura se confirme, activar las notificaciones— se
 * queda aquí dentro.
 *
 * ## Las trampas de BLE en Android, que son todas
 *
 * Ninguna de estas da un error claro; todas se manifiestan como "conecta y no
 * pasa nada", que es el peor modo de fallo posible:
 *
 *  1. **El CCCD.** No basta `setCharacteristicNotification(c, true)`: eso solo
 *     apunta la intención en el móvil. Hay que escribir además el descriptor
 *     `0x2902` del nodo. Si falta, se conecta, se descubre todo, se puede
 *     escribir... y **no llega nada de vuelta**.
 *  2. **Una operación GATT cada vez.** Android tiene UNA cola por conexión: si
 *     se lanza una escritura antes de que la anterior confirme por
 *     `onCharacteristicWrite`, la segunda **se pierde en silencio**. Por eso
 *     cada escritura espera aquí a su confirmación.
 *  3. **El MTU.** Sin pedirlo se queda en 23, o sea 20 bytes útiles por
 *     paquete. Se puede vivir así (un lote de voz son 74 bytes = 4 paquetes
 *     cada 480 ms), pero una actualización de firmware tardaría una eternidad.
 *     `requestMtu` es de Android 5 en adelante; en 4.4 no hay nada que hacer y
 *     el firmware ya trocea a lo que salga.
 *  4. **El error 133.** El código de error más famoso de Android: es un
 *     "algo ha ido mal" genérico que sale mucho al primer intento y desaparece
 *     al segundo. Se reintenta.
 *  5. **La caché de servicios.** Android se guarda la lista de servicios del
 *     cacharro y no la vuelve a pedir. Si el nodo cambia de firmware, el móvil
 *     puede seguir viendo el GATT viejo. Se limpia con `refresh()`, que es un
 *     método oculto y por eso va por reflexión.
 *  6. **`connectGatt` desde el hilo principal.** En los Android viejos —que
 *     son justo el público de esto— conectar desde un hilo cualquiera es
 *     inestable. Se hace desde el hilo de UI y se espera aquí.
 */
class EnlaceBle(private val ctx: android.content.Context,
                private val apunta: (String) -> Unit) {

    companion object {
        private const val TAG = "pttlora.ble"

        /** Servicio "Nordic UART" (NUS), el mismo que usa el firmware.
         *  Los nombres RX/TX son **desde el punto de vista del MÓVIL**, que es
         *  la convención de Nordic: el móvil ESCRIBE en RX y RECIBE por TX.
         *  Confundirlos es la mitad de los fallos al portar esto. */
        val UUID_SERV: UUID = UUID.fromString("6E400001-B5A3-F393-E0A9-E50E24DCCA9E")
        val UUID_RX: UUID = UUID.fromString("6E400002-B5A3-F393-E0A9-E50E24DCCA9E")
        val UUID_TX: UUID = UUID.fromString("6E400003-B5A3-F393-E0A9-E50E24DCCA9E")
        /** El descriptor de configuración de cliente. Ver la trampa 1. */
        val UUID_CCCD: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

        /** Cuánto se espera a cada paso de la apertura. Generoso: un móvil
         *  ocupado puede tardar segundos en descubrir servicios y no por eso
         *  está roto. */
        private const val PLAZO_MS = 12000L
        /** Confirmación de UNA escritura. Corto a propósito: si una escritura
         *  no confirma en medio segundo, el enlace está mal y es mejor que se
         *  rompa y se reconecte que quedarse colgado con el PTT pulsado. */
        private const val PLAZO_ESCRITURA_MS = 500L
    }

    /** ¿Escribir CON respuesta? Ver la nota en `TuboSalida.write`.
     *  Lo pone [Nodo.fiable] mientras dura una actualización de firmware. */
    @Volatile var conRespuesta = false

    private var gatt: BluetoothGatt? = null
    private var caracRx: BluetoothGattCharacteristic? = null   // móvil -> nodo
    @Volatile private var mtu = 23
    @Volatile private var roto = false

    private val entrada = TuboEntrada()
    private val escrituraHecha = java.util.concurrent.Semaphore(0)

    private var latchConectado: CountDownLatch? = null
    private var latchServicios: CountDownLatch? = null
    private var latchMtu: CountDownLatch? = null
    private var latchCccd: CountDownLatch? = null

    private val cb = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, estado: Int, nuevo: Int) {
            if (nuevo == BluetoothProfile.STATE_CONNECTED && estado == BluetoothGatt.GATT_SUCCESS) {
                apunta("BLE: conectado (estado $estado)")
                latchConectado?.countDown()
            } else {
                /* CUALQUIER OTRA COSA ES UNA CAÍDA, y hay que tratarla igual:
                   con `estado != 0` el móvil ni siquiera llegó a conectar (el
                   famoso 133), y con `nuevo == DISCONNECTED` el nodo se fue.
                   En los dos casos lo que no puede pasar es quedarse esperando
                   en un latch para siempre. */
                apunta("BLE: desconectado (estado $estado, nuevo $nuevo)")
                roto = true
                entrada.cierra()
                escrituraHecha.release()          // despierta a quien esperaba
                latchConectado?.countDown()
                latchServicios?.countDown()
                latchMtu?.countDown()
                latchCccd?.countDown()
            }
        }

        override fun onServicesDiscovered(g: BluetoothGatt, estado: Int) {
            apunta("BLE: servicios descubiertos (estado $estado)")
            latchServicios?.countDown()
        }

        override fun onMtuChanged(g: BluetoothGatt, nuevoMtu: Int, estado: Int) {
            if (estado == BluetoothGatt.GATT_SUCCESS) mtu = nuevoMtu
            apunta("BLE: MTU $mtu (estado $estado)")
            latchMtu?.countDown()
        }

        override fun onDescriptorWrite(g: BluetoothGatt,
                                       d: BluetoothGattDescriptor, estado: Int) {
            apunta("BLE: notificaciones activadas (estado $estado)")
            latchCccd?.countDown()
        }

        override fun onCharacteristicWrite(g: BluetoothGatt,
                                           c: BluetoothGattCharacteristic, estado: Int) {
            // Ver la trampa 2: sin esto no se puede mandar el siguiente trozo.
            if (estado != BluetoothGatt.GATT_SUCCESS)
                Log.w(TAG, "escritura rechazada: $estado")
            escrituraHecha.release()
        }

        override fun onCharacteristicChanged(g: BluetoothGatt,
                                             c: BluetoothGattCharacteristic) {
            if (c.uuid == UUID_TX) c.value?.let { if (it.isNotEmpty()) entrada.mete(it) }
        }
    }

    /**
     * Abre el enlace y devuelve los flujos. **Bloquea** hasta que está listo
     * de verdad —conectado, servicios descubiertos y notificaciones activadas—
     * o lanza excepción. No vale devolver antes: `Nodo` empieza a mandar su
     * identificación en cuanto esto vuelve, y si el CCCD no está escrito esas
     * órdenes salen al vacío.
     */
    fun abre(mac: String): Triple<InputStream, OutputStream, java.io.Closeable> {
        val ad = BluetoothAdapter.getDefaultAdapter()
            ?: throw IllegalStateException("sin Bluetooth")
        if (!ad.isEnabled) throw IllegalStateException("Bluetooth apagado")
        val dev = ad.getRemoteDevice(mac)

        /* Dos intentos por el 133 (trampa 4). Entre uno y otro se cierra del
           todo: reutilizar un `BluetoothGatt` que falló es otra forma conocida
           de no conectar nunca más. */
        var ultimo: Exception? = null
        for (intento in 1..2) {
            try {
                conecta(dev, intento)
                return Triple(entrada, TuboSalida(), java.io.Closeable { cierra() })
            } catch (e: Exception) {
                ultimo = e
                apunta("BLE: intento $intento fallido: ${e.message}")
                cierra()
                roto = false
                if (intento < 2) Thread.sleep(600)
            }
        }
        throw (ultimo ?: java.io.IOException("BLE: no se pudo abrir"))
    }

    private fun conecta(dev: android.bluetooth.BluetoothDevice, intento: Int) {
        latchConectado = CountDownLatch(1)

        /* `connectGatt` desde el hilo de UI (trampa 6), y `autoConnect=false`
           porque con true Android conecta "cuando pueda", que pueden ser
           minutos: para un PTT eso es no conectar. */
        val ui = android.os.Handler(android.os.Looper.getMainLooper())
        val listo = CountDownLatch(1)
        ui.post {
            gatt = try {
                if (Build.VERSION.SDK_INT >= 23)
                    dev.connectGatt(ctx, false, cb, BluetoothDevice_TRANSPORT_LE)
                else
                    dev.connectGatt(ctx, false, cb)
            } catch (e: SecurityException) {   // falta BLUETOOTH_CONNECT
                apunta("BLE: sin permiso para conectar: ${e.message}")
                null
            }
            listo.countDown()
        }
        listo.await(3, TimeUnit.SECONDS)
        val g = gatt ?: throw java.io.IOException("BLE: connectGatt no devolvió nada")

        if (!latchConectado!!.await(PLAZO_MS, TimeUnit.MILLISECONDS) || roto)
            throw java.io.IOException("BLE: no conectó")

        /* Prioridad alta: intervalos de conexión cortos. Sin esto el móvil
           elige un intervalo largo para ahorrar batería y la voz llega a
           tirones. Es una petición, no una orden: manda el móvil. */
        if (Build.VERSION.SDK_INT >= 21)
            try { g.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_HIGH) }
            catch (e: Exception) { Log.w(TAG, "prioridad: ${e.message}") }

        /* Caché de servicios (trampa 5). Solo en el SEGUNDO intento: limpiarla
           siempre alarga la conexión de todas las veces por un problema que es
           raro, y es justo cuando el primer intento ha ido mal cuando merece
           la pena sospechar de ella. */
        if (intento > 1) refresca(g)

        latchServicios = CountDownLatch(1)
        if (!g.discoverServices()) throw java.io.IOException("BLE: no se pudo descubrir")
        if (!latchServicios!!.await(PLAZO_MS, TimeUnit.MILLISECONDS) || roto)
            throw java.io.IOException("BLE: no descubrió los servicios")

        val serv = g.getService(UUID_SERV)
            ?: throw java.io.IOException(
                "BLE: este cacharro no lleva el servicio de PTT LoRa " +
                "(¿es un nodo, y con firmware 1.26 o más nuevo?)")
        caracRx = serv.getCharacteristic(UUID_RX)
            ?: throw java.io.IOException("BLE: falta la característica de escritura")
        val caracTx = serv.getCharacteristic(UUID_TX)
            ?: throw java.io.IOException("BLE: falta la característica de lectura")

        /* MTU antes que el CCCD: pedirlo con notificaciones ya activas hace que
           algunos móviles renegocien a media faena y se pierdan paquetes. */
        if (Build.VERSION.SDK_INT >= 21) {
            latchMtu = CountDownLatch(1)
            if (g.requestMtu(517)) latchMtu!!.await(4, TimeUnit.SECONDS)
        } else {
            apunta("BLE: Android 4.4, sin negociación de MTU: se va a 20 bytes por paquete")
        }

        // Las DOS mitades, que es la trampa 1.
        if (!g.setCharacteristicNotification(caracTx, true))
            throw java.io.IOException("BLE: no se pudieron activar las notificaciones")
        val cccd = caracTx.getDescriptor(UUID_CCCD)
            ?: throw java.io.IOException("BLE: la característica no tiene CCCD")
        cccd.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
        latchCccd = CountDownLatch(1)
        if (!g.writeDescriptor(cccd))
            throw java.io.IOException("BLE: no se pudo escribir el CCCD")
        if (!latchCccd!!.await(PLAZO_MS, TimeUnit.MILLISECONDS) || roto)
            throw java.io.IOException("BLE: el nodo no aceptó las notificaciones")

        apunta("BLE: enlace listo, MTU $mtu (${mtu - 3} bytes por paquete)")
    }

    /** `BluetoothGatt.refresh()` es un método oculto: no está en el SDK y solo
     *  se llega por reflexión. Vacía la caché de servicios del móvil. */
    private fun refresca(g: BluetoothGatt) {
        try {
            val m = g.javaClass.getMethod("refresh")
            val ok = m.invoke(g) as? Boolean
            apunta("BLE: caché de servicios limpiada ($ok)")
        } catch (e: Exception) {
            Log.w(TAG, "refresh: ${e.message}")
        }
    }

    fun cierra() {
        roto = true
        entrada.cierra()
        escrituraHecha.release()
        val g = gatt
        gatt = null
        caracRx = null
        try { g?.disconnect() } catch (_: Exception) {}
        try { g?.close() } catch (_: Exception) {}
    }

    /** Constante `BluetoothDevice.TRANSPORT_LE`, escrita a mano porque el
     *  campo es de API 23 y esto compila contra minSdk 19. Forzar LE evita que
     *  Android intente el transporte clásico con un cacharro que ya está
     *  emparejado por SPP de la época anterior — que es exactamente el caso
     *  de los móviles del usuario. */
    private val BluetoothDevice_TRANSPORT_LE = 2

    // ------------------------------------------------------------- flujos --

    /** Lo que llega por notificaciones, servido como flujo de bytes.
     *
     *  KISS es un flujo, no paquetes: da igual en cuántos trozos venga cada
     *  trama, el desentramado de `Nodo` los pega solo. */
    private class TuboEntrada : InputStream() {
        private val cola = LinkedBlockingQueue<ByteArray>(512)
        private var actual: ByteArray? = null
        private var pos = 0
        @Volatile private var cerrado = false

        fun mete(d: ByteArray) {
            // Si se llena es que la app no da abasto leyendo: se tira lo más
            // viejo, que en audio siempre es lo correcto.
            if (!cola.offer(d)) { cola.poll(); cola.offer(d) }
        }

        fun cierra() {
            cerrado = true
            cola.offer(ByteArray(0))    // despierta al lector bloqueado
        }

        /** Deja `actual` con algo que leer, o devuelve false si se acabó. */
        private fun hayQueLeer(): Boolean {
            while (actual == null || pos >= actual!!.size) {
                if (cerrado && cola.isEmpty()) return false
                val t = try { cola.take() } catch (e: InterruptedException) { return false }
                if (t.isEmpty()) { if (cerrado) return false else continue }
                actual = t; pos = 0
            }
            return true
        }

        override fun read(): Int {
            if (!hayQueLeer()) return -1
            return actual!![pos++].toInt() and 0xFF
        }

        override fun read(b: ByteArray, off: Int, len: Int): Int {
            if (len == 0) return 0
            if (!hayQueLeer()) return -1
            val a = actual!!
            val n = minOf(len, a.size - pos)
            System.arraycopy(a, pos, b, off, n)
            pos += n
            return n
        }

        override fun available(): Int =
            (actual?.let { it.size - pos } ?: 0) + cola.sumOf { it.size }

        override fun close() = cierra()
    }

    /** Lo que sale hacia el nodo: troceado al MTU y **de uno en uno**. */
    private inner class TuboSalida : OutputStream() {
        override fun write(b: Int) = write(byteArrayOf(b.toByte()), 0, 1)

        override fun write(b: ByteArray, off: Int, len: Int) {
            val g = gatt ?: throw java.io.IOException("BLE: sin enlace")
            val c = caracRx ?: throw java.io.IOException("BLE: sin característica")
            /* MTU-3: los tres bytes son la cabecera ATT. Con el MTU mínimo (23)
               quedan 20 útiles, que es lo que toca en Android 4.4. */
            val trozo = maxOf(20, mtu - 3)
            var i = 0
            while (i < len) {
                if (roto) throw java.io.IOException("BLE: enlace roto")
                val n = minOf(trozo, len - i)
                c.value = b.copyOfRange(off + i, off + i + n)
                /* SIN RESPUESTA: la mitad de viajes por radio, y en un flujo de
                   audio se nota. El orden lo garantiza el propio enlace BLE.
                   Aun así Android avisa por `onCharacteristicWrite` de que ya
                   puede aceptar la siguiente, y **hay que esperar a eso**
                   (trampa 2) o el trozo siguiente se pierde sin más.
                   ⚠️ Y POR ESO LA ACTUALIZACIÓN DE FIRMWARE VA CON RESPUESTA
                   (`conRespuesta`): escribir sin respuesta no tiene control de
                   flujo, y lo que no cabe **se tira sin que nadie se entere**.
                   En audio eso es perder 480 ms; en una actualización,
                   corromperla. Mordió en el banco del 8-sep-2026 con la
                   herramienta de la Raspberry. */
                c.writeType = if (conRespuesta)
                    BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
                else
                    BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
                escrituraHecha.drainPermits()
                val lanzada = try { g.writeCharacteristic(c) }
                              catch (e: SecurityException) { false }
                if (!lanzada) throw java.io.IOException("BLE: escritura rechazada")
                if (!escrituraHecha.tryAcquire(PLAZO_ESCRITURA_MS, TimeUnit.MILLISECONDS))
                    throw java.io.IOException("BLE: la escritura no confirmó")
                if (roto) throw java.io.IOException("BLE: enlace roto")
                i += n
            }
        }

        override fun flush() {}   // no hay nada que vaciar: cada write ya salió
        override fun close() = cierra()
    }
}
