package c31ag.pttlora

import android.content.Context
import android.media.AudioAttributes
import android.os.Build
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import android.util.Log

/**
 * Vibracion que funciona tambien en moviles modernos.
 *
 * Dos motivos por los que no vibraba en un OnePlus 8T:
 *  - desde Android 12 el vibrador se pide por `VibratorManager`; el servicio de
 *    siempre esta obsoleto y en algunos equipos ya no da el vibrador bueno;
 *  - sin atributos de audio, la vibracion sale como "uso desconocido" y el
 *    sistema la descarta cuando la app no esta delante. Marcandola como
 *    notificacion sigue saliendo con la pantalla apagada, que es cuando hace
 *    falta.
 */
object Vibra {

    private var v: Vibrator? = null
    private var probado = false

    private fun vibrador(ctx: Context): Vibrator? {
        if (!probado) {
            probado = true
            v = try {
                if (Build.VERSION.SDK_INT >= 31) {
                    (ctx.getSystemService(Context.VIBRATOR_MANAGER_SERVICE)
                            as VibratorManager).defaultVibrator
                } else {
                    @Suppress("DEPRECATION")
                    ctx.getSystemService(Context.VIBRATOR_SERVICE) as Vibrator
                }
            } catch (e: Exception) {
                Log.w("ptt", "sin vibrador", e); null
            }
        }
        return v?.takeIf { it.hasVibrator() }
    }

    /** true si llego a vibrar (para poder decirlo en Ajustes). */
    /** Vibracion larga a golpes, para el aviso de llamada privada. */
    fun alarma(ctx: Context): Boolean {
        val vib = vibrador(ctx) ?: return false
        val patron = longArrayOf(0, 400, 200, 400, 200, 400)
        return try {
            if (Build.VERSION.SDK_INT >= 26) {
                val attrs = AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_NOTIFICATION)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                    .build()
                vib.vibrate(VibrationEffect.createWaveform(patron, -1), attrs)
            } else {
                @Suppress("DEPRECATION") vib.vibrate(patron, -1)
            }
            true
        } catch (e: Exception) {
            Log.w("ptt", "no vibra", e)
            false
        }
    }

    fun buzz(ctx: Context, ms: Long): Boolean {
        val vib = vibrador(ctx) ?: return false
        return try {
            if (Build.VERSION.SDK_INT >= 26) {
                val attrs = AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_NOTIFICATION)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                    .build()
                vib.vibrate(
                    VibrationEffect.createOneShot(ms, VibrationEffect.DEFAULT_AMPLITUDE), attrs)
            } else {
                @Suppress("DEPRECATION") vib.vibrate(ms)
            }
            true
        } catch (e: Exception) {
            Log.w("ptt", "no vibra", e)
            false
        }
    }
}
