import java.util.Properties

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

/* Firma propia y ESTABLE.
 *
 * Un APK de depuracion se firma con una clave que no es la misma en cada
 * maquina, asi que cada actualizacion se instalaba como si fuera otra app
 * distinta: Android obliga a desinstalar la anterior y se pierden los ajustes.
 * Con una clave propia, todas las versiones son la misma app.
 *
 * El almacen NO va en el repositorio: vive en ~/pttlora-firma/ (y hay copia en
 * la raspi). Si se pierde, no se puede volver a actualizar sobre lo instalado —
 * ni siquiera con la misma clave regenerada, porque el certificado seria otro.
 */
val ficheroFirma = File(System.getProperty("user.home"), "pttlora-firma/claves.properties")
val clavesFirma = Properties().apply {
    if (ficheroFirma.exists()) ficheroFirma.inputStream().use { load(it) }
}

android {
    namespace = "c31ag.pttlora"
    compileSdk = 34
    // NDK 25: el ultimo que compila para API 19 (Android 4.4).
    ndkVersion = "25.2.9519653"

    defaultConfig {
        applicationId = "c31ag.pttlora"
        minSdk = 19
        targetSdk = 34
        versionCode = 62
        versionName = "0.9.48"
        /* Con `-PsoloV7a` sale una APK **solo de 32 bits**: la mitad de tamaño
           y sin la biblioteca de 64, que en un móvil viejo no pinta nada. Es
           el mismo truco del /apk7 de la app PTT. Sin la propiedad, las dos
           arquitecturas en un solo fichero, que es lo normal. */
        val soloV7a = project.hasProperty("soloV7a")
        ndk {
            abiFilters += if (soloV7a) listOf("armeabi-v7a")
                          else listOf("armeabi-v7a", "arm64-v8a")
        }
        externalNativeBuild {
            cmake {
                arguments += listOf("-DANDROID_STL=none")
                // SOLO nuestra libreria. codec2 construye ademas un monton de
                // ejecutables de ejemplo (freedv_rx, ofdm_*...) que no usamos y
                // que ni siquiera enlazan en Android: usan cabsf/cargf, que no
                // estan en su libm. Limitando el objetivo, ninja compila la
                // libreria y nada mas.
                targets += "pttlora"
            }
        }
    }

    externalNativeBuild {
        cmake { path = file("src/main/cpp/CMakeLists.txt"); version = "3.22.1" }
    }

    signingConfigs {
        create("propia") {
            val jks = File(System.getProperty("user.home"), "pttlora-firma/pttlora.jks")
            if (jks.exists() && clavesFirma.getProperty("almacen") != null) {
                storeFile = jks
                storePassword = clavesFirma.getProperty("almacen")
                keyAlias = clavesFirma.getProperty("alias") ?: "pttlora"
                keyPassword = clavesFirma.getProperty("clave")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"))
            signingConfig = signingConfigs.getByName("propia")
        }
        /* La de depuracion tambien va con la clave propia: asi se puede
           actualizar encima de una release y al reves, que es justo lo que
           hace falta mientras el proyecto se prueba a diario. */
        debug {
            signingConfig = signingConfigs.getByName("propia")
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_1_8
        targetCompatibility = JavaVersion.VERSION_1_8
    }
    kotlinOptions { jvmTarget = "1.8" }
    // Sin appcompat a proposito: en los POC viejos era justo lo que impedia
    // instalar la app PTT (ver la historia de la v0.9.7).
    buildFeatures { buildConfig = true }
}

dependencies { }
