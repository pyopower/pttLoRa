package c31ag.pttlora

import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioRecord
import android.content.Context
import android.media.AudioTrack
import android.media.MediaRecorder
import android.util.Log
import java.util.concurrent.ArrayBlockingQueue

/**
 * Audio simplex a 8 kHz mono, en tramas de 60 ms.
 *
 * Al ser simplex no hay captura y reproduccion a la vez, asi que no hace falta
 * cancelacion de eco (que es justo lo que peor funciona en los POC baratos): se
 * abre el microfono solo mientras se transmite.
 */
class AudioEngine(private val ctx: android.content.Context,
                  /** Muestras por trama. Lo fija Codec2 (320 = 40 ms en los
                   *  modos de 1200 y 700C; 160 = 20 ms en 3200 y 2400), no la
                   *  app: por eso es un parametro y no una constante. */
                  val FRAME: Int,
                  private val onFrame: (ShortArray) -> Unit) {

    companion object {
        const val SR = 8000
        // Colchon de reproduccion. En LoRa la latencia ya es de ~1 s por el
        // agrupamiento en lotes de 480 ms, asi que aqui no hace falta apilar
        // mucho: con dos tramas sobra para absorber el jitter del Bluetooth.
        private const val JITTER_START = 2
        /** A partir de aqui el limitador empieza a doblar la curva. */
        private const val UMBRAL = 20000f
        /** Nivel de voz al que apunta el nivelador del microfono (RMS). */
        private const val OBJETIVO = 6500.0
        /** Pasos de ganancia que ofrece Ajustes. */
        val GANANCIAS = floatArrayOf(1.0f, 2.0f, 3.0f, 4.0f, 5.0f)
    }

    private var record: AudioRecord? = null
    private var recThread: Thread? = null
    @Volatile private var capturing = false

    /** Factor de amplificacion del audio recibido (1 = tal cual). */
    @Volatile var gain = 1.0f

    private var pistaFrames = 6

    /**
     * Encaminar el audio por un manos libres Bluetooth (SCO).
     *
     * Un micro PTT de Bluetooth es, para Android, un manos libres: su microfono
     * SOLO se usa si se abre el canal SCO a mano. Sin esto el equipo seguiria
     * grabando por el microfono del movil aunque el micro externo este
     * conectado, que es el fallo tipico de "se oye lejos".
     */
    @Volatile var bt = false
        set(v) {
            if (field == v) return
            field = v
            rutaBt(v)
            // La pista de salida cambia de tipo (musica <-> llamada), y eso
            // obliga a rehacerla.
            try { track?.release() } catch (_: Exception) {}
            playing = false
            playThread?.interrupt()
            playThread = null
            track = null
        }

    private fun rutaBt(on: Boolean) {
        try {
            val am = ctx.getSystemService(Context.AUDIO_SERVICE) as AudioManager
            if (on) {
                am.mode = AudioManager.MODE_IN_COMMUNICATION
                @Suppress("DEPRECATION") am.startBluetoothSco()
                @Suppress("DEPRECATION") am.isBluetoothScoOn = true
            } else {
                @Suppress("DEPRECATION") am.isBluetoothScoOn = false
                @Suppress("DEPRECATION") am.stopBluetoothSco()
                am.mode = AudioManager.MODE_NORMAL
            }
        } catch (e: Exception) {
            Log.w("ptt", "no se pudo enrutar el audio por Bluetooth", e)
        }
    }

    /** Nivelador automatico del microfono (ver [nivelar]). */
    @Volatile var agc = true
    /**
     * Ganancia fija del microfono, ademas del nivelador.
     *
     * El nivelador solo puede subir lo que le entra: si el equipo entrega la voz
     * muy baja -- pasa en los POC chinos, que ademas meten su propio reductor de
     * ruido -- se queda corto. Esto es un empujon fijo por delante, con el mismo
     * limitador de rodilla suave que el audio recibido.
     */
    @Volatile var micGain = 1.0f
    /**
     * Fuente de audio: `VOICE_COMMUNICATION` (con cancelacion de eco y ruido del
     * propio equipo) o el microfono **crudo**. En un ROM chino barato el
     * tratamiento de VOICE_COMMUNICATION puede comerse media voz, y el crudo
     * suena bastante mas fuerte; por eso se puede elegir.
     */
    @Volatile var micCrudo = false
    private var agcGain = 1.0f
    private var agcFrames = 0

    private var track: AudioTrack? = null
    private var playThread: Thread? = null
    @Volatile private var playing = false
    private val queue = ArrayBlockingQueue<ShortArray>(64)
    @Volatile private var primed = false

    // ---------------------------------------------------------- captura ----
    fun startCapture(): Boolean {
        if (capturing) return true
        agcGain = 1.0f                      // cada pulsacion empieza limpia
        agcFrames = 0
        val min = AudioRecord.getMinBufferSize(SR, AudioFormat.CHANNEL_IN_MONO,
            AudioFormat.ENCODING_PCM_16BIT)
        if (min <= 0) return false
        val size = maxOf(min, FRAME * 2 * 4)
        val src = if (micCrudo) MediaRecorder.AudioSource.MIC
                  else MediaRecorder.AudioSource.VOICE_COMMUNICATION
        var r = try { AudioRecord(src, SR, AudioFormat.CHANNEL_IN_MONO,
            AudioFormat.ENCODING_PCM_16BIT, size) } catch (e: Exception) { null }
        if (r == null || r.state != AudioRecord.STATE_INITIALIZED) {
            // Muchos POC no traen VOICE_COMMUNICATION: se cae al micro normal.
            try { r?.release() } catch (_: Exception) {}
            r = try { AudioRecord(MediaRecorder.AudioSource.MIC, SR,
                AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT, size)
            } catch (e: Exception) { null }
        }
        if (r == null || r.state != AudioRecord.STATE_INITIALIZED) {
            Log.e("ptt", "no se pudo abrir el microfono")
            return false
        }
        record = r
        capturing = true
        r.startRecording()
        recThread = Thread({
            val buf = ShortArray(FRAME)
            while (capturing) {
                var got = 0
                while (got < FRAME && capturing) {
                    val n = r.read(buf, got, FRAME - got)
                    if (n <= 0) break
                    got += n
                }
                if (got == FRAME) {
                    nivelar(buf, FRAME)
                    if (micGain > 1.0f) aplicar(buf, FRAME, micGain)
                    onFrame(buf)
                }
            }
        }, "ptt-rec").apply { priority = Thread.MAX_PRIORITY; start() }
        return true
    }

    fun stopCapture() {
        capturing = false
        recThread?.join(300)
        recThread = null
        try { record?.stop() } catch (_: Exception) {}
        try { record?.release() } catch (_: Exception) {}
        record = null
    }

    // ----------------------------------------------------- reproduccion ----
    private fun ensureTrack() {
        if (track != null) return
        val min = AudioTrack.getMinBufferSize(SR, AudioFormat.CHANNEL_OUT_MONO,
            AudioFormat.ENCODING_PCM_16BIT)
        val size = maxOf(min, FRAME * 2 * 6)
        // Cuantas tramas caben en el buffer del altavoz: un pitido mas corto que
        // eso se queda dentro sin sonar hasta que llega audio nuevo, y entonces
        // sale a destiempo (el "beep de soltar" que se oia al pulsar).
        pistaFrames = maxOf(6, (size / 2 + FRAME - 1) / FRAME)
        @Suppress("DEPRECATION")
        val t = AudioTrack(
            if (bt) AudioManager.STREAM_VOICE_CALL else AudioManager.STREAM_MUSIC,
            SR, AudioFormat.CHANNEL_OUT_MONO,
            AudioFormat.ENCODING_PCM_16BIT, size, AudioTrack.MODE_STREAM)
        if (t.state != AudioTrack.STATE_INITIALIZED) {
            Log.e("ptt", "no se pudo abrir el altavoz")
            return
        }
        track = t
        playing = true
        playThread = Thread({
            t.play()
            while (playing) {
                val pcm = try { queue.take() } catch (e: InterruptedException) { break }
                try {
                    // Por si algun equipo deja la pista parada tras un rato sin
                    // escribir: escribir en una pista detenida no suena.
                    if (t.playState != AudioTrack.PLAYSTATE_PLAYING) t.play()
                    t.write(pcm, 0, pcm.size)
                } catch (_: Exception) { break }
            }
            try { t.stop() } catch (_: Exception) {}
        }, "ptt-play").apply { priority = Thread.MAX_PRIORITY; start() }
    }

    /**
     * Ganancia extra con limitador de rodilla suave.
     *
     * El audio que sale del vocoder DMR viene bajo (unos -13 dBFS) y el altavoz
     * del movil a tope se queda justo en la calle. Multiplicar a secas
     * distorsiona en los picos, asi que por encima de UMBRAL la curva se dobla
     * en vez de recortar: sube el volumen percibido sin que la voz raspe. Sin
     * funciones trigonometricas, que esto corre interpretado en los POC.
     */
    private fun amplificar(pcm: ShortArray, n: Int) {
        val g = gain
        if (g <= 1.0f) return
        aplicar(pcm, n, g)
    }

    /** Multiplica con rodilla suave en los picos, para no recortar. */
    private fun aplicar(pcm: ShortArray, n: Int, g: Float) {
        for (i in 0 until n) {
            val v = pcm[i] * g
            val a = if (v < 0) -v else v
            pcm[i] = if (a <= UMBRAL) {
                v.toInt().toShort()
            } else {
                val exceso = a - UMBRAL
                val comp = UMBRAL + exceso / (1f + exceso / (32700f - UMBRAL))
                (if (v < 0) -comp else comp).toInt().toShort()
            }
        }
    }

    /**
     * Nivelador del microfono antes de transmitir.
     *
     * Una radio DMR comprime su audio y sale siempre igual de fuerte; un movil
     * no, y depende ademas de a que distancia hables. Sin esto, en el mismo TG
     * unos se oyen bien y otros hay que adivinarlos. Se busca un nivel objetivo
     * de voz y se corrige despacio al subir y deprisa al bajar, que es lo que
     * evita el bombeo en las pausas. El ruido de fondo (RMS bajo) no se toca:
     * amplificarlo solo sirve para llenar el canal de siseo.
     */
    private fun nivelar(pcm: ShortArray, n: Int) {
        if (!agc) return
        var suma = 0.0
        for (i in 0 until n) {
            val v = pcm[i].toDouble()
            suma += v * v
        }
        val rms = Math.sqrt(suma / n)
        if (rms > 250) {
            val deseada = (OBJETIVO / rms).toFloat().coerceIn(0.4f, 8f)
            // Subir despacio evita que respire con el ruido de fondo, pero al
            // principio de cada pulsacion deja las primeras palabras bajas: las
            // primeras tramas van deprisa y luego se calma.
            val arranque = agcFrames < 8
            agcFrames++
            val paso = if (deseada < agcGain) 0.35f else if (arranque) 0.30f else 0.06f
            agcGain += (deseada - agcGain) * paso
        }
        if (agcGain > 1.02f || agcGain < 0.98f) aplicar(pcm, n, agcGain)
    }

    /** Encola una trama recibida; si el buffer se llena, tira la mas vieja. */
    fun play(pcm: ShortArray) {
        ensureTrack()
        amplificar(pcm, pcm.size)
        if (!queue.offer(pcm.copyOf())) {
            queue.poll()
            queue.offer(pcm.copyOf())
        }
        primed = queue.size >= JITTER_START
    }

    /** Nueva llamada entrante: se vacia lo que quedara de la anterior. */
    fun resetPlayback() {
        queue.clear()
        primed = false
    }

    // ------------------------------------------------------------ avisos ---
    /**
     * Pitidos sintetizados y metidos por la MISMA cola que la voz.
     *
     * Antes esto era un `ToneGenerator` reutilizado, y tras varios PTT seguidos
     * dejaba de sonar: es un recurso del sistema que se queda tonto cuando el
     * micro y el altavoz se abren y cierran a su alrededor. Generando el tono a
     * mano no hay nada compartido que se pueda atascar, y ademas el aviso sale
     * por donde salga la voz (altavoz, manos libres o bluetooth).
     */
    // El de fin es un toque discreto: solo confirma que se ha cerrado la
    // transmision, no hace falta que suene como una alarma.
    fun beep(ok: Boolean) =
        if (ok) tono(1200.0, 70) else tono(650.0, 45, 3200)

    /** Aviso de que empieza a entrar alguien: agudo y muy corto.
     *  Sin cola de silencio porque la voz viene detras y ya lo empuja. */
    fun beepRx() = tono(1600.0, 60, 7000, cola = 1)

    /**
     * Aviso de "te estan llamando": tres parejas de tonos, fuerte y largo.
     *
     * Tiene que oirse con el equipo en el bolsillo y en la calle, asi que va al
     * maximo de amplitud y alterna dos frecuencias: un timbre de dos notas se
     * distingue del pitido de PTT aunque el altavoz sea malo.
     */
    fun alarma() {
        repeat(3) {
            tono(1500.0, 180, 12000, cola = 1)
            tono(1000.0, 180, 12000, cola = 1)
            repeat(3) { queue.offer(ShortArray(FRAME)) }   // hueco entre parejas
        }
        repeat(pistaFrames) { queue.offer(ShortArray(FRAME)) }
    }

    /**
     * Mete un tono por la cola de reproduccion.
     *
     * Los 120 ms de silencio del principio no son un capricho: tras un rato
     * callado, el altavoz esta parado y las primeras muestras que se le mandan
     * se pierden mientras arranca. Sin ese colchon el pitido salia unas veces si
     * y otras no, que era justo el sintoma raro.
     */
    private fun tono(hz: Double, ms: Int, amplitud: Int = 7000, cola: Int = -1) {
        ensureTrack()
        repeat(2) { queue.offer(ShortArray(FRAME)) }
        val n = SR * ms / 1000
        val rampa = SR * 0.006                       // 6 ms de subida y bajada
        val b = ShortArray(n)
        for (i in 0 until n) {
            val env = minOf(1.0, minOf(i, n - i) / rampa)
            b[i] = (amplitud * env * kotlin.math.sin(2 * Math.PI * hz * i / SR)).toInt().toShort()
        }
        var off = 0
        while (off < n) {                            // en trozos de una trama
            val len = minOf(FRAME, n - off)
            queue.offer(b.copyOfRange(off, off + len))
            off += len
        }
        // Silencio detras hasta llenar el buffer del altavoz: es lo que empuja
        // el tono hacia fuera en vez de dejarlo esperando al siguiente audio.
        repeat(if (cola >= 0) cola else pistaFrames) { queue.offer(ShortArray(FRAME)) }
    }

    /**
     * Aviso insistente: dos tonos graves. Suena cuando te cortan por tiempo,
     * que es cuando hay que enterarse aunque el movil este en el bolsillo y la
     * pantalla apagada (puede que estes accionando el PTT sin querer).
     */
    fun alerta() {
        tono(700.0, 130)
        queue.offer(ShortArray(FRAME))                // 60 ms de silencio
        tono(700.0, 130)
    }

    fun release() {
        stopCapture()
        playing = false
        playThread?.interrupt()
        playThread = null
        try { track?.release() } catch (_: Exception) {}
        track = null
    }
}
