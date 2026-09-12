package c31ag.pttlora

import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.Typeface

/**
 * EL INSTRUMENTO DEL PTT. Un medidor de aguja de bobina móvil dibujado a mano,
 * **siempre puesto**, que mide dos cosas distintas según lo que esté pasando:
 *
 *  - **Escuchando** → S-metro: la señal que entra por la antena, de 1 a 9 y +60.
 *  - **Transmitiendo** → vatímetro: los milivatios a los que está puesta la
 *    placa. No es una medida de la salida real —la placa no la manda— sino la
 *    potencia configurada; por eso al lado va también en dBm, que es como se
 *    configura y como se habla de ella.
 *
 * Está siempre, y no sólo mientras entra voz, porque **la aguja cayendo a cero
 * es parte del gesto**: aparecer y desaparecer da un salto feo, y una esfera
 * con la aguja en reposo es exactamente lo que se ve en una emisora encendida
 * sin nadie hablando.
 *
 * ── LA ESCALA S ─────────────────────────────────────────────────────────────
 * Convención IARU para **VHF/UHF**: S9 = −93 dBm y 6 dB por punto S, así que
 * S1 = −141 dBm. Es la que toca por encima de 30 MHz y no es un capricho: la
 * de HF (S9 = −73) dejaría la aguja pegada a la izquierda todo el rato, porque
 * LoRa trabaja de −40 a −140 dBm y un enlace excelente a 3 km anda por −105.
 *
 * ⚠️ **HONESTIDAD**: el RSSI del SX1278 no es un S-metro calibrado. Es una
 * estimación del propio chip, con varios dB de error, que no sabe nada de la
 * ganancia de tu antena ni de la pérdida del cable. Sirve para comparar una
 * señal con otra en el MISMO equipo, que es justo para lo que se usa aquí. Por
 * eso la cifra en dBm sigue escrita en la esfera: la aguja es la lectura
 * bonita, el número es el dato.
 *
 * ── LA BALÍSTICA ────────────────────────────────────────────────────────────
 * Una aguja de verdad tiene masa: sube deprisa y baja despacio, y nunca da
 * saltos. Sin eso —pintando el valor crudo de cada lote— esto parece un gráfico
 * de barras temblando y se pierde todo el efecto. Ataque rápido, caída lenta, y
 * una marca de pico que se queda un momento arriba, como el índice de arrastre
 * de un medidor de aguja.
 */
class SMetro {

    enum class Escala { SENAL, POTENCIA }

    var escala = Escala.SENAL
        private set

    /** dBm de la última medida real, o null si ahora mismo no hay ninguna. */
    private var medida: Int? = null
    /** Potencia configurada, en dBm, mientras se transmite. */
    private var potDbm: Int = 0

    /** Posición de la aguja, 0..1 sobre el arco. Es lo que se dibuja. */
    private var aguja = 0f
    /** A dónde quiere ir la aguja. */
    private var destino = 0f
    /** Índice de arrastre: el máximo reciente. */
    private var pico = 0f
    private var picoRetiene = 0

    /** Llega una medida del aire. `rssi` 126/127 no son medidas (ver
     *  NodoService): son marcas de "esto no vino por la antena". */
    fun mide(rssi: Int) {
        // Transmitiendo manda el vatímetro: una medida de señal que llegue
        // suelta —el aviso de canal libre, por ejemplo— no le mueve la aguja.
        if (escala == Escala.POTENCIA) return
        if (rssi >= 126) { reposo(); return }
        medida = rssi
        destino = posicionSenal(rssi.toFloat())
    }

    /** Se transmite: el instrumento pasa a vatímetro. */
    fun transmite(dBm: Int) {
        escala = Escala.POTENCIA
        potDbm = dBm
        medida = null
        destino = Math.min(mW(dBm) / FONDO_MW, 1f)
    }

    /** Ni se recibe ni se transmite: vuelve a ser S-metro y la aguja cae. */
    fun reposo() {
        escala = Escala.SENAL
        medida = null
        destino = 0f
    }

    /**
     * Un paso de balística, pensado para 30 por segundo. Devuelve si hace falta
     * seguir repintando: cuando la aguja ha llegado a su sitio y el pico ya ha
     * bajado, esto dice `false` y la pantalla se queda quieta sin gastar nada.
     */
    fun paso(): Boolean {
        val antes = aguja
        // Sube deprisa (la señal aparece de golpe), baja despacio (así se ve
        // caer un desvanecimiento en vez de parpadear).
        val k = if (destino > aguja) 0.45f else 0.12f
        aguja += (destino - aguja) * k
        if (Math.abs(destino - aguja) < 0.002f) aguja = destino

        if (aguja >= pico) { pico = aguja; picoRetiene = 24 }   // ~0,8 s arriba
        else if (picoRetiene > 0) picoRetiene--
        else pico = Math.max(aguja, pico - 0.010f)

        return Math.abs(aguja - antes) > 0.0005f || aguja != destino ||
               pico > aguja + 0.004f
    }

    // ------------------------------------------------------------ ESCALAS ---

    /** dBm → puntos S. S9 = −93 dBm, 6 dB por punto. */
    private fun puntosS(dBm: Float) = (dBm + 141f) / 6f

    /**
     * dBm → posición 0..1 en el arco. **Por tramos, como los medidores de
     * verdad**: de S1 a S9 se reparte el 62 % del recorrido y los 60 dB de
     * arriba se comprimen en el 38 % restante. Repartir todo por igual dejaría
     * el tráfico normal amontonado en el primer cuarto del dial.
     */
    private fun posicionSenal(dBm: Float): Float {
        val s = puntosS(dBm)
        val p = if (s <= 9f) (s / 9f) * 0.62f
                else 0.62f + Math.min((dBm + 93f) / 60f, 1f) * 0.38f
        return Math.max(0f, Math.min(1f, p))
    }

    /** La lectura escrita, a la manera de siempre: "S7" o "S9+20". */
    private fun lectura(dBm: Int): String {
        val s = puntosS(dBm.toFloat())
        if (s >= 9f) {
            val sobra = ((dBm + 93) / 10) * 10
            return if (sobra > 0) "S9+$sobra" else "S9"
        }
        val n = Math.max(0, Math.round(s - 0.5f))
        return "S$n"
    }

    private fun mW(dBm: Int) = Math.pow(10.0, dBm / 10.0).toFloat()

    // ------------------------------------------------------------ DIBUJO ---

    private val tinta = Paint(Paint.ANTI_ALIAS_FLAG)
    private val letra = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        typeface = Typeface.MONOSPACE
        textAlign = Paint.Align.CENTER
    }
    private val cristal = RectF()
    private val arco = RectF()

    /**
     * Pinta el instrumento dentro de `caja`. El pivote va **pegado al borde
     * inferior**: así el arco que se ve es el trozo de arriba de una
     * circunferencia grande, que es lo que hace que parezca un instrumento y no
     * un semicírculo de tarta.
     */
    fun dibuja(c: Canvas, caja: RectF) {
        if (caja.width() <= 0f || caja.height() <= 0f) return
        val h = caja.height()

        // La esfera: fondo casi negro con el borde de fósforo apagado.
        cristal.set(caja)
        val r = h * 0.10f
        tinta.style = Paint.Style.FILL
        tinta.color = ESFERA
        c.drawRoundRect(cristal, r, r, tinta)
        tinta.style = Paint.Style.STROKE
        tinta.strokeWidth = h * 0.025f
        tinta.color = MARCO
        c.drawRoundRect(cristal, r, r, tinta)

        val cx = caja.centerX()
        val cy = caja.bottom - h * 0.10f
        val rad = h * 0.78f
        arco.set(cx - rad, cy - rad, cx + rad, cy + rad)

        letra.textSize = h * 0.15f
        tinta.style = Paint.Style.STROKE
        tinta.strokeCap = Paint.Cap.BUTT

        if (escala == Escala.SENAL) {
            tinta.strokeWidth = h * 0.022f
            tinta.color = FOSFORO_FLOJO
            c.drawArc(arco, A0, ABARRE * 0.62f, false, tinta)
            // El tramo de "+dB", más grueso y más brillante: en un medidor
            // clásico aquí iba el rojo, pero esta esfera es de fósforo y el
            // rojo la rompe.
            tinta.strokeWidth = h * 0.040f
            tinta.color = FOSFORO
            c.drawArc(arco, A0 + ABARRE * 0.62f, ABARRE * 0.38f, false, tinta)

            // Marcas y números. Los impares con número, como en las emisoras.
            for (s in 1..9) {
                val p = (s / 9f) * 0.62f
                marca(c, cx, cy, rad, p, if (s % 2 == 1) 0.14f else 0.08f,
                      h * (if (s % 2 == 1) 0.030f else 0.020f), FOSFORO_FLOJO)
                if (s % 2 == 1) rotulo(c, cx, cy, rad, p, "$s", FOSFORO_FLOJO)
            }
            /* En el tramo de "+dB" van las tres marcas pero sólo dos números:
               el del medio se pisaba con los otros dos. La banda gruesa ya dice
               dónde empieza esta zona, que es lo que hace falta saber. */
            for (db in intArrayOf(20, 40, 60))
                marca(c, cx, cy, rad, 0.62f + (db / 60f) * 0.38f, 0.14f,
                      h * 0.030f, FOSFORO)
            rotulo(c, cx, cy, rad, 0.62f + (20f / 60f) * 0.38f, "+20", FOSFORO)
            rotulo(c, cx, cy, rad, 1f, "+60", FOSFORO)
        } else {
            /* VATÍMETRO. Lineal en potencia —como cualquier medidor de salida—
               y con el fondo de escala en 100 mW, que es el techo de estas
               placas (20 dBm). Con los 17 dBm de siempre la aguja se planta en
               media escala, y eso es la verdad: son 50 mW. */
            tinta.strokeWidth = h * 0.022f
            tinta.color = FOSFORO_FLOJO
            c.drawArc(arco, A0, ABARRE * 0.75f, false, tinta)
            tinta.strokeWidth = h * 0.040f
            tinta.color = FOSFORO
            c.drawArc(arco, A0 + ABARRE * 0.75f, ABARRE * 0.25f, false, tinta)
            var i = 0
            while (i <= 8) {
                val p = i / 8f
                val gordo = i % 2 == 0
                marca(c, cx, cy, rad, p, if (gordo) 0.14f else 0.08f,
                      h * (if (gordo) 0.030f else 0.020f),
                      if (p >= 0.75f) FOSFORO else FOSFORO_FLOJO)
                if (gordo) rotulo(c, cx, cy, rad, p, "${(p * FONDO_MW).toInt()}",
                                  if (p >= 0.75f) FOSFORO else FOSFORO_FLOJO)
                i++
            }
            /* La unidad NO va rotulada en el centro de la esfera: a media
               escala —los 17 dBm de siempre— la aguja se planta justo encima y
               la tapa. Los milivatios ya van escritos abajo a la derecha. */
        }

        // La aguja: un halo ancho y flojo debajo y el filamento encima. Es la
        // manera barata de que el fósforo "brille" sin capas de software.
        val a = Math.toRadians((A0 + ABARRE * aguja).toDouble())
        val px = cx + Math.cos(a).toFloat() * rad * 0.97f
        val py = cy + Math.sin(a).toFloat() * rad * 0.97f
        val qx = cx - Math.cos(a).toFloat() * rad * 0.12f   // contrapeso
        val qy = cy - Math.sin(a).toFloat() * rad * 0.12f
        tinta.strokeCap = Paint.Cap.ROUND
        tinta.strokeWidth = h * 0.055f
        tinta.color = AGUJA_HALO
        c.drawLine(qx, qy, px, py, tinta)
        tinta.strokeWidth = h * 0.016f
        tinta.color = AGUJA
        c.drawLine(qx, qy, px, py, tinta)

        // El índice de arrastre, sólo cuando se ha despegado de la aguja.
        if (pico > aguja + 0.004f)
            marca(c, cx, cy, rad, pico, 0.10f, h * 0.022f, AGUJA)

        // El tornillo del eje.
        tinta.style = Paint.Style.FILL
        tinta.color = MARCO
        c.drawCircle(cx, cy, h * 0.045f, tinta)

        // Y el dato de verdad, pequeño y a un lado: la aguja es la lectura
        // bonita, esto es lo que se apunta en el cuaderno.
        letra.textSize = h * 0.14f
        letra.color = FOSFORO
        val base = caja.bottom - h * 0.06f
        if (escala == Escala.POTENCIA) {
            esquinas(c, caja, base, "$potDbm dBm", "%.0f mW".format(mW(potDbm)))
        } else medida?.let { dBm ->
            esquinas(c, caja, base, "$dBm dBm", lectura(dBm))
        }
    }

    private fun esquinas(c: Canvas, caja: RectF, base: Float, izq: String, der: String) {
        val m = caja.height() * 0.14f
        letra.textAlign = Paint.Align.LEFT
        c.drawText(izq, caja.left + m, base, letra)
        letra.textAlign = Paint.Align.RIGHT
        c.drawText(der, caja.right - m, base, letra)
        letra.textAlign = Paint.Align.CENTER
    }

    private fun marca(c: Canvas, cx: Float, cy: Float, rad: Float,
                      p: Float, largo: Float, grueso: Float, color: Int) {
        val a = Math.toRadians((A0 + ABARRE * p).toDouble())
        val co = Math.cos(a).toFloat(); val si = Math.sin(a).toFloat()
        tinta.style = Paint.Style.STROKE
        tinta.strokeCap = Paint.Cap.BUTT
        tinta.strokeWidth = grueso
        tinta.color = color
        c.drawLine(cx + co * rad * (1f - largo), cy + si * rad * (1f - largo),
                   cx + co * rad, cy + si * rad, tinta)
    }

    /** Números horizontales por dentro del arco, como en un instrumento real:
     *  girados con la marca serían más "de reloj" y menos legibles de reojo. */
    private fun rotulo(c: Canvas, cx: Float, cy: Float, rad: Float,
                       p: Float, t: String, color: Int) {
        val a = Math.toRadians((A0 + ABARRE * p).toDouble())
        val rr = rad * 0.76f
        letra.color = color
        c.drawText(t, cx + Math.cos(a).toFloat() * rr,
                   cy + Math.sin(a).toFloat() * rr + letra.textSize * 0.36f, letra)
    }

    companion object {
        /** Ángulos en grados de Canvas: 0° a las 3 y creciendo hacia abajo, o
         *  sea que la aguja arriba vive entre 205° y 335°. Ciento treinta
         *  grados de barrido, que es un arco generoso: con poco barrido las
         *  marcas se amontonan hasta que no se leen —probado dibujándolo— y
         *  estirándolo caben los números separados y sigue pareciendo un
         *  instrumento y no un semicírculo de tarta. */
        private const val A0 = 205f
        private const val ABARRE = 130f

        /** Fondo de escala del vatímetro, en mW: 100 mW = 20 dBm, el techo de
         *  estas placas. Ojo si algún día se mete un amplificador: esto mide lo
         *  que sale de la PLACA, no lo que sale de la antena. */
        private const val FONDO_MW = 100f

        private val ESFERA = 0xFF07120A.toInt()
        private val MARCO = 0xFF1E3A24.toInt()
        private val FOSFORO = 0xFF6FFF8A.toInt()
        private val FOSFORO_FLOJO = 0xFF3E9E55.toInt()
        private val AGUJA = 0xFFB8FFC6.toInt()
        /** El halo de la aguja es opaco y no translúcido: con alpha, sobre el
         *  ámbar del botón se ensucia en vez de brillar. */
        private val AGUJA_HALO = 0xFF2E8A44.toInt()
    }
}
