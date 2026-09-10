package c31ag.pttlora

import android.Manifest
import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Color
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.View
import android.widget.Button
import android.widget.EditText
import android.widget.TextView
import android.widget.Toast

/**
 * Pantalla única: enlace, quién habla, estado del canal y un PTT grande.
 *
 * La Activity es solo un mando: todo lo que importa vive en NodoService, para
 * que siga funcionando con la pantalla apagada.
 */
/** Puntos de reunión por defecto. Se ofrecen hechos porque un usuario nuevo no
 *  tiene forma de adivinarlos, y porque el sentido de un reflector es
 *  precisamente que todos apunten al mismo. El segundo solo entra si el
 *  primero no responde. */
private const val REFLECTOR = "or.adan.ovh"
private const val REFLECTOR_RESPALDO = "urf.adan.ovh"

class MainActivity : Activity(), NodoService.Observador {

    /* ASPECTO CLARO / OSCURO, sin AppCompat.
     *
     * Los colores viven en `values/` y `values-night/`, así que seguir al
     * sistema sale gratis. Para FORZAR uno se sobrescribe el modo de la
     * configuración antes de que se resuelva ningún recurso — y eso sólo se
     * puede hacer aquí, en `attachBaseContext`: en `onCreate` ya es tarde,
     * porque el tema de la ventana ya está elegido. */
    override fun attachBaseContext(base: android.content.Context) {
        val tema = Prefs(base).tema
        if (tema == 0) { super.attachBaseContext(base); return }
        val cfg = android.content.res.Configuration(base.resources.configuration)
        cfg.uiMode = (cfg.uiMode and
            android.content.res.Configuration.UI_MODE_NIGHT_MASK.inv()) or
            (if (tema == 2) android.content.res.Configuration.UI_MODE_NIGHT_YES
             else android.content.res.Configuration.UI_MODE_NIGHT_NO)
        super.attachBaseContext(base.createConfigurationContext(cfg))
    }

    private lateinit var prefs: Prefs
    private lateinit var tEstado: TextView
    private lateinit var tQuien: TextView
    private lateinit var tLog: TextView
    private lateinit var bPtt: Button
    private lateinit var rueda: android.widget.LinearLayout
    private val ui = Handler(Looper.getMainLooper())
    private var dialogoCodigo = false

    override fun onCreate(b: Bundle?) {
        super.onCreate(b)
        prefs = Prefs(this)
        setContentView(R.layout.principal)
        tEstado = findViewById(R.id.estado)
        tQuien = findViewById(R.id.quien)
        tLog = findViewById(R.id.log)
        bPtt = findViewById(R.id.ptt)
        rueda = findViewById(R.id.rueda)
        Nombres.arranca(this)
        Nombres.alLlegar = { ui.post { pintaRueda() } }
        pintaRueda()

        findViewById<Button>(R.id.ajustes).setOnClickListener { ajustes() }
        findViewById<Button>(R.id.salir).setOnClickListener { salir() }

        // Mantener pulsado = transmitir. Sin enganche: en un canal compartido
        // dejar el PTT fijado es lo que más molesta a los demás.
        bPtt.setOnTouchListener { v, ev ->
            when (ev.action) {
                MotionEvent.ACTION_DOWN -> { NodoService.instancia?.pulsacion(true); true }
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                    NodoService.instancia?.pulsacion(false); v.performClick(); true
                }
                else -> false
            }
        }

        permisos()
        if (!prefs.listo) ajustes() else arranca()
    }

    override fun onResume() {
        super.onResume()
        ui.removeCallbacks(vigilante)
        ui.post(vigilante)
        NodoService.instancia?.let {
            it.observador = this
            /* Vale cualquiera de los dos caminos: con solo el de datos
               tambien se puede hablar, y la pantalla no debe decir "sin
               enlace" cuando en realidad se esta saliendo por Internet. */
            onEnlace(it.enlazado || it.datosEnlazado,
                     if (it.enlazado) prefs.nombreNodo
                     else if (it.datosEnlazado) "solo datos"
                     else it.ultimoFallo)
            onCanal(it.canalOcupado)
            /* El nodo puede haber pedido el codigo ANTES de que esta pantalla
               estuviera escuchando: el servicio conecta por su cuenta y el
               aviso llega cuando llega. Si solo se atendiera el evento en
               vivo, ese aviso se perderia y el usuario se quedaria con un
               enlace abierto que no responde a nada, sin saber por que. Por
               eso el servicio guarda que esta pendiente y se consulta aqui. */
            if (it.pideCodigo) onPideCodigo()
        }
    }

    override fun onPause() {
        super.onPause()
        ui.removeCallbacks(vigilante)
        if (NodoService.instancia?.observador === this)
            NodoService.instancia?.observador = null
    }

    /** Tecla física de PTT: en los POC suele ser una tecla suelta del lateral.
     *  Se atiende aquí y en el servicio para que valga también en segundo plano. */
    override fun dispatchKeyEvent(e: KeyEvent): Boolean {
        if (e.keyCode == KeyEvent.KEYCODE_VOLUME_DOWN ||
            e.keyCode == KeyEvent.KEYCODE_HEADSETHOOK) {
            if (e.action == KeyEvent.ACTION_DOWN && e.repeatCount == 0)
                NodoService.instancia?.pulsacion(true)
            else if (e.action == KeyEvent.ACTION_UP)
                NodoService.instancia?.pulsacion(false)
            return true
        }
        return super.dispatchKeyEvent(e)
    }

    private fun arranca() {
        val i = Intent(this, NodoService::class.java)
        if (Build.VERSION.SDK_INT >= 26) startForegroundService(i) else startService(i)
        /* El servicio tarda un poco en existir, asi que se intenta varias veces
           en vez de una sola a los 500 ms: si se falla esa unica ventana, la
           pantalla se queda sin enterarse de nada — y lo primero que se pierde
           es la peticion del codigo, que es justo lo que bloquea al usuario. */
        for (t in longArrayOf(0, 200, 500, 1000, 2000))
            ui.postDelayed({ NodoService.instancia?.observador = this }, t)
    }

    /* Vigilante de respaldo mientras la pantalla esta a la vista.
     *
     * Depender de que el evento llegue justo cuando el observador ya esta
     * puesto es fragil: el servicio conecta por su cuenta y el aviso llega
     * cuando llega. Aqui se mira la bandera una vez por segundo, que no cuesta
     * nada y hace que la peticion del codigo NO se pueda perder por cuestion de
     * milisegundos. */
    private val vigilante = object : Runnable {
        override fun run() {
            NodoService.instancia?.let {
                if (it.observador !== this@MainActivity) it.observador = this@MainActivity
                if (it.pideCodigo && !dialogoCodigo) onPideCodigo()
            }
            ui.postDelayed(this, 1000)
        }
    }

    /* El permiso de ubicación llega DESPUÉS, y la posición hay que arrancarla
       entonces: pedirlo y encender en la misma línea deja el GPS sin arrancar y
       un «Posición: encendida» que no manda nada — el peor de los dos mundos,
       porque el usuario cree que ya está. */
    override fun onRequestPermissionsResult(codigo: Int, permisos: Array<out String>,
                                            resultado: IntArray) {
        super.onRequestPermissionsResult(codigo, permisos, resultado)
        if (codigo != 3) return
        val dado = resultado.isNotEmpty() &&
                   resultado[0] == PackageManager.PERMISSION_GRANTED
        if (dado && prefs.posActiva) {
            NodoService.instancia?.posicion(true)
        } else if (!dado) {
            prefs.posActiva = false
            aviso("Sin permiso de ubicación no se puede mandar la posición. " +
                  "Queda apagada.")
        }
    }

    private fun permisos() {
        val faltan = ArrayList<String>()
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO)
                != PackageManager.PERMISSION_GRANTED)
            faltan.add(Manifest.permission.RECORD_AUDIO)
        if (Build.VERSION.SDK_INT >= 31) {
            // Desde Android 12 el Bluetooth clásico también pide permiso.
            if (checkSelfPermission("android.permission.BLUETOOTH_CONNECT")
                    != PackageManager.PERMISSION_GRANTED)
                faltan.add("android.permission.BLUETOOTH_CONNECT")
        }
        if (Build.VERSION.SDK_INT >= 33) {
            if (checkSelfPermission("android.permission.POST_NOTIFICATIONS")
                    != PackageManager.PERMISSION_GRANTED)
                faltan.add("android.permission.POST_NOTIFICATIONS")
        }
        if (faltan.isNotEmpty() && Build.VERSION.SDK_INT >= 23)
            requestPermissions(faltan.toTypedArray(), 1)
    }

    // ------------------------------------------------------------ ajustes ---
    private fun ajustes() {
        val v = layoutInflater.inflate(R.layout.ajustes, null)
        val eInd = v.findViewById<EditText>(R.id.indicativo)
        val bNodo = v.findViewById<Button>(R.id.nodo)
        val bModo = v.findViewById<Button>(R.id.modo)
        val bMicro = v.findViewById<Button>(R.id.micro)
        val bColchon = v.findViewById<Button>(R.id.colchon)
        val bPerfil = v.findViewById<Button>(R.id.perfil)
        val bWifi = v.findViewById<Button>(R.id.wifi)
        val bRadio = v.findViewById<Button>(R.id.radio)
        eInd.setText(prefs.indicativo)
        fun textoNodo() = when {
            prefs.porWifi -> "Nodo por WiFi: " + prefs.host
            prefs.mac.isBlank() -> "Elegir nodo…"
            else -> "Nodo por Bluetooth: " + prefs.nombreNodo
        }
        bNodo.text = textoNodo()
        bModo.text = "Códec: " + Codec2.NOMBRES[prefs.modo]
        fun textoMicro(): String {
            val g = AudioEngine.MIC_GANANCIAS.getOrElse(prefs.micGanancia) { 1.0f }
            val extra = (if (!prefs.micAgc) " · sin nivelador" else "") +
                        (if (prefs.micCrudo) " · crudo" else "")
            return "Micrófono: ×%.1f%s".format(g, extra)
        }
        bMicro.text = textoMicro()
        fun textoColchon() = "Colchón: " + Prefs.COLCHONES[prefs.retrasoLotes]
        bColchon.text = textoColchon()
        bPerfil.text = "Papel: " + Prefs.PERFILES[prefs.perfil]
        /* Se marca lo que NO está en valores de fábrica. Quien haya tocado algo
           sin querer lo ve desde fuera, sin tener que abrir el diálogo. */
        fun textoRadio(): String {
            val d = Prefs.PorDefecto
            val tocado = prefs.frecuenciaKHz != d.FREC_KHZ || prefs.sf != d.SF ||
                         prefs.anchoKHz != d.ANCHO_KHZ || prefs.canalLogico != d.CANAL
            return "Radio: %.3f MHz · sf%d/%d · canal %d · %d dBm%s".format(
                prefs.frecuenciaKHz / 1000.0, prefs.sf, prefs.anchoKHz,
                prefs.canalLogico, prefs.potencia,
                if (tocado) "  ⚠️ cambiado" else "")
        }
        bRadio.text = textoRadio()
        bRadio.setOnClickListener { ajustesRadio { bRadio.text = textoRadio() } }
        bWifi.text = if (prefs.wifiSsid.isBlank()) "WiFi del nodo: sin poner"
                     else "WiFi del nodo: " + prefs.wifiSsid

        bNodo.setOnClickListener { eligeTransporte { bNodo.text = textoNodo() } }
        val bRed = v.findViewById<Button>(R.id.red)
        val bEnlace = v.findViewById<Button>(R.id.enlace)
        bRed.setOnClickListener { redDelNodo() }
        /* La pulsacion larga de «Radio» abre lo AVANZADO —canal, SF y ancho—,
           que es lo que puede dejarte fuera de la red. Antes abria el registro,
           que se queda igual de accesible desde el boton «Registro» de este
           mismo dialogo y desde el estado de la pantalla principal. */
        bRadio.setOnLongClickListener {
            ajustesRadioAvanzado { bRadio.text = textoRadio() }; true
        }
        bEnlace.setOnClickListener { enlaces() }
        val bDatos = v.findViewById<Button>(R.id.datos)
        fun textoDatos() = "Camino de datos: " +
            if (prefs.datosActivo) prefs.datosHost else "apagado"
        bDatos.text = textoDatos()
        bDatos.setOnClickListener { caminoDeDatos { bDatos.text = textoDatos() } }
        val bPos = v.findViewById<Button>(R.id.posicion)
        fun textoPos() = "Posición y APRS: " + if (prefs.posActiva) "encendida" else "apagada"
        bPos.text = textoPos()
        bPos.setOnClickListener { posicion { bPos.text = textoPos() } }
        val bTema = v.findViewById<Button>(R.id.tema)
        val nombresTema = arrayOf("Como el sistema", "Claro", "Oscuro")
        fun textoTema() = "Aspecto: " + nombresTema[prefs.tema]
        bTema.text = textoTema()
        bTema.setOnClickListener {
            AlertDialog.Builder(this)
                .setTitle("Aspecto")
                .setItems(nombresTema) { _, i ->
                    if (i != prefs.tema) {
                        prefs.tema = i
                        // Se rehace la pantalla: el tema se elige antes de
                        // inflar, así que no basta con repintar.
                        recreate()
                    }
                }.show()
        }
        val bNombres = v.findViewById<Button>(R.id.nombres)
        fun textoNombres() = "Nombres de los operadores: " +
            if (prefs.nombresActivo) "sí" else "no"
        bNombres.text = textoNombres()
        bNombres.setOnClickListener {
            AlertDialog.Builder(this)
                .setTitle("Nombres de los operadores")
                .setMessage("En la lista de quién ha hablado, junto al " +
                    "indicativo puede salir el nombre — «EA3XYZ · Marc». En " +
                    "una rueda con varios se reconoce mucho antes un nombre " +
                    "que un distintivo.\n\n" +
                    "Se consulta una vez en radioid.net y se guarda en el " +
                    "móvil, así que después funciona SIN COBERTURA. Lo que se " +
                    "consulta es un indicativo, que ya va en claro por el aire " +
                    "en cada transmisión.\n\n" +
                    "Si lo apagas, sale el indicativo a secas.")
                .setPositiveButton(if (prefs.nombresActivo) "Apagar" else "Encender") { _, _ ->
                    prefs.nombresActivo = !prefs.nombresActivo
                    bNombres.text = textoNombres()
                }
                .setNeutralButton("Borrar lo guardado") { _, _ ->
                    Nombres.olvida(); aviso("Nombres borrados del móvil.")
                }
                .setNegativeButton("Dejarlo", null)
                .show()
        }
        v.findViewById<Button>(R.id.reiniciar).setOnClickListener {
            val svc = NodoService.instancia
            if (svc == null || !svc.enlazado) { aviso("Primero hay que estar enlazado con el nodo."); return@setOnClickListener }
            AlertDialog.Builder(this)
                .setTitle("Reiniciar el nodo")
                .setMessage("El nodo se apaga y vuelve en unos 20 segundos. La " +
                    "app se reconecta sola.\n\nHace falta, por ejemplo, después " +
                    "de cambiarle la WiFi a un nodo con firmware antiguo.")
                .setPositiveButton("Reiniciar") { _, _ -> svc.reiniciaNodo() }
                .setNegativeButton("Dejarlo", null)
                .show()
        }
        v.findViewById<Button>(R.id.firmware).setOnClickListener { eligeFirmware() }
        bWifi.setOnClickListener { wifiDelNodo { bWifi.text =
            if (prefs.wifiSsid.isBlank()) "WiFi del nodo: sin poner"
            else "WiFi del nodo: " + prefs.wifiSsid } }
        bPerfil.setOnClickListener {
            AlertDialog.Builder(this)
                .setTitle("Papel del nodo")
                .setItems(Prefs.PERFILES) { _, i ->
                    prefs.perfil = i
                    bPerfil.text = "Papel: " + Prefs.PERFILES[i]
                    // Se explica lo que hace: el usuario no tiene por que saber
                    // que es una malla por inundacion para elegir bien.
                    AlertDialog.Builder(this)
                        .setMessage(Prefs.PERFILES_AYUDA[i])
                        .setPositiveButton("Vale", null).show()
                }.show()
        }
        /* EL COLCHON, Y POR QUE SE PUEDE APAGAR.
           Esperar antes de reproducir permite reordenar los lotes y tapar con
           Internet lo que la radio no trajo, pero se paga en retardo y toca el
           borde de cada frase. Que se pueda poner en «directo» no es una
           coquetería: es poder volver a un comportamiento conocido sin esperar
           una versión nueva, y poder comparar las dos cosas en el aire en vez
           de discutirlas. */
        /* EL MICRÓFONO, Y POR QUÉ SE PUEDE BAJAR.
           El micro de un móvil entrega de sobra, y hablando de cerca **satura**
           — la voz sale rota y desde el otro lado eso se oye exactamente igual
           que un códec malo o una radio con pérdidas. Costó una tarde de
           perseguir el códec y el colchón antes de dar con ello, así que aquí
           hay tres mandos y, sobre todo, un aviso: cuando el micro recorta, el
           registro lo dice al soltar el PTT en vez de dejarte adivinando. */
        bMicro.setOnClickListener {
            val col = android.widget.LinearLayout(this)
            col.orientation = android.widget.LinearLayout.VERTICAL
            col.setPadding(40, 20, 40, 0)
            val cAgc = android.widget.CheckBox(this)
            cAgc.text = "Nivelador automático"
            cAgc.isChecked = prefs.micAgc
            val cCrudo = android.widget.CheckBox(this)
            cCrudo.text = "Micrófono crudo (sin el procesado del sistema)"
            cCrudo.isChecked = prefs.micCrudo
            col.addView(cAgc); col.addView(cCrudo)
            AlertDialog.Builder(this)
                .setTitle("Micrófono")
                .setMessage("Si te oyen la voz rota, casi siempre es que el " +
                            "micro satura: sepárate un palmo o baja la " +
                            "ganancia. El registro avisa cuando recorta.\n\n" +
                            "Ganancia actual: " + textoMicro())
                .setView(col)
                .setPositiveButton("Elegir ganancia…") { _, _ ->
                    prefs.micAgc = cAgc.isChecked
                    prefs.micCrudo = cCrudo.isChecked
                    AlertDialog.Builder(this)
                        .setTitle("Ganancia del micrófono")
                        .setItems(AudioEngine.MIC_NOMBRES) { _, i ->
                            prefs.micGanancia = i
                            bMicro.text = textoMicro()
                        }.show()
                }
                .setNegativeButton("Guardar") { _, _ ->
                    prefs.micAgc = cAgc.isChecked
                    prefs.micCrudo = cCrudo.isChecked
                    bMicro.text = textoMicro()
                }.show()
        }

        bColchon.setOnClickListener {
            AlertDialog.Builder(this)
                .setTitle("Colchón de audio")
                .setMessage("Cuánto se espera antes de reproducir. Más colchón " +
                            "tapa mejor los lotes perdidos; menos, responde antes.")
                .setItems(Prefs.COLCHONES) { _, i ->
                    prefs.retrasoLotes = i
                    bColchon.text = textoColchon()
                }.show()
        }

        bModo.setOnClickListener {
            AlertDialog.Builder(this)
                .setTitle("Modo de Codec2")
                .setItems(Codec2.NOMBRES.mapIndexed { i, n ->
                    if (i == Prefs.PorDefecto.MODO) "$n  (por defecto)" else n
                }.toTypedArray()) { _, i ->
                    prefs.modo = i
                    bModo.text = "Códec: " + Codec2.NOMBRES[i]
                }.show()
        }

        AlertDialog.Builder(this)
            /* La version, a la vista. Sin esto no hay manera de saber que se
               esta probando — ni el usuario ni quien le da soporte —, y eso
               convierte cualquier fallo en una discusion sobre si la
               actualizacion entro o no. */
            .setTitle("Ajustes · v" + BuildConfig.VERSION_NAME)
            .setView(v)
            .setNeutralButton("Registro") { _, _ -> registro() }
            .setPositiveButton("Guardar") { _, _ ->
                prefs.indicativo = eInd.text.toString()
                if (!prefs.listo) {
                    Toast.makeText(this, "Hace falta indicativo y nodo",
                                   Toast.LENGTH_LONG).show()
                    ajustes()
                } else {
                    /* Una sola orden de reconectar, y forzada. Antes se
                       desconectaba, se arrancaba el servicio (que se conecta
                       solo al crearse) y ADEMAS se pedia conectar a los 800 ms:
                       dos sockets contra un nodo que admite uno. */
                    arranca()
                    /* Guardar es el usuario diciendo expresamente lo que
                       quiere, así que aquí SÍ se le pone el indicativo al
                       nodo (en una conexión normal no se le toca: ver
                       `Nodo.configura`). */
                    NodoService.instancia?.ponIndicativo = true
                    ui.postDelayed({
                        NodoService.instancia?.ponIndicativo = true
                        NodoService.instancia?.conecta(true)
                    }, 800)
                }
            }
            .setCancelable(prefs.listo)
            .show()
    }

    /** Por dónde se habla con el nodo.
     *
     *  No hay uno mejor: el Bluetooth no necesita red ninguna pero es de un
     *  usuario, y el WiFi admite hasta ocho, cada uno con su indicativo. Se
     *  explica en el propio menú porque el usuario no tiene por qué saberlo. */
    private fun eligeTransporte(luego: () -> Unit) {
        AlertDialog.Builder(this)
            .setTitle("¿Cómo se habla con el nodo?")
            .setItems(arrayOf(
                "Bluetooth — un usuario, sin necesidad de red",
                "WiFi — hasta 8 usuarios en el mismo nodo")) { _, i ->
                prefs.porWifi = (i == 1)
                if (i == 1) nodoPorWifi(luego) else eligeNodo(luego)
            }.show()
    }

    /** Dirección del nodo cuando se habla por WiFi.
     *
     *  Se ofrece hecha la del punto de acceso del propio nodo (192.168.4.1),
     *  que es el caso más común y el único que el usuario no puede adivinar. */
    /** El nodo por WiFi.
     *
     *  ANTES pedía la dirección a pelo y ofrecía 192.168.4.1 por defecto, que
     *  es la del punto de acceso **del propio nodo**: si el nodo está colgado
     *  de la red de casa —el caso normal— esa dirección no vale, y su IP no
     *  hay forma de adivinarla. Encima el usuario se lleva un fallo de
     *  conexión sin ninguna pista de por qué.
     *  Ahora se busca **nada más abrir**, que es lo que ya sabe hacer el nodo
     *  (contesta a una difusión propia), y escribir la dirección a mano queda
     *  como salida, no como puerta de entrada. */
    private fun nodoPorWifi(luego: () -> Unit) {
        val col = android.widget.LinearLayout(this)
        col.orientation = android.widget.LinearLayout.VERTICAL
        col.setPadding(40, 20, 40, 0)
        val t = TextView(this)
        t.text = "Buscando nodos en esta red…"
        val lista = android.widget.LinearLayout(this)
        lista.orientation = android.widget.LinearLayout.VERTICAL
        val e = EditText(this)
        e.setText(prefs.host)
        e.hint = "o escribe la dirección"
        col.addView(t); col.addView(lista); col.addView(e)

        val dlg = AlertDialog.Builder(this)
            .setTitle("Nodo por WiFi")
            .setView(col)
            .setPositiveButton("Vale") { _, _ ->
                val h = e.text.toString().trim()
                if (h.isNotBlank()) prefs.host = h
                luego()
            }
            .setNeutralButton("Buscar otra vez") { _, _ -> nodoPorWifi(luego) }
            .setNegativeButton("Cancelar", null)
            .show()

        Thread({
            val vistos = Nodo({ _, _ -> }, { _, _ -> }, this).buscaEnRed(3)
            ui.post {
                if (vistos.isEmpty()) {
                    t.text = "No se ha visto ningún nodo en esta red.\n\n" +
                        "Comprueba que el móvil está en la MISMA red que el " +
                        "nodo (el nodo solo ve las de 2,4 GHz) y que no está " +
                        "usando datos móviles para salir.\n\n" +
                        "Si te has conectado al WiFi que levanta el propio " +
                        "nodo, su dirección es ${Nodo.IP_AP}."
                    if (e.text.isNullOrBlank()) e.setText(Nodo.IP_AP)
                    return@post
                }
                t.text = "Toca el nodo al que quieres conectarte:"
                for ((ip, dice) in vistos) {
                    val b = Button(this)
                    b.text = "$dice\n$ip"
                    b.setOnClickListener {
                        prefs.host = ip
                        prefs.porWifi = true
                        dlg.dismiss()
                        luego()
                    }
                    lista.addView(b)
                }
            }
        }, "busca-red").start()
    }

    /** Busca nodos en la red por difusión: no hace falta saberse ninguna IP. */
    private fun buscaEnRed(luego: () -> Unit) {
        val esperando = AlertDialog.Builder(this)
            .setTitle("Buscando en la red…")
            .setMessage("Preguntando a todos los nodos que haya en esta red.")
            .setCancelable(false)
            .show()
        Thread({
            val vistos = Nodo({ _, _ -> }, { _, _ -> }, this).buscaEnRed(3)
            ui.post {
                esperando.dismiss()
                if (vistos.isEmpty()) {
                    aviso("No se ha encontrado ningún nodo.\n\nComprueba que " +
                          "el móvil está en la misma red que el nodo (o " +
                          "conectado a su propio WiFi), y que el nodo tiene la " +
                          "red encendida.")
                    return@post
                }
                val ips = vistos.keys.toList()
                val textos = ips.map { "${vistos[it]}\n$it" }.toTypedArray()
                AlertDialog.Builder(this)
                    .setTitle("Nodos encontrados")
                    .setItems(textos) { _, i ->
                        prefs.host = ips[i]
                        prefs.porWifi = true
                        luego()
                    }
                    .setNegativeButton("Cancelar", null)
                    .show()
            }
        }, "busca-red").start()
    }

    /** Modo de red del NODO: apagado, colgado de una red, o punto de acceso
     *  propio. Lo último es lo que permite dar servicio a un grupo sin
     *  infraestructura de ninguna clase. */
    private fun redDelNodo() {
        AlertDialog.Builder(this)
            .setTitle("Red del nodo")
            .setItems(arrayOf(
                "Apagada (menos consumo)",
                "Conectarlo a una red WiFi",
                "Que levante su propio WiFi")) { _, i ->
                when (i) {
                    0 -> {
                        NodoService.instancia?.mandaRed(Nodo.RED_OFF, "", "")
                        aviso("Red apagada. Tiene efecto al reiniciar el nodo.")
                    }
                    else -> credencialesRed(i)
                }
            }.show()
    }

    private fun credencialesRed(modo: Int) {
        val col = android.widget.LinearLayout(this)
        col.orientation = android.widget.LinearLayout.VERTICAL
        col.setPadding(40, 20, 40, 0)
        val t = TextView(this)
        t.text = if (modo == 1)
            "Nombre y clave de la red a la que se conectará el nodo.\n\n" +
            "Ojo: el ESP32 solo ve redes de 2,4 GHz."
        else
            "Nombre y clave del WiFi que levantará el nodo.\n\n" +
            "La clave es obligatoria (8 caracteres o más): un punto de acceso " +
            "abierto dejaría el mando del nodo a cualquiera que pasara cerca.\n\n" +
            "Si dejas el nombre en blanco se llamará PTTLoRa-<indicativo>."
        val eSsid = EditText(this); eSsid.hint = "Nombre de la red"
        eSsid.setText(prefs.wifiSsid)
        val eClave = EditText(this); eClave.hint = "Clave"
        eClave.setText(prefs.wifiClave)
        col.addView(t); col.addView(eSsid); col.addView(eClave)
        AlertDialog.Builder(this)
            .setTitle(if (modo == 1) "Conectar el nodo a una red" else "WiFi propio del nodo")
            .setView(col)
            .setPositiveButton("Guardar en el nodo") { _, _ ->
                /* ⚠️ `trim()`, y no es cosmético: el otro diálogo de WiFi lo
                   hacía y éste no, y un espacio final colado por el teclado del
                   móvil dejó un nodo horas con `wifi=buscando` contra una red
                   que no existe. No hay forma de verlo: el nombre se ve igual
                   con espacio que sin él. La clave NO se toca — ahí un espacio
                   puede ser parte de la contraseña de verdad. */
                val ssid = eSsid.text.toString().trim()
                val clave = eClave.text.toString()
                if (modo == 2 && clave.length < 8) {
                    aviso("El punto de acceso necesita una clave de 8 caracteres " +
                          "o más.")
                    return@setPositiveButton
                }
                prefs.wifiSsid = ssid; prefs.wifiClave = clave
                NodoService.instancia?.mandaRed(
                    if (modo == 1) Nodo.RED_CLIENTE else Nodo.RED_AP, ssid, clave)
                aviso("Guardado. Tiene efecto al reiniciar el nodo.")
            }
            .setNegativeButton("Cancelar", null)
            .show()
    }

    /** Enlaces con otros nodos por Internet.
     *
     *  Une zonas de cobertura que no se oyen por radio. Lo normal es apuntar al
     *  reflector: así el nodo SALE y no hace falta abrir ningún puerto. */
    private fun enlaces() {
        val col = android.widget.LinearLayout(this)
        col.orientation = android.widget.LinearLayout.VERTICAL
        col.setPadding(40, 20, 40, 0)
        val t = TextView(this)
        t.text = "Une por Internet nodos que no se oyen por radio: lo que oye " +
                 "uno lo repite el otro por su antena.\n\n" +
                 "Lo normal es apuntar al punto de reunión — así el nodo sale " +
                 "y no hay que abrir ningún puerto en tu router. Puedes poner " +
                 "dos: el segundo solo se usa si el primero no responde.\n\n" +
                 "El nodo necesita estar conectado a una red con Internet."
        /* El valor de fábrica se enseña SIEMPRE, no sólo como pista del campo
           vacío: quien los borre probando tiene que poder volver a poner lo que
           había sin buscarlo en ningún sitio. */
        val tDef = TextView(this)
        tDef.text = "\nPor defecto:\n  principal  ${Prefs.PorDefecto.ENLACE1}" +
                    "\n  respaldo   ${Prefs.PorDefecto.ENLACE2}\n"
        val e1 = EditText(this); e1.hint = Prefs.PorDefecto.ENLACE1
        e1.setText(prefs.enlace1)
        val e2 = EditText(this); e2.hint = Prefs.PorDefecto.ENLACE2
        e2.setText(prefs.enlace2)
        col.addView(t); col.addView(tDef); col.addView(e1); col.addView(e2)
        AlertDialog.Builder(this)
            .setTitle("Enlace con otros nodos")
            .setNeutralButton("Poner los de fábrica") { _, _ ->
                prefs.enlace1 = Prefs.PorDefecto.ENLACE1
                prefs.enlace2 = Prefs.PorDefecto.ENLACE2
                val s2 = NodoService.instancia
                s2?.mandaEnlace("", Nodo.PUERTO_ENLACE)
                s2?.mandaEnlace(prefs.enlace1, Nodo.PUERTO_ENLACE)
                s2?.mandaEnlace(prefs.enlace2, Nodo.PUERTO_ENLACE)
                aviso("Enlaces restaurados. El nodo se conecta en unos segundos.")
            }
            .setView(col)
            .setPositiveButton("Guardar en el nodo") { _, _ ->
                prefs.enlace1 = e1.text.toString().trim()
                prefs.enlace2 = e2.text.toString().trim()
                val s = NodoService.instancia
                // Vacío primero: es como se sueltan los que hubiera.
                s?.mandaEnlace("", Nodo.PUERTO_ENLACE)
                if (prefs.enlace1.isNotBlank())
                    s?.mandaEnlace(prefs.enlace1, Nodo.PUERTO_ENLACE)
                if (prefs.enlace2.isNotBlank())
                    s?.mandaEnlace(prefs.enlace2, Nodo.PUERTO_ENLACE)
                aviso(if (prefs.enlace1.isBlank()) "Enlaces soltados."
                      else "Guardado. El nodo se enlaza solo en unos segundos.")
            }
            .setNeutralButton("Usar los de siempre") { _, _ ->
                prefs.enlace1 = REFLECTOR
                prefs.enlace2 = REFLECTOR_RESPALDO
                val s = NodoService.instancia
                s?.mandaEnlace("", Nodo.PUERTO_ENLACE)
                s?.mandaEnlace(REFLECTOR, Nodo.PUERTO_ENLACE)
                s?.mandaEnlace(REFLECTOR_RESPALDO, Nodo.PUERTO_ENLACE)
                aviso("Puestos $REFLECTOR y, de respaldo, $REFLECTOR_RESPALDO.")
            }
            .setNegativeButton("Cancelar", null)
            .show()
    }

    /** REGISTRO: lo que ha entrado y lo que ha dicho el nodo.
     *
     *  Una recepción dura segundos y el aviso de la pantalla se va con ella;
     *  aquí queda. Y para dar soporte vale su peso: cada transmisión sale
     *  resumida en una línea con cuántos lotes llegaron y con qué señal, que
     *  es la diferencia entre "no se oye bien" y "llegan 9 de 13 a -112 dBm".
     *  El botón de copiar existe para poder pegarlo en un mensaje sin tener
     *  que transcribirlo a mano. */
    private fun registro() {
        val lineas = NodoService.instancia?.registro().orEmpty()
        val texto = if (lineas.isEmpty()) "Todavía no hay nada apuntado."
                    else lineas.reversed().joinToString("\n")
        val v = TextView(this)
        v.text = texto
        v.setTextIsSelectable(true)
        v.setPadding(30, 20, 30, 20)
        v.textSize = 12f
        v.typeface = android.graphics.Typeface.MONOSPACE
        AlertDialog.Builder(this)
            .setTitle("Registro (${lineas.size})")
            .setView(android.widget.ScrollView(this).apply { addView(v) })
            .setPositiveButton("Cerrar", null)
            .setNeutralButton("Copiar") { _, _ ->
                val cm = getSystemService(android.content.Context.CLIPBOARD_SERVICE)
                         as android.content.ClipboardManager
                cm.setPrimaryClip(android.content.ClipData.newPlainText("PTT LoRa", texto))
                Toast.makeText(this, "Registro copiado", Toast.LENGTH_SHORT).show()
            }
            .setNegativeButton("Limpiar") { _, _ ->
                NodoService.instancia?.limpiaRegistro()
            }
            .show()
    }

    /** CERRAR DE VERDAD.
     *
     *  Esto vive en un servicio en primer plano para poder oír con la pantalla
     *  apagada y el móvil en el bolsillo — que es el uso normal. La otra cara
     *  de eso es que salir de la pantalla NO cierra nada, y hasta ahora la
     *  única forma de pararlo era ir a Android y forzar el cierre. Un botón.
     *
     *  Con confirmación, porque el error caro no es cerrar de más: es creer que
     *  estás a la escucha y no estarlo. */
    private fun salir() {
        AlertDialog.Builder(this)
            .setTitle("Cerrar PTT LoRa")
            .setMessage("Se suelta el nodo y dejas de recibir. Habrá que abrir " +
                        "la app otra vez para volver a estar a la escucha.")
            .setPositiveButton("Cerrar") { _, _ ->
                NodoService.instancia?.desconecta()
                stopService(Intent(this, NodoService::class.java))
                if (Build.VERSION.SDK_INT >= 21) finishAndRemoveTask() else finish()
            }
            .setNegativeButton("Seguir", null)
            .show()
    }

    /** ACTUALIZAR EL FIRMWARE DEL NODO DESDE EL MOVIL.
     *
     *  Para no depender de un cable, que es lo que hace que un nodo no se
     *  actualice nunca: uno cliente lo lleva alguien en el bolsillo y una celda
     *  puede estar en un tejado.
     *
     *  Se elige un `.bin` de los que haya en el movil (normalmente descargado
     *  antes del navegador). No se descarga aqui a proposito: asi funciona sin
     *  Internet, que es medio sentido de este proyecto. */
    private fun eligeFirmware() {
        val svc = NodoService.instancia
        if (svc == null || !svc.enlazado) {
            aviso("Primero hay que estar enlazado con el nodo.")
            return
        }
        AlertDialog.Builder(this)
            .setTitle("Actualizar el firmware del nodo")
            .setMessage("Elige el fichero .bin del firmware.\n\n" +
                "Tarda unos minutos y el nodo se reinicia al acabar. " +
                "No cierres la app ni te alejes del nodo mientras dure.\n\n" +
                "⚠️ Si se corta a medias no pasa nada: el nodo sigue con el " +
                "firmware que tenía y se puede volver a intentar.")
            .setPositiveButton("Elegir fichero") { _, _ ->
                val i = android.content.Intent(android.content.Intent.ACTION_GET_CONTENT)
                i.type = "*/*"
                i.addCategory(android.content.Intent.CATEGORY_OPENABLE)
                try {
                    startActivityForResult(
                        android.content.Intent.createChooser(i, "Firmware .bin"), 77)
                } catch (e: Exception) {
                    aviso("No hay ningún explorador de ficheros: ${e.message}")
                }
            }
            .setNegativeButton("Dejarlo", null)
            .show()
    }

    override fun onActivityResult(codigo: Int, resultado: Int,
                                  datos: android.content.Intent?) {
        super.onActivityResult(codigo, resultado, datos)
        if (codigo != 77 || resultado != RESULT_OK) return
        val uri = datos?.data ?: return
        val bin = try {
            contentResolver.openInputStream(uri)?.use { it.readBytes() }
        } catch (e: Exception) {
            aviso("No se pudo leer el fichero: ${e.message}"); return
        } ?: return
        subeFirmware(bin)
    }

    private fun subeFirmware(bin: ByteArray) {
        val barra = android.widget.ProgressBar(
            this, null, android.R.attr.progressBarStyleHorizontal)
        barra.max = 100
        val texto = TextView(this)
        texto.text = "Empezando…"
        val col = android.widget.LinearLayout(this)
        col.orientation = android.widget.LinearLayout.VERTICAL
        col.setPadding(40, 30, 40, 10)
        col.addView(texto); col.addView(barra)

        val dlg = AlertDialog.Builder(this)
            .setTitle("Actualizando el nodo")
            .setView(col)
            .setCancelable(false)
            .setNegativeButton("Cancelar") { _, _ ->
                NodoService.instancia?.ota?.cancela()
            }
            .show()

        /* La pantalla encendida mientras dura: si el movil se duerme, Android
           frena el BLE y la subida se arrastra o se corta. */
        window.addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        Thread({
            val fallo = NodoService.instancia?.actualizaFirmware(bin) { pct, msg ->
                ui.post { barra.progress = pct; texto.text = msg }
            } ?: "sin servicio"
            ui.post {
                window.clearFlags(
                    android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
                try { dlg.dismiss() } catch (_: Exception) {}
                AlertDialog.Builder(this)
                    .setTitle(if (fallo == null) "Nodo actualizado"
                              else "No se pudo actualizar")
                    .setMessage(if (fallo == null)
                        "El nodo se está reiniciando con el firmware nuevo. " +
                        "La app volverá a enlazar sola en unos segundos."
                        else "$fallo\n\nEl nodo sigue con el firmware que " +
                             "tenía. Puedes volver a intentarlo.")
                    .setPositiveButton("Vale", null).show()
            }
        }, "ota").start()
    }

    /** EL SEGUNDO CAMINO: por Internet, además de por radio.
     *
     *  No sustituye a la radio, la respalda. Se manda por los dos a la vez y
     *  quien recibe descarta el duplicado; así da igual cuál de los dos falle.
     *
     *  Y se puede apagar, que no es un detalle: hay quien no quiere que su voz
     *  salga a Internet, y esa es una decisión suya. Con esto apagado el
     *  sistema es exactamente lo que era — radio y nada más. */
    private fun caminoDeDatos(luego: () -> Unit) {
        val col = android.widget.LinearLayout(this)
        col.orientation = android.widget.LinearLayout.VERTICAL
        col.setPadding(40, 20, 40, 0)
        val t = TextView(this)
        t.text = "Servidor de datos (host:puerto):"
        val e = EditText(this)
        e.setText("%s:%d".format(prefs.datosHost, prefs.datosPuerto))
        e.setSingleLine(true)
        val tDef = TextView(this)
        tDef.text = "Por defecto: ${Prefs.PorDefecto.DATOS_HOST}:" +
                    "${Prefs.PorDefecto.DATOS_PUERTO}"
        col.addView(t); col.addView(e); col.addView(tDef)

        AlertDialog.Builder(this)
            .setTitle("Camino de datos")
            .setMessage("Además de por radio, manda y recibe por Internet.\n\n" +
                "Sirve para seguir hablando cuando la radio no llega. Se manda " +
                "por los dos caminos a la vez y el duplicado se descarta solo.\n\n" +
                "En la pantalla verás de dónde llega cada transmisión: " +
                "📻 por radio o 🌐 por Internet. Si de pronto todo entra por " +
                "Internet teniendo al otro cerca, es que la radio ha dejado de " +
                "llegar.\n\n" +
                "Apagado, el sistema funciona solo por radio, como siempre.")
            .setView(col)
            .setPositiveButton(if (prefs.datosActivo) "Guardar" else "Activar") { _, _ ->
                val txt = e.text.toString().trim()
                /* Vacío = el de fábrica, no un error: si alguien borra el
                   campo probando, lo peor que puede pasar es que vuelva a
                   funcionar como venía. */
                val host = txt.substringBefore(':').ifBlank { Prefs.PorDefecto.DATOS_HOST }
                val pto = txt.substringAfter(':', "").toIntOrNull()
                    ?: Prefs.PorDefecto.DATOS_PUERTO
                prefs.datosHost = host
                prefs.datosPuerto = pto
                prefs.datosActivo = true
                NodoService.instancia?.conectaDatos(true)
                luego()
            }
            .setNegativeButton("Apagar") { _, _ ->
                prefs.datosActivo = false
                NodoService.instancia?.conectaDatos(true)
                luego()
            }
            .setNeutralButton("Dejarlo", null)
            .show()
    }

    /** Encender o apagar la posición del móvil en la baliza del nodo.
     *
     *  Se explica lo que hace ANTES de encenderla, y en particular lo que no se
     *  puede deshacer: **la posición por RF va en claro y la repite media red**.
     *  Quien la enciende tiene que saber que no es un dato que viaje a un
     *  servidor de confianza, sino algo que emite su estación por la antena,
     *  como el indicativo. */
    private fun posicion(luego: () -> Unit) {
        if (prefs.posActiva) {
            /* Se enseña LO ÚLTIMO QUE SE LE MANDÓ AL NODO, no un "encendida" a
               secas: encendida y sin haber mandado nunca nada se ven igual, y
               esa confusión costó una mañana el 9-sep. */
            val estado = NodoService.instancia?.posTexto ?: "el servicio no está en marcha"
            AlertDialog.Builder(this)
                .setTitle("Posición y APRS")
                .setMessage("Tu posición sale en el INICIO de cada transmisión: " +
                    "cuando aprietas el PTT, no cada minuto.\n\n" +
                    "Va por radio, y desde una celda con Internet puede " +
                    "publicarse en APRS-IS y verse en aprs.fi.\n\n" +
                    "Último envío al nodo:\n$estado")
                .setPositiveButton("Apagar") { _, _ ->
                    prefs.posActiva = false
                    NodoService.instancia?.posicion(false)
                    luego()
                }
                .setNegativeButton("Dejarlo", null)
                .show()
            return
        }
        AlertDialog.Builder(this)
            .setTitle("Posición")
            .setMessage("Tu posición viajará en el INICIO de cada transmisión, " +
                "cuando aprietas el PTT — no cada minuto.\n\n" +
                "SALE POR DOS SITIOS, y conviene tener claros los dos:\n\n" +
                "1) POR RADIO Y EN CLARO. La repiten los demás nodos, así que " +
                "cualquiera con un receptor puede leerla. No es como mandarla " +
                "a un servidor: es algo que emite tu estación, igual que el " +
                "indicativo.\n\n" +
                "2) EN APRS-IS, si una celda con Internet la publica. Entonces " +
                "sales en aprs.fi con tu indicativo — que es PÚBLICO, lo ve " +
                "cualquiera, y queda guardado en el histórico PARA SIEMPRE. " +
                "Borrarlo después no está en tu mano.\n\n" +
                "Apagándola, el nodo la olvida y deja de publicarla.")
            .setPositiveButton("Encender") { _, _ ->
                /* El permiso se pide AQUI y no al arrancar la app: pedir la
                   ubicación nada más abrir, para algo que la mayoría no va a
                   usar, es lo que hace que la gente se la deniegue a todo. */
                if (Build.VERSION.SDK_INT >= 23 &&
                    checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION)
                        != PackageManager.PERMISSION_GRANTED) {
                    requestPermissions(
                        arrayOf(Manifest.permission.ACCESS_FINE_LOCATION), 3)
                }
                prefs.posActiva = true
                NodoService.instancia?.posicion(true)
                luego()
            }
            .setNegativeButton("Dejarlo", null)
            .show()
    }

    private fun aviso(texto: String) {
        AlertDialog.Builder(this).setMessage(texto)
            .setPositiveButton("Vale", null).show()
    }

    private fun eligeNodo(luego: () -> Unit) {
        // Se listan TODOS los emparejados, no solo los que se llaman PTTLoRa:
        // si el movil tiene el nodo guardado con un nombre viejo en cache
        // (pasa, y mucho), filtrar por nombre lo esconderia justo cuando mas
        // falta hace verlo.
        val lista = Nodo({ _, _ -> }, { _, _ -> }).emparejados()
        /* EL NODO YA ELEGIDO, EL PRIMERO — y era el fallo de fondo del
           "no lo guarda": un nodo encontrado por busqueda NO queda emparejado
           (el SPP no lo necesita), asi que no salia en esta lista de
           emparejados aunque estuviera guardado. Quien perdia el enlace se
           encontraba con una lista donde su nodo no estaba y tenia que volver
           a buscarlo, con toda la razon del mundo para pensar que la app no
           recuerda nada. */
        val guardado = prefs.mac
        /* `it.name` lanza SecurityException en Android 12+ sin
           BLUETOOTH_CONNECT: sin proteger, elegir nodo tiraba la app justo
           donde no hay otra salida. */
        fun comoSeLlama(d: android.bluetooth.BluetoothDevice): String? =
            try { d.name } catch (e: SecurityException) { null }
        val nombres = lista.map {
            "${comoSeLlama(it) ?: "(sin nombre)"}\n${it.address}" }.toMutableList()
        val hayGuardado = guardado.isNotBlank() &&
            lista.none { it.address.equals(guardado, true) }
        if (hayGuardado) nombres.add(0,
            "${prefs.nombreNodo.ifBlank { "Nodo guardado" }}\n$guardado  ·  guardado")
        nombres.add("Buscar nodos cerca…")
        nombres.add("Escribir la MAC a mano…")

        AlertDialog.Builder(this)
            .setTitle(if (lista.isEmpty()) "Sin dispositivos emparejados"
                      else "Elegir nodo")
            .setItems(nombres.toTypedArray()) { _, i ->
                // El guardado ocupa la fila 0 cuando lo hay, asi que todo lo
                // demas va corrido una posicion.
                val desplaza = if (hayGuardado) 1 else 0
                val j = i - desplaza
                when {
                    hayGuardado && i == 0 -> luego()      // seguir con el de siempre
                    j == lista.size -> buscaNodos(luego)
                    j > lista.size -> macAMano(luego)
                    else -> {
                        prefs.mac = lista[j].address
                        prefs.nombreNodo = comoSeLlama(lista[j]) ?: lista[j].address
                        luego()
                    }
                }
            }
            .setNeutralButton("Ayuda") { _, _ ->
                AlertDialog.Builder(this)
                    .setTitle("No aparece el nodo, o no conecta")
                    .setMessage("El nodo tiene que llevar firmware 1.26 o " +
                        "más nuevo. Desde esa versión el enlace es Bluetooth " +
                        "BLE, y esta app ya no habla con nodos anteriores.\n\n" +
                        "NO hay que emparejar nada desde los ajustes de " +
                        "Android: usa «Buscar nodos cerca». La puerta del " +
                        "nodo es el código de 6 cifras que enseña en su " +
                        "pantalla, y no tiene nada que ver con el " +
                        "emparejamiento de Bluetooth.\n\n" +
                        "Si tenías el nodo emparejado de antes y no conecta, " +
                        "olvídalo desde los ajustes de Bluetooth: un " +
                        "emparejamiento clásico a medias puede estorbar.\n\n" +
                        "En la búsqueda solo salen nodos ${Nodo.PREFIJO}, " +
                        "con su nombre desde el primer momento. Si no " +
                        "aparece ninguno: mira que el nodo esté encendido y " +
                        "que no tenga ya otro móvil conectado — solo admite " +
                        "uno a la vez, y mientras lo tiene deja de anunciarse.")
                    .setPositiveButton("Vale", null).show()
            }

            .show()
    }

    /** Busca nodos por radio (escaneo BLE), sin emparejar.
     *
     *  Emparejar desde los ajustes de Android no hace falta y nunca hizo falta:
     *  el codigo de 6 cifras que pide el nodo NO es el del emparejamiento de
     *  Bluetooth. Se puede emparejar sin ver ningun codigo y seguir sin poder
     *  usar el nodo, que es exactamente lo que despista. */
    private fun buscaNodos(luego: () -> Unit) {
        // En Android 12+ hace falta BLUETOOTH_SCAN; en 6-11, ubicación.
        val faltan = ArrayList<String>()
        if (Build.VERSION.SDK_INT >= 31) {
            if (checkSelfPermission("android.permission.BLUETOOTH_SCAN")
                    != PackageManager.PERMISSION_GRANTED)
                faltan.add("android.permission.BLUETOOTH_SCAN")
        } else if (Build.VERSION.SDK_INT >= 23) {
            if (checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION)
                    != PackageManager.PERMISSION_GRANTED)
                faltan.add(Manifest.permission.ACCESS_FINE_LOCATION)
        }
        if (faltan.isNotEmpty()) {
            requestPermissions(faltan.toTypedArray(), 2)
            aviso("Android pide permiso para buscar por Bluetooth. Concédelo y " +
                  "vuelve a intentarlo.\n\nSi prefieres no darlo, empareja el " +
                  "nodo desde los ajustes del móvil o escribe su MAC.")
            return
        }

        /* La lista se REPINTA cada vez que se sabe algo nuevo.
         *
         * En BLE el nombre viene ya en el anuncio, asi que esto es mucho menos
         * dramatico que con el descubrimiento clasico —donde el nombre llegaba
         * segundos despues y la lista era una columna de MAC—, pero se mantiene:
         * un anuncio puede llegar sin nombre (cabe justo en 31 bytes) y
         * completarse en el siguiente. Se guarda lo que se sabe de cada MAC y se
         * vuelve a pintar entera. */
        val nombres = LinkedHashMap<String, String?>()
        var macs = listOf<String>()
        val n = Nodo({ _, _ -> }, { _, _ -> })
        val lista = android.widget.ArrayAdapter<String>(
            this, android.R.layout.simple_list_item_1)
        // El testigo del escaneo BLE: se lo devuelve `Nodo.busca` y hay que
        // pasárselo a `paraBusqueda`. Es opaco a propósito (una API u otra
        // según la versión de Android).
        var receptor: Any? = null

        fun repinta() {
            val ordenadas = nombres.keys.sortedWith(compareBy(
                { !(nombres[it] ?: "").startsWith(Nodo.PREFIJO) },   // nodos arriba
                { nombres[it] == null },                             // luego los que dicen su nombre
                { nombres[it] ?: it }))
            macs = ordenadas
            lista.clear()
            for (mac in ordenadas) {
                val nom = nombres[mac]
                lista.add(if (nom != null) "$nom\n$mac"
                          else "$mac\n(todavía no dice su nombre)")
            }
            lista.notifyDataSetChanged()
        }

        val dlg = AlertDialog.Builder(this)
            .setTitle("Buscando nodos…")
            .setAdapter(lista) { _, i ->
                val mac = macs[i]
                prefs.mac = mac
                prefs.nombreNodo = nombres[mac] ?: mac
                n.paraBusqueda(this, receptor)
                luego()
            }
            .setNegativeButton("Parar") { _, _ -> n.paraBusqueda(this, receptor) }
            .setOnDismissListener { n.paraBusqueda(this, receptor) }
            .show()

        receptor = n.busca(this, { dev, nombre ->
            // Los que no dicen su nombre se enseñan igual: puede ser el nodo
            // tardando en contestar, y se le puede elegir por la MAC.
            val nuevo = !nombres.containsKey(dev.address)
            val mejora = nombre != null && nombres[dev.address] == null
            if (nuevo || mejora) {
                nombres[dev.address] = nombre ?: nombres[dev.address]
                ui.post { repinta() }
            }
        }, {
            ui.post {
                val cuantos = nombres.keys.count {
                    (nombres[it] ?: "").startsWith(Nodo.PREFIJO) }
                dlg.setTitle(when {
                    nombres.isEmpty() -> "No se vio ningún dispositivo"
                    cuantos > 0 -> "Elegir nodo"
                    // Sin ningun PTTLoRa a la vista, lo util es decir por que.
                    else -> "Sin nodos a la vista"
                })
            }
        })
    }

    /** Frecuencia, canal lógico y potencia.
     *
     *  Los rangos los pone el NODO, no la app: el estado del nodo trae
     *  `hw=<fmin>-<fmax>MHz/<pmin>-<pmax>dBm` y de ahí salen los límites. Si
     *  aún no hemos hablado con él, se usan los del SX1278 de estas placas.
     *
     *  La potencia es responsabilidad del operador: en ISM 433 el límite legal
     *  son 10 mW, en banda de aficionado se puede más. Por eso se avisa pero no
     *  se impide.
     *
     *  La FRECUENCIA es otra cosa y ahí sí se impide: fuera de 430-440 MHz esto
     *  no es un ajuste discutible, es transmitir donde no toca. El nodo lo
     *  rechaza de todas formas (ver `CMD_RADIO`); esto solo hace que el aviso
     *  salga aquí, con el teclado delante, en vez de tres segundos después en
     *  el registro. Quien tenga otra atribución cambia `BANDA_MIN`/`BANDA_MAX`
     *  en el firmware y compila lo suyo. */
    /** Radio: **sólo frecuencia y potencia**.
     *
     *  El canal lógico, el spreading factor y el ancho de banda se fueron a
     *  «Avanzado» (pulsación larga en el botón). No es por esconderlos: es que
     *  **cambiarlos rompe la red y el síntoma no lo explica**. Con un SF
     *  distinto dos nodos no se oyen peor — no se oyen, y el usuario lo vive
     *  como «de repente no hay nadie». Frecuencia y potencia, en cambio, se
     *  entienden solas y son las que uno quiere tocar de verdad. */
    private fun ajustesRadio(luego: () -> Unit) {
        val hw = NodoService.instancia?.limitesHw
        val pMin = hw?.get(2)?.toInt() ?: 2
        val pMax = hw?.get(3)?.toInt() ?: 17
        val banda = NodoService.instancia?.limitesBanda
        val fMin = banda?.get(0) ?: 430f
        val fMax = banda?.get(1) ?: 440f

        val col = android.widget.LinearLayout(this)
        col.orientation = android.widget.LinearLayout.VERTICAL
        col.setPadding(40, 20, 40, 0)

        val tFrec = TextView(this)
        tFrec.text = ("Frecuencia en MHz — por defecto %s\n\n" +
                      "Tiene que ser LA MISMA en todos los nodos que se quieran " +
                      "oír. Rango permitido: %.0f–%.0f, banda de aficionados; " +
                      "necesitas licencia e indicativo para transmitir aquí.")
                     .format(Prefs.PorDefecto.frecMHz(), fMin, fMax)
        val eFrec = EditText(this)
        eFrec.inputType = android.text.InputType.TYPE_CLASS_NUMBER or
                          android.text.InputType.TYPE_NUMBER_FLAG_DECIMAL
        eFrec.setText("%.3f".format(prefs.frecuenciaKHz / 1000.0))

        val tPot = TextView(this)
        tPot.text = "\nPotencia en dBm ($pMin–$pMax) — por defecto " +
                    "${Prefs.PorDefecto.POTENCIA}\n\n" +
                    "Bajarla no te saca de la red: sólo llegas menos lejos. " +
                    "Para probar dos placas en la misma mesa, ponla en 2."
        val ePot = EditText(this)
        ePot.inputType = android.text.InputType.TYPE_CLASS_NUMBER
        ePot.setText(prefs.potencia.toString())

        val tOtros = TextView(this)
        tOtros.text = "\nCanal ${prefs.canalLogico} · SF ${prefs.sf} · " +
                      "${prefs.anchoKHz} kHz\n(mantén pulsado «Radio» para " +
                      "cambiarlos — sólo si sabes lo que haces)"

        col.addView(tFrec); col.addView(eFrec)
        col.addView(tPot); col.addView(ePot)
        col.addView(tOtros)

        AlertDialog.Builder(this)
            .setTitle("Radio")
            .setView(android.widget.ScrollView(this).apply { addView(col) })
            .setPositiveButton("Aplicar") { _, _ ->
                val f = (eFrec.text.toString().toDoubleOrNull() ?: 0.0) * 1000
                if (f < fMin * 1000 || f > fMax * 1000) {
                    android.widget.Toast.makeText(this,
                        "%.3f MHz está fuera de %.0f–%.0f: no se aplica"
                            .format(f / 1000.0, fMin, fMax),
                        android.widget.Toast.LENGTH_LONG).show()
                    luego()
                    return@setPositiveButton
                }
                prefs.frecuenciaKHz = f.toInt()
                prefs.potencia = ePot.text.toString().toIntOrNull() ?: pMax
                NodoService.instancia?.mandaRadio()
                luego()
            }
            .setNeutralButton("Valores de fábrica") { _, _ ->
                confirma("Volver a los valores de fábrica",
                    "Frecuencia ${Prefs.PorDefecto.frecMHz()} MHz, " +
                    "potencia ${Prefs.PorDefecto.POTENCIA} dBm, canal " +
                    "${Prefs.PorDefecto.CANAL}, SF ${Prefs.PorDefecto.SF}, " +
                    "${Prefs.PorDefecto.ANCHO_KHZ} kHz.\n\n" +
                    "Es la configuración con la que la red funciona.") {
                    prefs.frecuenciaKHz = Prefs.PorDefecto.FREC_KHZ
                    prefs.potencia = Prefs.PorDefecto.POTENCIA
                    prefs.canalLogico = Prefs.PorDefecto.CANAL
                    prefs.sf = Prefs.PorDefecto.SF
                    prefs.anchoKHz = Prefs.PorDefecto.ANCHO_KHZ
                    NodoService.instancia?.mandaRadio()
                    luego()
                }
            }
            .setNegativeButton("Cancelar", null)
            .show()
    }

    /** Lo que puede dejarte fuera de la red, detrás de una pulsación larga. */
    private fun ajustesRadioAvanzado(luego: () -> Unit) {
        val col = android.widget.LinearLayout(this)
        col.orientation = android.widget.LinearLayout.VERTICAL
        col.setPadding(40, 20, 40, 0)
        val t = TextView(this)
        t.text = "⚠️ ESTO TE PUEDE DEJAR FUERA DE LA RED.\n\n" +
                 "Estos tres tienen que ser IDÉNTICOS en todos los nodos. Si " +
                 "no coinciden, no se oyen peor: NO SE OYEN, y no hay ningún " +
                 "aviso que lo explique.\n\n" +
                 "Por defecto: canal ${Prefs.PorDefecto.CANAL}, " +
                 "SF ${Prefs.PorDefecto.SF}, ${Prefs.PorDefecto.ANCHO_KHZ} kHz."
        val eCanal = EditText(this); eCanal.hint = "Canal (0–255)"
        eCanal.inputType = android.text.InputType.TYPE_CLASS_NUMBER
        eCanal.setText(prefs.canalLogico.toString())
        val eSf = EditText(this); eSf.hint = "Spreading factor (7–12)"
        eSf.inputType = android.text.InputType.TYPE_CLASS_NUMBER
        eSf.setText(prefs.sf.toString())
        val eBw = EditText(this); eBw.hint = "Ancho de banda en kHz (125 o 250)"
        eBw.inputType = android.text.InputType.TYPE_CLASS_NUMBER
        eBw.setText(prefs.anchoKHz.toString())
        col.addView(t); col.addView(eCanal); col.addView(eSf); col.addView(eBw)
        AlertDialog.Builder(this)
            .setTitle("Radio · avanzado")
            .setView(android.widget.ScrollView(this).apply { addView(col) })
            .setPositiveButton("Aplicar") { _, _ ->
                prefs.canalLogico = eCanal.text.toString().toIntOrNull() ?: Prefs.PorDefecto.CANAL
                prefs.sf = eSf.text.toString().toIntOrNull() ?: Prefs.PorDefecto.SF
                prefs.anchoKHz = eBw.text.toString().toIntOrNull() ?: Prefs.PorDefecto.ANCHO_KHZ
                NodoService.instancia?.mandaRadio()
                luego()
            }
            .setNegativeButton("Cancelar", null)
            .show()
    }

    /** Confirmación corta y reutilizable para lo que se puede romper. */
    private fun confirma(titulo: String, texto: String, hazlo: () -> Unit) {
        AlertDialog.Builder(this)
            .setTitle(titulo).setMessage(texto)
            .setPositiveButton("Sí") { _, _ -> hazlo() }
            .setNegativeButton("Dejarlo", null)
            .show()
    }

    /** WiFi del nodo. Es opcional y solo sirve para poder actualizarle el
     *  firmware por red, sin bajarlo del sitio donde esté. */
    private fun wifiDelNodo(luego: () -> Unit) {
        val col = android.widget.LinearLayout(this)
        col.orientation = android.widget.LinearLayout.VERTICAL
        col.setPadding(40, 20, 40, 0)
        val eS = EditText(this); eS.hint = "Nombre de la red (2,4 GHz)"
        eS.setText(prefs.wifiSsid)
        val eC = EditText(this); eC.hint = "Contraseña"
        eC.setText(prefs.wifiClave)
        eC.inputType = android.text.InputType.TYPE_CLASS_TEXT or
                       android.text.InputType.TYPE_TEXT_VARIATION_PASSWORD
        col.addView(eS); col.addView(eC)

        AlertDialog.Builder(this)
            .setTitle("WiFi del nodo (opcional)")
            .setMessage("Sirve para actualizar el firmware del nodo por red, " +
                        "sin tener que bajarlo. El nodo funciona igual sin " +
                        "esto.\n\nGuardar aquí también pone el nodo en modo " +
                        "cliente: si estaba levantando su propia red (AP), " +
                        "esto es lo que lo devuelve a la tuya.\n\n" +
                        "OJO: el nodo solo ve redes de 2,4 GHz.")
            .setView(col)
            .setPositiveButton("Guardar") { _, _ ->
                prefs.wifiSsid = eS.text.toString().trim()
                prefs.wifiClave = eC.text.toString()
                NodoService.instancia?.mandaWifi()
                /* ⚠️ NO hay que reiniciar nada, y decirlo estaba haciendo daño:
                   el firmware aplica el WiFi EN EL ACTO desde hace tiempo (ver
                   CMD_WIFI en main.cpp, "WiFi guardado; conectando ahora"), y
                   además es esto lo que saca al nodo del modo AP. Con el texto
                   viejo, quien tenía el nodo en AP reiniciaba una y otra vez
                   —que es justo lo que NO lo arregla, porque al arrancar vuelve
                   a su ajuste guardado— en vez de mandar el comando. */
                Toast.makeText(this, "Enviado: el nodo se conecta ahora, " +
                               "sin reiniciar.", Toast.LENGTH_LONG).show()
                luego()
            }
            .setNeutralButton("Quitar") { _, _ ->
                prefs.wifiSsid = ""; prefs.wifiClave = ""
                NodoService.instancia?.mandaWifi()
                luego()
            }
            .setNegativeButton("Cancelar", null)
            .show()
    }

    /** Salida de emergencia: conectar por MAC sin pasar por el emparejamiento.
     *  El SPP del nodo acepta la conexion directa, asi que esto funciona
     *  aunque Android se empeñe en no listarlo. */
    private fun macAMano(luego: () -> Unit) {
        val e = EditText(this)
        e.hint = "AA:BB:CC:DD:EE:FF"
        e.setText(prefs.mac)
        AlertDialog.Builder(this)
            .setTitle("MAC del nodo")
            .setView(e)
            .setPositiveButton("Usar") { _, _ ->
                val m = e.text.toString().trim().uppercase()
                if (Regex("^([0-9A-F]{2}:){5}[0-9A-F]{2}$").matches(m)) {
                    prefs.mac = m
                    prefs.nombreNodo = m
                    luego()
                } else {
                    Toast.makeText(this, "MAC no válida", Toast.LENGTH_SHORT).show()
                }
            }
            .setNegativeButton("Cancelar", null)
            .show()
    }

    /** El nodo pide el código que enseña en su pantalla. */
    override fun onPideCodigo() { ui.post {
        /* Ademas del dialogo, se deja dicho en la propia pantalla y se puede
           tocar para volver a abrirlo: un dialogo que sale en mal momento (o
           que se cierra sin querer) dejaria al usuario mirando un enlace que no
           responde, sin ninguna pista de por que. */
        tEstado.text = "El nodo pide un código · toca aquí"
        tEstado.setTextColor(Color.parseColor("#ff8f00"))
        tEstado.setOnClickListener { dialogoCodigo = false; onPideCodigo() }
        if (dialogoCodigo) return@post
        dialogoCodigo = true
        val e = EditText(this)
        e.hint = "000000"
        e.inputType = android.text.InputType.TYPE_CLASS_NUMBER
        AlertDialog.Builder(this)
            .setTitle("Código del nodo")
            .setMessage("El nodo está mostrando un código de 6 cifras en su " +
                        "pantalla. Escríbelo aquí para autorizar este móvil.\n\n" +
                        "Solo hace falta la primera vez.")
            .setView(e)
            .setCancelable(false)
            .setPositiveButton("Autorizar") { _, _ ->
                dialogoCodigo = false
                tEstado.setOnClickListener(null)
                NodoService.instancia?.autoriza(e.text.toString())
            }
            .setNegativeButton("Ahora no") { _, _ -> dialogoCodigo = false }
            .show()
    } }

    // ---------------------------------------------------------- observador ---
    override fun onEnlace(conectado: Boolean, detalle: String) { ui.post {
        /* El motivo, tal cual. "Sin enlace · sin enlace" no le sirve a nadie:
           el mensaje de la excepcion es lo unico que distingue "el nodo esta
           apagado" de "otro cliente tiene la ranura" o "falta un permiso". */
        var motivo = detalle.ifBlank {
            NodoService.instancia?.ultimoFallo.orEmpty().ifBlank { "sin enlace" } }
        /* El nodo admite UN móvil por Bluetooth, y si ya lo tiene ocupado por
           otro que está vivo, rechaza. Sin esta pista el síntoma es idéntico al
           de un nodo apagado, y se acaba buscando el fallo donde no está. */
        if (!conectado && motivo.contains("read failed", true))
            motivo += " (¿lo tiene ya otro móvil?)" 
        tEstado.text = if (conectado) "Enlazado · $detalle"
                       else "Sin enlace · $motivo · toca para reintentar"
        tEstado.setTextColor(if (conectado) Color.parseColor("#2e7d32") else Color.GRAY)
        /* Tocar el estado reintenta AHORA. El servicio ya reintenta solo cada
           pocos segundos, pero cuando uno acaba de encender el nodo no quiere
           esperar a la siguiente vuelta: sin este atajo, la unica salida
           visible era entrar en los ajustes a re-elegir el nodo, que es de
           donde salia la sensacion de que la app no guarda nada. */
        tEstado.setOnLongClickListener { registro(); true }
        if (!conectado) tEstado.setOnClickListener {
            tEstado.text = "Conectando…"
            NodoService.instancia?.reintenta()
        } else tEstado.setOnClickListener(null)
        bPtt.isEnabled = conectado
    } }

    override fun onCanal(ocupado: Boolean) { ui.post {
        // Verde: puedes hablar. Ámbar: hay alguien. Es la única disciplina que
        // existe en un canal simplex, así que tiene que verse de un vistazo.
        bPtt.setBackgroundColor(
            if (ocupado) Color.parseColor("#ff8f00") else Color.parseColor("#2e7d32"))
        bPtt.text = if (ocupado) "CANAL OCUPADO" else "PULSAR PARA HABLAR"
    } }

    override fun onQuienHabla(indicativo: String?, rssi: Int) { ui.post {
        // rssi 127 no es una medida: es la marca de que la voz no vino por la
        // antena, sino de alguien colgado de este mismo nodo (o del otro lado
        // de un enlace). Enseñar "127 dBm" sería mentir.
        tQuien.text = when {
            indicativo == null -> ""
            rssi == 127 -> "◀ $indicativo   (por red)"
            else -> "◀ $indicativo   $rssi dBm"
        }
    } }

    /** Arbitraje del micrófono: con varios usuarios en el mismo nodo, o con
     *  varias ubicaciones enlazadas, el PTT puede estar cogido. Se enseña de
     *  quién en vez de dejar un botón que no responde y no se sabe por qué. */
    override fun onPtt(estado: Int, quien: String?) { ui.post {
        if (estado == Nodo.PTT_DE_OTRO) {
            bPtt.setBackgroundColor(Color.parseColor("#ff8f00"))
            bPtt.text = if (quien.isNullOrBlank()) "OCUPADO" else "HABLA $quien"
            tQuien.text = if (quien.isNullOrBlank()) "" else "◀ $quien"
        } else if (estado == Nodo.PTT_LIBRE) {
            bPtt.setBackgroundColor(Color.parseColor("#2e7d32"))
            bPtt.text = "PULSAR PARA HABLAR"
        }
    } }

    override fun onTx(transmitiendo: Boolean) { ui.post {
        bPtt.setBackgroundColor(
            if (transmitiendo) Color.parseColor("#c62828") else Color.parseColor("#2e7d32"))
        bPtt.text = if (transmitiendo) "TRANSMITIENDO" else "PULSAR PARA HABLAR"
    } }

    /** LA RUEDA: qué se ha dicho, en qué orden y cuánto ha durado.
     *
     *  Una fila POR TRANSMISION y no por persona: lo que hace falta ver es el
     *  orden real —quién entró detrás de quién— y con una sola fila por
     *  indicativo eso se pierde en cuanto alguien habla dos veces.
     *
     *  ⚠️ **Y las propias van dentro.** Sin ellas la lista miente sobre el
     *  orden: enseña a quién oíste pero no cuándo entraste tú entre medias, que
     *  es justo lo que hace falta para seguir la rueda en un QSO con varios.
     *
     *  Se pinta a mano en un LinearLayout en vez de con una lista y su
     *  adaptador: son veinte filas como mucho y así no entra ni una dependencia
     *  más en una app que cabe en 400 KB. */
    private val reloj = java.text.SimpleDateFormat("HH:mm:ss", java.util.Locale.US)

    private fun pintaRueda() {
        val svc = NodoService.instancia
        val lista = svc?.ultimosHablantes() ?: emptyList()
        rueda.removeAllViews()
        if (lista.isEmpty()) {
            val t = TextView(this)
            t.text = "todavía no ha hablado nadie"
            t.textSize = 13f
            t.setTextColor(resources.getColor(R.color.texto_flojo))
            rueda.addView(t)
            return
        }
        for (h in lista) {
            /* El nombre, si se conoce. Nunca bloquea: si aún no ha llegado sale
               el indicativo a secas y la línea se repinta sola cuando llegue. */
            val nombre = Nombres.de(h.indicativo, prefs.nombresActivo)
            val quien = if (nombre != null) "${h.indicativo} ${nombre}" else h.indicativo
            /* La duración se escribe al CERRAR la transmisión, así que mientras
               alguien habla todavía no la hay: se dice que está en curso en vez
               de enseñar un 0,0 s que sería mentira. */
            val dura = if (h.segundos > 0) "%.1fs".format(h.segundos) else "···"
            /* Tres orígenes y tres marcas, porque significan cosas
               distintas: 📻 ha cruzado el aire, 🌐 ha dado la vuelta por
               Internet, y 👥 ni una cosa ni la otra — el que habla está colgado
               de MI MISMO nodo. Verlo importa: 👥 no dice nada de la cobertura,
               y hasta la 0.9.41 salía como 🌐. */
            val via = when {
                h.yo -> if (h.porRf) "▶📻" else "▶🌐"
                h.rssi == 126 -> "👥"
                h.porRf && h.rssi < 126 -> "📻${h.rssi}"
                h.porRf -> "📻"
                else -> "🌐"
            }
            val t = TextView(this)
            t.text = "%s  %-18s %6s  %s".format(
                reloj.format(java.util.Date(h.cuando)), quien.take(18), dura, via)
            t.textSize = 14f
            t.typeface = android.graphics.Typeface.MONOSPACE
            t.setTextColor(when {
                // Lo propio, en otro color: de un vistazo se ve dónde entraste.
                h.yo -> Color.parseColor("#1565c0")
                h.segundos <= 0.0 -> Color.parseColor("#2e7d32")   // hablando ahora
                else -> resources.getColor(R.color.texto)
            })
            rueda.addView(t)
        }
    }

    override fun onRueda() { ui.post { pintaRueda() } }

    override fun onLog(texto: String) { ui.post {
        tLog.text = texto
        /* Al autorizar hay que DESHACER el aviso naranja. Sin esto la pantalla
           se quedaba diciendo "el nodo pide un código · toca aquí" con el móvil
           ya dentro, y quien lo lee da por hecho que sigue sin entrar. */
        if (texto.startsWith("autorizado")) {
            tEstado.setOnClickListener(null)
            val s = NodoService.instancia
            onEnlace(s?.enlazado ?: false, "autorizado")
        }
    } }
}
