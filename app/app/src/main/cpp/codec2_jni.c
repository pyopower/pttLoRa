// codec2_jni.c — puente JNI mínimo a Codec2 para PTT LoRa.
//
// Todo el audio va a 8 kHz mono. Codec2 trabaja en tramas de 20 ms (modos 3200
// y 2400) o 40 ms (el resto); el numero de muestras y de bytes por trama los
// dice la propia libreria, asi que aqui no se cablea nada: se pregunta.
//
// El lote que viaja en un paquete de LoRa son 480 ms de audio, pero eso lo
// arma el servicio en Kotlin: aqui solo se codifica y descodifica trama a
// trama.

#include <jni.h>
#include <stdlib.h>
#include <string.h>
#include <codec2.h>

#define CLASE Java_c31ag_pttlora_Codec2

JNIEXPORT jlong JNICALL
Java_c31ag_pttlora_Codec2_crear(JNIEnv *env, jclass c, jint modo)
{
    struct CODEC2 *p = codec2_create(modo);
    if (!p) return 0;
    // Sin el "natural" el descodificador mete un tono de excitacion artificial
    // que en voz cerrada suena peor. Es lo que usan las aplicaciones de radio.
    codec2_set_natural_or_gray(p, 1);
    return (jlong)(intptr_t) p;
}

JNIEXPORT void JNICALL
Java_c31ag_pttlora_Codec2_destruir(JNIEnv *env, jclass c, jlong h)
{
    if (h) codec2_destroy((struct CODEC2 *)(intptr_t) h);
}

JNIEXPORT jint JNICALL
Java_c31ag_pttlora_Codec2_muestrasPorTrama(JNIEnv *env, jclass c, jlong h)
{
    return h ? codec2_samples_per_frame((struct CODEC2 *)(intptr_t) h) : 0;
}

JNIEXPORT jint JNICALL
Java_c31ag_pttlora_Codec2_bytesPorTrama(JNIEnv *env, jclass c, jlong h)
{
    return h ? codec2_bytes_per_frame((struct CODEC2 *)(intptr_t) h) : 0;
}

/* PCM (short[]) -> bits. Devuelve los bytes escritos, o 0 si algo no cuadra. */
JNIEXPORT jint JNICALL
Java_c31ag_pttlora_Codec2_codificar(JNIEnv *env, jclass c, jlong h,
                                       jshortArray pcm, jint off,
                                       jbyteArray out, jint outOff)
{
    if (!h) return 0;
    struct CODEC2 *p = (struct CODEC2 *)(intptr_t) h;
    int n = codec2_samples_per_frame(p);
    int nb = codec2_bytes_per_frame(p);
    if ((*env)->GetArrayLength(env, pcm) < off + n) return 0;
    if ((*env)->GetArrayLength(env, out) < outOff + nb) return 0;

    jshort *sp = (*env)->GetShortArrayElements(env, pcm, NULL);
    jbyte *bp = (*env)->GetByteArrayElements(env, out, NULL);
    codec2_encode(p, (unsigned char *)(bp + outOff), (short *)(sp + off));
    (*env)->ReleaseShortArrayElements(env, pcm, sp, JNI_ABORT);
    (*env)->ReleaseByteArrayElements(env, out, bp, 0);
    return nb;
}

/* bits -> PCM. Devuelve las muestras escritas. */
JNIEXPORT jint JNICALL
Java_c31ag_pttlora_Codec2_descodificar(JNIEnv *env, jclass c, jlong h,
                                          jbyteArray in, jint inOff,
                                          jshortArray pcm, jint off)
{
    if (!h) return 0;
    struct CODEC2 *p = (struct CODEC2 *)(intptr_t) h;
    int n = codec2_samples_per_frame(p);
    int nb = codec2_bytes_per_frame(p);
    if ((*env)->GetArrayLength(env, in) < inOff + nb) return 0;
    if ((*env)->GetArrayLength(env, pcm) < off + n) return 0;

    jbyte *bp = (*env)->GetByteArrayElements(env, in, NULL);
    jshort *sp = (*env)->GetShortArrayElements(env, pcm, NULL);
    codec2_decode(p, (short *)(sp + off), (unsigned char *)(bp + inOff));
    (*env)->ReleaseByteArrayElements(env, in, bp, JNI_ABORT);
    (*env)->ReleaseShortArrayElements(env, pcm, sp, 0);
    return n;
}
