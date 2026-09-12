package c31ag.pttlora

import android.graphics.Canvas
import android.graphics.ColorFilter
import android.graphics.Paint
import android.graphics.Path
import android.graphics.PixelFormat
import android.graphics.RectF
import android.graphics.drawable.Drawable

/**
 * Fondo del botón de PTT: dice de un vistazo qué pasa y **cuánto tiempo de
 * transmisión queda**.
 *
 * Viene de la app PTT privada, donde salió de un problema real: con la pantalla
 * al sol y a un brazo de distancia el texto no se lee, el color de medio
 * botonazo sí. Aquí se mantienen los colores que YA tenía esta app, que no son
 * los de aquélla y significan otra cosa:
 *
 *  - **verde**: puedes hablar.
 *  - **ámbar**: el canal está ocupado, o el PTT lo tiene otro.
 *  - **rojo**: estás transmitiendo. Y el rojo **se vacía de arriba abajo** como
 *    un depósito: el hueco oscuro de arriba es el tiempo consumido y lo que
 *    queda rojo, el que queda hasta el corte por tiempo (TOT). Es la misma
 *    información que un contador, pero se entiende sin leer y de reojo.
 *
 * El TOT no es un adorno: en un canal simplex y compartido un PTT trabado deja
 * el canal inservible para todos. Ver la voz de esto en `NodoService.TOT_S`.
 *
 * Se dibuja a mano en vez de poner un `ProgressBar` encima porque el "líquido"
 * tiene que respetar las esquinas redondeadas y ocupar TODO el fondo, y porque
 * así el botón sigue siendo un Button normal: el `OnTouchListener` y la tecla
 * física no se enteran de nada.
 */
class PttFondo : Drawable() {

    enum class Modo { LIBRE, OCUPADO, TX }

    private val fondo = Paint(Paint.ANTI_ALIAS_FLAG)
    private val relleno = Paint(Paint.ANTI_ALIAS_FLAG)
    private val borde = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE }
    private val rect = RectF()
    private val camino = Path()
    private val ventana = RectF()

    /** EL S-METRO. Vive aquí dentro y no en una vista aparte a propósito: el
     *  PTT es un `Button` normal con este `Drawable` de fondo, y así sigue
     *  siéndolo — el `OnTouchListener`, la tecla física y el manos libres no se
     *  enteran de que ahora hay un instrumento pintado encima. Una vista
     *  flotando por delante habría que hacerla transparente a los toques, que
     *  es justo la clase de arreglo que se rompe en algún móvil raro. */
    val sMetro = SMetro()

    var modo: Modo = Modo.LIBRE
        set(v) {
            if (field == v) return
            field = v
            invalidateSelf()
        }

    /** Parte llena, de 0 (se acabó el tiempo) a 1 (lleno). Sólo se usa en TX. */
    var frac: Float = 1f
        set(v) {
            val n = if (v < 0f) 0f else if (v > 1f) 1f else v
            // Un cuarto de píxel no se ve y ahorra repintados.
            if (Math.abs(field - n) < 0.002f) return
            field = n
            invalidateSelf()
        }

    override fun draw(canvas: Canvas) {
        val b = bounds
        if (b.width() <= 0 || b.height() <= 0) return
        val r = Math.min(b.width(), b.height()) * 0.08f
        rect.set(b.left + 1f, b.top + 1f, b.right - 1f, b.bottom - 1f)
        camino.reset()
        camino.addRoundRect(rect, r, r, Path.Direction.CW)

        val claro: Int; val oscuro: Int; val linea: Int
        when (modo) {
            Modo.LIBRE -> { claro = VERDE; oscuro = VERDE_OSCURO; linea = VERDE_BORDE }
            Modo.OCUPADO -> { claro = AMBAR; oscuro = AMBAR_OSCURO; linea = AMBAR_BORDE }
            Modo.TX -> { claro = ROJO; oscuro = ROJO_OSCURO; linea = ROJO_BORDE }
        }
        fondo.color = oscuro
        canvas.drawPath(camino, fondo)

        val f = if (modo == Modo.TX) frac else 1f
        if (f > 0f) {
            val save = canvas.save()
            canvas.clipPath(camino)
            relleno.color = claro
            // El nivel BAJA: lo que queda lleno se apoya en el fondo del botón.
            canvas.drawRect(
                rect.left, rect.top + rect.height() * (1f - f),
                rect.right, rect.bottom, relleno
            )
            canvas.restoreToCount(save)
        }

        /* EL INSTRUMENTO, SIEMPRE PUESTO. No sólo mientras entra voz: una
           esfera con la aguja en reposo es lo que se ve en una emisora
           encendida, y que aparezca y desaparezca da un salto feo. Se limita su
           anchura a poco más del doble de su altura: en una tableta, un medidor
           de un palmo de ancho y dos dedos de alto no parece un instrumento,
           parece una regla. */
        run {
            val alto = rect.height() * 0.44f
            val ancho = Math.min(rect.width() * 0.94f, alto * 2.3f)
            val cx = rect.centerX()
            val arriba = rect.top + rect.height() * 0.07f
            ventana.set(cx - ancho / 2f, arriba, cx + ancho / 2f, arriba + alto)
            sMetro.dibuja(canvas, ventana)
        }

        borde.color = linea
        borde.strokeWidth = 2f
        canvas.drawPath(camino, borde)
    }

    /** Un paso de la aguja. Devuelve si hace falta seguir repintando. */
    fun pasoMetro(): Boolean {
        val sigue = sMetro.paso()
        if (sigue) invalidateSelf()
        return sigue
    }

    override fun setAlpha(alpha: Int) {}
    override fun setColorFilter(cf: ColorFilter?) {}
    @Deprecated("Drawable", ReplaceWith("PixelFormat.OPAQUE"))
    override fun getOpacity(): Int = PixelFormat.OPAQUE

    companion object {
        // Los mismos que tenía la app antes, para no cambiarle el significado a
        // nadie: sólo se les añade el tono oscuro del "depósito vacío".
        private val VERDE = 0xFF2E7D32.toInt()
        private val VERDE_OSCURO = 0xFF14301A.toInt()
        private val VERDE_BORDE = 0xFF43A047.toInt()
        private val AMBAR = 0xFFFF8F00.toInt()
        private val AMBAR_OSCURO = 0xFF3A2200.toInt()
        private val AMBAR_BORDE = 0xFFFFB300.toInt()
        private val ROJO = 0xFFC62828.toInt()
        private val ROJO_OSCURO = 0xFF3A100F.toInt()
        private val ROJO_BORDE = 0xFFE53935.toInt()
    }
}
