#!/usr/bin/env bash
# instalar.sh — monta el servidor de PTT LoRa paso a paso, preguntando lo mínimo.
#
#   curl -fsSL https://raw.githubusercontent.com/pyopower/pttLoRa/main/servidor/instalar.sh | sudo bash
#
# o, desde una copia del repositorio:
#
#   sudo servidor/instalar.sh
#
# Vale para cualquier Linux con systemd: un PC o servidor (amd64), una
# Raspberry Pi de la 1 a la 5, la Zero / Zero 2 W, un VPS en la nube... Los
# programas son Python 3 sin dependencias, así que no hay nada que compilar y
# la arquitectura da igual.
#
# Una vez instalado queda como orden: `sudo pttlora-servidor` vuelve a abrir el
# menú (cambiar ajustes, actualizar, ver el estado o desinstalar).
#
# Opciones:   --actualizar   baja los programas nuevos y reinicia, sin preguntar
#             --estado       enseña cómo está cada pieza
#             --desinstalar  lo quita todo (pregunta antes)
#             --texto        sin ventanas, preguntas en texto normal
#
# Instalación sin preguntas (para quien sabe lo que hace, y para los bancos de
# pruebas): PTTLORA_DESATENDIDO=1 y las variables de `guarda_ajustes`, por
# ejemplo  PTTLORA_DESATENDIDO=1 INDICATIVO=EA1ABC PIEZAS="reflector datos" ...

set -u
VERSION_INSTALADOR=1

REPO="${PTTLORA_REPO:-pyopower/pttLoRa}"
RAMA="${PTTLORA_RAMA:-main}"
PREFIJO="${PTTLORA_PREFIJO:-}"           # sólo para pruebas: instala en otra raíz

DIR_PROG="$PREFIJO/opt/pttlora"
DIR_CONF="$PREFIJO/etc/pttlora"
DIR_UNID="$PREFIJO/etc/systemd/system"
DIR_DATOS="$PREFIJO/var/lib/pttlora"
AJUSTES="$DIR_CONF/instalacion.conf"
ORDEN="$PREFIJO/usr/local/sbin/pttlora-servidor"
USUARIO="pttlora"
SERVIDOR_PUBLICO="or.adan.ovh"

PROGRAMAS="nodovirtual.py nododatos.py mandovirtual.py igate.py registro.py"
TODAS="reflector datos registro igate mando"

TITULO="PTT LoRa"
# Sin esto, en PuTTY, JuiceSSH y compañía los bordes salen como «lqqqk».
export NCURSES_NO_UTF8_ACS=1

# ============================================================ la interfaz ====
# Dos caras para las mismas cinco preguntas: ventanas (whiptail) si se puede, y
# texto de toda la vida si no. Todo lo demás del script sólo llama a `ui_*`.

MODO_UI=ventanas
[ -n "${PTTLORA_DESATENDIDO:-}" ] && MODO_UI=desatendido

# El tamaño se calcula de la terminal Y del texto: mucha gente lo lanzará por
# SSH desde el móvil, con 40 columnas, y una ventana más ancha que la pantalla
# es ilegible. NUNCA se usa la barra de desplazamiento de whiptail: con ella,
# Intro no hace nada hasta pulsar Tab, y eso deja atascado a cualquiera. Lo que
# no cabe se reparte en varias ventanas.
medidas() {      # medidas "texto" líneas-extra [filas-de-lista]  -> ANCHO ALTO LISTA CABE
    local c l n
    c=$(tput cols 2>/dev/null || echo 80); l=$(tput lines 2>/dev/null || echo 24)
    ANCHO=$(( c > 78 ? 74 : c - 4 )); [ "$ANCHO" -lt 30 ] && ANCHO=30
    ALTO_MAX=$(( l - 1 )); [ "$ALTO_MAX" -lt 10 ] && ALTO_MAX=10
    n=$(printf '%s\n' "${1:-}" | fold -s -w $(( ANCHO - 5 )) | wc -l)
    ALTO=$(( n + ${2:-7} ))
    CABE=1
    [ "$ALTO" -gt "$ALTO_MAX" ] && { ALTO=$ALTO_MAX; CABE=""; }
    LISTA=${3:-0}
}

# Ventanas seguidas con el texto troceado a lo que quepa en pantalla.
paginas() {      # paginas "texto"
    local lineas por n i=0 trozo total
    medidas "" 7
    por=$(( ALTO_MAX - 7 )); [ "$por" -lt 3 ] && por=3
    mapfile -t lineas < <(printf '%s\n' "$1" | fold -s -w $(( ANCHO - 5 )))
    n=${#lineas[@]}
    total=$(( (n + por - 1) / por ))
    while [ $i -lt "$n" ]; do
        trozo=$(printf '%s\n' "${lineas[@]:$i:$por}")
        i=$(( i + por ))
        [ $i -lt "$n" ] && trozo="$trozo
(sigue...)"
        whiptail --title "$TITULO ($(( (i + por - 1) / por ))/$total)" --ok-button "Seguir" \
            --msgbox "$trozo" "$(( $(printf '%s\n' "$trozo" | wc -l) + 7 ))" "$ANCHO" || return 1
    done
}

ancho_texto() { local c; c=$(tput cols 2>/dev/null || echo 80); echo $(( c > 80 ? 76 : c - 2 )); }

pausa_texto() { printf '\n  [Intro para seguir] '; read -r _ </dev/tty; }

ui_msg() {       # ui_msg "texto"
    case $MODO_UI in
        ventanas)
            medidas "$1" 6
            if [ -n "$CABE" ]; then
                whiptail --title "$TITULO" --ok-button "Seguir" --msgbox "$1" "$ALTO" "$ANCHO"
            else
                paginas "$1"
            fi ;;
        texto)    printf '\n%s\n' "$1" | fold -s -w "$(ancho_texto)"; pausa_texto ;;
        *)        printf '%s\n' "$1" ;;
    esac
}

ui_si() {        # ui_si "pregunta" [si|no por defecto]  -> 0 = sí
    local _udef=${2:-si}
    case $MODO_UI in
        ventanas)
            local _upreg=$1
            medidas "$_upreg" 6
            # whiptail no desplaza un sí/no: si no cabe, primero se lee entero
            # y luego se pregunta sólo el último párrafo, que es la pregunta.
            if [ -z "$CABE" ]; then
                paginas "${_upreg%$'\n\n'*}" || return 1
                _upreg=${_upreg##*$'\n\n'}
                medidas "$_upreg" 6
            fi
            if [ "$_udef" = no ]; then
                whiptail --title "$TITULO" --yes-button "Sí" --no-button "No" --defaultno --yesno "$_upreg" "$ALTO" "$ANCHO"
            else
                whiptail --title "$TITULO" --yes-button "Sí" --no-button "No" --yesno "$_upreg" "$ALTO" "$ANCHO"
            fi ;;
        texto)
            local _ur _uop="[S/n]"; [ "$_udef" = no ] && _uop="[s/N]"
            printf '\n%s\n' "$1" | fold -s -w "$(ancho_texto)"
            printf '  %s ' "$_uop"; read -r _ur </dev/tty
            _ur=${_ur:-$_udef}
            case $_ur in s|S|si|sí|Si|Sí|SI|y|Y|yes) return 0 ;; *) return 1 ;; esac ;;
        *)  [ "$_udef" = si ] ;;
    esac
}

# OJO: las variables de dentro llevan prefijo `_u` a propósito. En bash una
# `local r` aquí taparía la `r` de quien llama, y `printf -v r` escribiría en
# la de la función, no en la suya.
ui_texto() {     # ui_texto VARIABLE "pregunta" "valor por defecto"  -> 1 = cancelado
    local _uvar=$1 _upreg=$2 _udef=${3:-} _ur
    case $MODO_UI in
        ventanas)
            medidas "$_upreg" 8
            if [ -z "$CABE" ]; then paginas "$_upreg" || return 1; _upreg="${_upreg##*$'\n\n'}"; medidas "$_upreg" 8; fi
            _ur=$(whiptail --title "$TITULO" --ok-button "Seguir" --cancel-button "Salir" --inputbox "$_upreg" "$ALTO" "$ANCHO" "$_udef" 3>&1 1>&2 2>&3) || return 1 ;;
        texto)
            printf '\n%s\n' "$_upreg" | fold -s -w "$(ancho_texto)"
            printf '  [%s] > ' "$_udef"; read -r _ur </dev/tty
            _ur=${_ur:-$_udef} ;;
        *)  _ur=$_udef ;;
    esac
    printf -v "$_uvar" '%s' "$_ur"
}

ui_menu() {      # ui_menu VARIABLE "texto" etiqueta "descripción" ...
    local _uvar=$1 _upreg=$2 _ur _ui=1 _uetiq=()
    shift 2
    case $MODO_UI in
        ventanas)
            medidas "$_upreg" $(( 7 + $# / 2 )) $(( $# / 2 ))
            if [ -z "$CABE" ]; then paginas "$_upreg" || return 1; _upreg="Elige:"; medidas "$_upreg" $(( 7 + $# / 2 )) $(( $# / 2 )); fi
            _ur=$(whiptail --title "$TITULO" --ok-button "Elegir" --cancel-button "Salir" --notags --menu "$_upreg" "$ALTO" "$ANCHO" "$LISTA" "$@" 3>&1 1>&2 2>&3) || return 1 ;;
        texto)
            printf '\n%s\n\n' "$_upreg" | fold -s -w "$(ancho_texto)"
            while [ $# -gt 0 ]; do
                printf '  %d) %s\n' "$_ui" "$2"; _uetiq+=("$1"); shift 2; _ui=$((_ui + 1))
            done
            while :; do
                printf '  Elige un número [1] > '; read -r _ur </dev/tty; _ur=${_ur:-1}
                if [[ $_ur =~ ^[0-9]+$ ]] && [ "$_ur" -ge 1 ] && [ "$_ur" -le ${#_uetiq[@]} ]; then
                    _ur=${_uetiq[$((_ur - 1))]}; break
                fi
            done ;;
        *)  _ur=$1 ;;
    esac
    printf -v "$_uvar" '%s' "$_ur"
}

ui_marcar() {    # ui_marcar VARIABLE "texto" etiqueta "descripción" ON|OFF ...
    local _uvar=$1 _upreg=$2 _ur="" _udef
    shift 2
    case $MODO_UI in
        ventanas)
            medidas "$_upreg" $(( 9 + $# / 3 )) $(( $# / 3 ))
            if [ -z "$CABE" ]; then paginas "$_upreg" || return 1; _upreg="Marca lo que quieras:"; medidas "$_upreg" $(( 9 + $# / 3 )) $(( $# / 3 )); fi
            _ur=$(whiptail --title "$TITULO" --ok-button "Seguir" --cancel-button "Salir" --notags --separate-output --checklist "$_upreg

(ESPACIO marca o desmarca)" "$ALTO" "$ANCHO" "$LISTA" "$@" 3>&1 1>&2 2>&3) || return 1 ;;
        texto)
            printf '\n%s\n' "$_upreg" | fold -s -w "$(ancho_texto)"
            while [ $# -gt 0 ]; do
                _udef=si; [ "$3" = OFF ] && _udef=no
                if ui_si "  ¿$2?" "$_udef"; then _ur="$_ur $1"; fi
                shift 3
            done ;;
        *)  _ur=${!_uvar:-} ;;
    esac
    _ur=$(echo $_ur)
    printf -v "$_uvar" '%s' "$_ur"
}

di()    { printf '  %s\n' "$*"; }
bien()  { printf '  \033[32m✔\033[0m %s\n' "$*"; }
aviso() { printf '  \033[33m⚠\033[0m %s\n' "$*"; }
falla() { printf '  \033[31m✘\033[0m %s\n' "$*"; }

# Salir con Cancelar / Esc en cualquier pregunta. No se ha tocado nada todavía:
# todo se escribe al final, después del resumen.
cancelado() {
    [ "$MODO_UI" = ventanas ] && clear
    echo; di "Cancelado. No se ha cambiado nada."; echo
    exit 1
}

# ====================================================== la máquina ===========

que_maquina() {
    ARQ=$(uname -m)
    MODELO=""
    [ -r /proc/device-tree/model ] && MODELO=$(tr -d '\0' </proc/device-tree/model)
    [ -z "$MODELO" ] && [ -r /sys/class/dmi/id/product_name ] && MODELO=$(cat /sys/class/dmi/id/product_name 2>/dev/null)
    SO="Linux"
    [ -r /etc/os-release ] && SO=$(. /etc/os-release; echo "${PRETTY_NAME:-Linux}")
    RAM_MB=$(awk '/MemTotal/ {print int($2/1024)}' /proc/meminfo 2>/dev/null || echo 0)
    case $ARQ in
        x86_64)          ARQ_TXT="PC / servidor de 64 bits (amd64)" ;;
        i?86)            ARQ_TXT="PC de 32 bits (i386)" ;;
        aarch64|arm64)   ARQ_TXT="ARM de 64 bits (Raspberry Pi 3/4/5, Zero 2 W...)" ;;
        armv7l|armv8l)   ARQ_TXT="ARM de 32 bits (Raspberry Pi 2/3/4 con sistema de 32 bits)" ;;
        armv6l)          ARQ_TXT="ARMv6 (Raspberry Pi 1 o Zero / Zero W)" ;;
        riscv64)         ARQ_TXT="RISC-V de 64 bits" ;;
        *)               ARQ_TXT="$ARQ" ;;
    esac
    NUBE=""
    if grep -qi oracle /sys/class/dmi/id/chassis_asset_tag 2>/dev/null; then NUBE="Oracle Cloud"
    elif grep -qi amazon /sys/class/dmi/id/sys_vendor /sys/class/dmi/id/bios_vendor 2>/dev/null; then NUBE="Amazon (AWS)"
    elif grep -qi google /sys/class/dmi/id/product_name 2>/dev/null; then NUBE="Google Cloud"
    elif grep -qi 'microsoft' /sys/class/dmi/id/sys_vendor 2>/dev/null; then NUBE="Azure"
    elif grep -qi 'hetzner' /sys/class/dmi/id/sys_vendor 2>/dev/null; then NUBE="Hetzner"
    fi
}

gestor_paquetes() {
    if command -v apt-get >/dev/null; then echo apt
    elif command -v dnf >/dev/null; then echo dnf
    elif command -v yum >/dev/null; then echo yum
    elif command -v pacman >/dev/null; then echo pacman
    elif command -v zypper >/dev/null; then echo zypper
    elif command -v apk >/dev/null; then echo apk
    else echo ninguno; fi
}

instala_paquete() {   # instala_paquete <nombre-apt> [<nombre-otros>]
    local p=$1 otro=${2:-$1}
    case $(gestor_paquetes) in
        apt)    DEBIAN_FRONTEND=noninteractive apt-get install -y -q "$p" >/dev/null 2>&1 \
                    || { apt-get update -q >/dev/null 2>&1; DEBIAN_FRONTEND=noninteractive apt-get install -y -q "$p" >/dev/null 2>&1; } ;;
        dnf)    dnf install -y -q "$otro" >/dev/null 2>&1 ;;
        yum)    yum install -y -q "$otro" >/dev/null 2>&1 ;;
        pacman) pacman -S --noconfirm --needed "$otro" >/dev/null 2>&1 ;;
        zypper) zypper -q -n install "$otro" >/dev/null 2>&1 ;;
        apk)    apk add -q "$otro" >/dev/null 2>&1 ;;
        *)      return 1 ;;
    esac
}

version_python_ok() {
    command -v python3 >/dev/null || return 1
    python3 -c 'import sys; sys.exit(0 if sys.version_info >= (3, 7) else 1)' 2>/dev/null
}

requisitos() {
    if [ "$(id -u)" -ne 0 ] && [ -z "$PREFIJO" ]; then
        echo
        echo "  Hace falta ser administrador. Lánzalo así:"
        echo
        echo "      sudo bash $0"
        echo
        exit 1
    fi
    if [ -z "$PREFIJO" ] && [ ! -d /run/systemd/system ]; then
        echo
        echo "  Este sistema no arranca con systemd, y el instalador lo necesita"
        echo "  para dejar los servicios en marcha y que vuelvan tras un corte."
        echo "  Los programas funcionan igual a mano: mira «El servidor» en el README."
        echo
        exit 1
    fi
    # Las ventanas. Si no están, se intenta poner whiptail (un paquete
    # pequeño que ya viene en Raspberry Pi OS, Debian y Ubuntu); si no se
    # puede, se sigue en modo texto, que pregunta exactamente lo mismo.
    if [ "$MODO_UI" = ventanas ] && ! command -v whiptail >/dev/null; then
        echo "  Preparando las ventanas del asistente..."
        instala_paquete whiptail newt >/dev/null 2>&1 || true
        command -v whiptail >/dev/null || MODO_UI=texto
    fi
    if ! version_python_ok; then
        echo "  Instalando Python 3 (lo único que necesitan los programas)..."
        instala_paquete python3 python3 || true
        if ! version_python_ok; then
            echo
            echo "  No he podido instalar Python 3.7 o más nuevo. Instálalo con el"
            echo "  gestor de paquetes de tu sistema y vuelve a lanzar el asistente."
            exit 1
        fi
    fi
    command -v curl >/dev/null || command -v wget >/dev/null || instala_paquete curl curl || true
}

# ======================================================== los programas =====

descarga() {   # descarga URL fichero
    if command -v curl >/dev/null; then curl -fsSL --retry 3 -o "$2" "$1"
    else wget -q -O "$2" "$1"; fi
}

# Deja los programas en $ORIGEN. Si el instalador se lanza desde una copia del
# repositorio, se usan esos (así se prueba lo que uno tiene delante); si no, se
# baja la rama de GitHub entera y se cogen de ahí.
consigue_programas() {
    local aqui
    aqui=$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" 2>/dev/null && pwd)
    if [ -n "$aqui" ] && [ -f "$aqui/../tools/nododatos.py" ] && [ -z "${PTTLORA_DESCARGA:-}" ]; then
        ORIGEN=$(cd "$aqui/.." && pwd)/tools
        ORIGEN_INST="$aqui/instalar.sh"
        ORIGEN_TXT="copia local ($ORIGEN)"
        return 0
    fi
    TMPD=$(mktemp -d)
    trap 'rm -rf "$TMPD"' EXIT
    local url="https://codeload.github.com/$REPO/tar.gz/refs/heads/$RAMA"
    di "Descargando los programas de github.com/$REPO ($RAMA)..."
    if ! descarga "$url" "$TMPD/repo.tgz"; then
        falla "No se ha podido descargar $url"
        di "¿Hay Internet en esta máquina? Prueba otra vez en un rato."
        return 1
    fi
    tar -xzf "$TMPD/repo.tgz" -C "$TMPD" --wildcards '*/tools/*.py' '*/servidor/*' 2>/dev/null \
        || tar -xzf "$TMPD/repo.tgz" -C "$TMPD"
    ORIGEN=$(dirname "$(find "$TMPD" -path '*/tools/nododatos.py' | head -1)")
    ORIGEN_INST=$(find "$TMPD" -path '*/servidor/instalar.sh' | head -1)
    ORIGEN_TXT="github.com/$REPO ($RAMA)"
    [ -f "$ORIGEN/nododatos.py" ] || { falla "La descarga no trae los programas."; return 1; }
}

copia_programas() {
    local p
    mkdir -p "$DIR_PROG"
    for p in $PROGRAMAS; do
        [ -f "$ORIGEN/$p" ] || { falla "Falta $p en $ORIGEN_TXT"; return 1; }
        install -m 0755 "$ORIGEN/$p" "$DIR_PROG/$p"
        # Que al menos se deje leer: un fichero cortado a medias en la descarga
        # no debe llegar a un servicio que se reinicia en bucle.
        python3 -m py_compile "$DIR_PROG/$p" 2>/dev/null \
            || { falla "$p no es Python válido (¿descarga incompleta?)"; return 1; }
    done
    rm -rf "$DIR_PROG/__pycache__"
    date '+%Y-%m-%d %H:%M' >"$DIR_PROG/VERSION"
    echo "$ORIGEN_TXT" >>"$DIR_PROG/VERSION"
}

# ====================================================== los ajustes =========

# Valores por defecto, que luego pisa lo que haya guardado de otra vez.
ajustes_por_defecto() {
    INDICATIVO=""
    PIEZAS="reflector datos registro"
    P_REFLECTOR=4461
    P_DATOS=4460
    P_MANDO=4464
    P_OPERADOR=4471
    RANURAS=4
    EXIGE_IND=si
    REFLECTOR_REMOTO="$SERVIDOR_PUBLICO:4461"
    IGATE_LLAMADA=""
    IGATE_SERVIDOR="euro.aprs2.net"
    IGATE_FREC="433.500"
    IGATE_ENVIAR=no
    IGATE_NODOS=""
    ABRIR_FIREWALL=si
}

carga_ajustes() {
    ajustes_por_defecto
    # shellcheck disable=SC1090
    [ -f "$AJUSTES" ] && . "$AJUSTES"
    # En desatendido, lo que venga en el entorno manda sobre lo guardado.
    if [ "$MODO_UI" = desatendido ]; then
        local v
        for v in INDICATIVO PIEZAS P_REFLECTOR P_DATOS P_MANDO P_OPERADOR RANURAS \
                 EXIGE_IND REFLECTOR_REMOTO IGATE_LLAMADA IGATE_SERVIDOR IGATE_FREC \
                 IGATE_ENVIAR IGATE_NODOS ABRIR_FIREWALL SECRETO_NUEVO; do
            eval "[ -n \"\${_ENV_$v+x}\" ]" && eval "$v=\$_ENV_$v"
        done
    fi
}

guarda_ajustes() {
    mkdir -p "$DIR_CONF"
    {
        echo "# Lo que se contestó en el asistente de PTT LoRa. Lo lee"
        echo "# \`sudo pttlora-servidor\` para proponerlo la próxima vez."
        echo "# El secreto del relevo NO está aquí: está en secreto (sólo root)."
        local v
        for v in INDICATIVO PIEZAS P_REFLECTOR P_DATOS P_MANDO P_OPERADOR RANURAS \
                 EXIGE_IND REFLECTOR_REMOTO IGATE_LLAMADA IGATE_SERVIDOR IGATE_FREC \
                 IGATE_ENVIAR IGATE_NODOS ABRIR_FIREWALL; do
            printf '%s=%q\n' "$v" "${!v}"
        done
    } >"$AJUSTES"
    chmod 0644 "$AJUSTES"
}

tiene() { case " $PIEZAS " in *" $1 "*) return 0 ;; esac; return 1; }

# Mismo criterio que el nodo de datos: UIT, con sufijos.
indicativo_valido() {
    [[ $1 =~ ^([A-Z0-9]{1,3}/)?[A-Z0-9]{1,3}[0-9][A-Z0-9]{0,4}[A-Z](-([1-9]|[1-9][0-9]))?(/[A-Z0-9]{1,4})?$ ]]
}
puerto_valido() { [[ $1 =~ ^[0-9]+$ ]] && [ "$1" -ge 1 ] && [ "$1" -le 65535 ]; }
hostpuerto_valido() { local re='^[][A-Za-z0-9._:-]+:[0-9]+$'; [[ $1 =~ $re ]] && puerto_valido "${1##*:}"; }

# ====================================================== el asistente ========

paso_bienvenida() {
    ui_msg "Este asistente monta un servidor de PTT LoRa en esta máquina.

Para hablar por radio NO hace falta servidor, y ya hay uno público ($SERVIDOR_PUBLICO). El tuyo sirve para tener TU PROPIA RED.

No se cambia nada hasta el final, después de un resumen.

Flechas y Tab para moverte, Intro para aceptar." || cancelado
    ui_msg "Esta máquina:

· $ARQ_TXT${MODELO:+
· $MODELO}
· $SO
· ${RAM_MB} MB de memoria${NUBE:+
· en la nube: $NUBE}

Vale cualquiera: el servidor entero ocupa unos 60 MB de memoria." || cancelado
}

paso_indicativo() {
    local r
    while :; do
        ui_texto r "Tu INDICATIVO de radioaficionado (el titular de este servidor).

Ejemplo: EA1ABC" "$INDICATIVO" || cancelado
        r=$(echo "$r" | tr '[:lower:]' '[:upper:]' | tr -d ' ')
        # El del titular va sin SSID: los SSID se reparten luego por aparato.
        if indicativo_valido "$r"; then INDICATIVO=${r%%-*}; break; fi
        [ "$MODO_UI" = desatendido ] && { echo "INDICATIVO no válido: $r"; exit 2; }
        ui_msg "«$r» no parece un indicativo. Escríbelo como EA1ABC (letras y números, con un número dentro)."
    done
}

paso_piezas() {
    local on
    onoff() { tiene "$1" && echo ON || echo OFF; }
    while :; do
        ui_marcar PIEZAS "¿Qué montas?

· Reflector: une zonas lejanas
· Datos: hablar desde el móvil sin placa
· Censo: quién oye a quién
· Igate: posiciones en aprs.fi
· Relevo: administrar celdas remotas" \
            reflector "Reflector"       "$(onoff reflector)" \
            datos     "Nodo de datos"   "$(onoff datos)" \
            registro  "Censo de la red" "$(onoff registro)" \
            igate     "Igate APRS"      "$(onoff igate)" \
            mando     "Relevo de mando" "$(onoff mando)" \
            || cancelado
        [ -n "$PIEZAS" ] && break
        [ "$MODO_UI" = desatendido ] && { echo "PIEZAS vacío"; exit 2; }
        ui_msg "No has marcado nada. Marca al menos una pieza con la barra ESPACIO."
    done
}

# Las piezas que cuelgan de un reflector (datos, censo, igate) necesitan saber
# de cuál. Si se monta aquí, es éste y no se pregunta.
paso_reflector_remoto() {
    tiene reflector && return 0
    tiene datos || tiene registro || tiene igate || return 0
    local r
    while :; do
        ui_texto r "No vas a montar reflector aquí, así que el resto se colgará de uno que ya exista.

¿Cuál? (servidor:puerto)

El público es $SERVIDOR_PUBLICO:4461" "$REFLECTOR_REMOTO" || cancelado
        if hostpuerto_valido "$r"; then REFLECTOR_REMOTO=$r; break; fi
        [ "$MODO_UI" = desatendido ] && { echo "REFLECTOR_REMOTO no válido: $r"; exit 2; }
        ui_msg "Escríbelo como servidor:puerto, por ejemplo $SERVIDOR_PUBLICO:4461"
    done
}

pide_puerto() {   # pide_puerto VARIABLE "qué"
    local var=$1 r
    while :; do
        ui_texto r "Puerto para $2:" "${!var}" || cancelado
        if puerto_valido "$r"; then printf -v "$var" '%s' "$r"; return 0; fi
        ui_msg "Un puerto es un número del 1 al 65535."
    done
}

# ¿Lo tiene cogido otro programa? Lo nuestro de una instalación anterior no cuenta.
puerto_ocupado() {
    command -v ss >/dev/null || return 1
    local pid
    for pid in $(ss -ltnpH "sport = :$1" 2>/dev/null | grep -o 'pid=[0-9]*' | cut -d= -f2 | sort -u); do
        tr '\0' ' ' <"/proc/$pid/cmdline" 2>/dev/null | grep -q 'nodovirtual\|nododatos\|mandovirtual' || return 0
    done
    return 1
}

paso_puertos() {
    local hay="" p ocupados=""
    for p in $(puertos_publicos); do puerto_ocupado "$p" && ocupados="$ocupados $p"; done
    tiene reflector && hay="$hay
  · reflector       $P_REFLECTOR"
    tiene datos     && hay="$hay
  · nodo de datos   $P_DATOS"
    tiene mando     && hay="$hay
  · relevo de mando $P_MANDO"
    [ -z "$hay" ] && return 0
    ui_si "Puertos que se usarán:
$hay

Son los que la app y el firmware traen de fábrica. Salvo que alguno esté ocupado por otra cosa, déjalos así.

${ocupados:+
⚠️ OJO: ya usa$( [ "$(echo $ocupados | wc -w)" -gt 1 ] && echo "n los puertos" || echo " el puerto")$ocupados OTRO programa en esta máquina. Contesta No y elige otro.
}
¿Usar estos puertos?" "$([ -n "$ocupados" ] && echo no || echo si)" && return 0
    tiene reflector && pide_puerto P_REFLECTOR "el reflector"
    tiene datos     && pide_puerto P_DATOS "el nodo de datos"
    tiene mando     && pide_puerto P_MANDO "el relevo de mando"
}

paso_datos() {
    tiene datos || return 0
    if ui_si "NODO DE DATOS: lo que habla un móvil por aquí SALE POR LA ANTENA de las celdas.

Recomendado: exigir un indicativo válido para TRANSMITIR (para escuchar no se pide nada). No es una contraseña, pero cada emisión tiene un titular.

¿Exigir indicativo para transmitir?" "$EXIGE_IND"; then
        EXIGE_IND=si
    else
        EXIGE_IND=no
    fi
}

paso_mando() {
    tiene mando || return 0
    SECRETO_NUEVO=${_ENV_SECRETO_NUEVO:-}
    [ -n "$SECRETO_NUEVO" ] && return 0
    local fich="$DIR_CONF/secreto" r
    if [ -s "$fich" ] && ui_si "RELEVO DE MANDO: ya hay un secreto guardado de otra vez.

Si lo cambias, tendrás que volver a flashear TODAS las celdas que usen este relevo.

¿Conservar el secreto que hay?" si; then
        return 0
    fi
    ui_menu r "RELEVO DE MANDO: por este puerto se manda en una celda ENTERA, así que va protegido con un secreto que comparten el servidor y el firmware de tus placas.

Si empiezas, crea uno nuevo.

¿Qué hacemos?" \
        nuevo "Crear uno nuevo" \
        pegar "Pegar el de mi secreto.h" || cancelado
    if [ "$r" = pegar ]; then
        while :; do
            ui_texto r "Pega el texto que va entre comillas en MANDO_SECRETO de tu firmware/src/secreto.h:" "" || cancelado
            r=$(echo "$r" | tr -d ' "')
            [ ${#r} -ge 16 ] && { SECRETO_NUEVO=$r; break; }
            ui_msg "Demasiado corto: un secreto debería tener al menos 16 caracteres."
        done
    else
        SECRETO_NUEVO=$(python3 -c 'import secrets; print(secrets.token_hex(16))')
    fi
}

paso_igate() {
    tiene igate || return 0
    local r
    local fich="$DIR_CONF/igate.conf"
    if [ -f "$fich" ] && ! grep -q '^# asistente' "$fich" && ui_si "IGATE: hay un igate.conf escrito a mano.

¿Lo dejo tal cual? (Si dices que no, se reescribe con tus respuestas y el viejo se guarda como igate.conf.anterior.)" si; then
        IGATE_A_MANO=1; return 0
    fi
    IGATE_A_MANO=""
    [ -z "$IGATE_LLAMADA" ] && IGATE_LLAMADA="$INDICATIVO-10"
    while :; do
        ui_texto r "IGATE APRS: con qué indicativo entra la pasarela en APRS-IS.

Usa tu indicativo con un SSID que no uses para otra cosa (por ejemplo -10). La contraseña de APRS-IS se calcula sola." "$IGATE_LLAMADA" || cancelado
        r=$(echo "$r" | tr '[:lower:]' '[:upper:]' | tr -d ' ')
        indicativo_valido "$r" && { IGATE_LLAMADA=$r; break; }
        ui_msg "«$r» no parece un indicativo con SSID. Ejemplo: $INDICATIVO-10"
    done
    while :; do
        ui_texto r "Frecuencia de tu red, en MHz (sale en aprs.fi para que te encuentren).

Ejemplo: 433.500" "$IGATE_FREC" || cancelado
        r=${r//,/.}
        [[ $r =~ ^[0-9]{2,4}\.[0-9]{1,4}$ ]] && { IGATE_FREC=$(printf '%.3f' "$r"); break; }
        ui_msg "Escríbela en MHz con punto decimal, por ejemplo 433.500"
    done
    paso_igate_estaciones
    if ui_si "¿Publicar YA en APRS-IS?

⚠️ Lo que se publica en APRS no se puede borrar: aprs.fi guarda el histórico para siempre.

Recomendado la primera vez: NO. El igate sólo MIRA y escribe en el registro lo que publicaría. Cuando lo veas bien, vuelve a lanzar «sudo pttlora-servidor» y cámbialo." "$IGATE_ENVIAR"; then
        IGATE_ENVIAR=si
    else
        IGATE_ENVIAR=no
    fi
}

# Lista blanca: sólo sale en el mapa lo que se apunta aquí.
paso_igate_estaciones() {
    [ "$MODO_UI" = desatendido ] && return 0
    local r nombre ssid
    while :; do
        local lista
        lista=$(echo "$IGATE_NODOS" | tr ';' '\n' | sed -n 's/^\(.*\)=\(.*\)$/  · \1  →  \2/p')
        [ -z "$lista" ] && lista="  (ninguna todavía)"
        ui_menu r "IGATE: sólo se publican las estaciones de esta lista (es a propósito: nadie acaba en el mapa por descuido).

$lista" \
            anadir "Añadir una estación" \
            vaciar "Vaciar la lista" \
            listo  "Listo, seguir" || cancelado
        case $r in
            listo)  break ;;
            vaciar) IGATE_NODOS="" ;;
            anadir)
                ui_texto nombre "Nombre de la placa (el que le pusiste, p. ej. celdaCERRO) o un indicativo (para publicar a la PERSONA cuando habla):" "" || continue
                nombre=$(echo "$nombre" | tr -d ' =;')
                [ -z "$nombre" ] && continue
                ui_texto ssid "¿Con qué indicativo y SSID sale «$nombre» en APRS?

Ejemplos: $INDICATIVO-11 para una celda, $INDICATIVO-9 para una persona." "$INDICATIVO-11" || continue
                ssid=$(echo "$ssid" | tr '[:lower:]' '[:upper:]' | tr -d ' ')
                if ! indicativo_valido "$ssid"; then ui_msg "«$ssid» no es un indicativo válido."; continue; fi
                IGATE_NODOS="${IGATE_NODOS:+$IGATE_NODOS;}$nombre=$ssid" ;;
        esac
    done
}

paso_firewall() {
    FW=""
    if command -v ufw >/dev/null && ufw status 2>/dev/null | grep -q 'Status: active'; then FW=ufw
    elif command -v firewall-cmd >/dev/null && firewall-cmd --state >/dev/null 2>&1; then FW=firewalld
    fi
    [ -z "$FW" ] && return 0
    local p; p=$(puertos_publicos | tr '\n' ' ')
    [ -z "$p" ] && return 0
    if ui_si "Esta máquina tiene cortafuegos ($FW) activo.

¿Abro los puertos TCP de PTT LoRa? ($p)" "$ABRIR_FIREWALL"; then
        ABRIR_FIREWALL=si
    else
        ABRIR_FIREWALL=no
    fi
}

puertos_publicos() {
    tiene reflector && echo "$P_REFLECTOR"
    tiene datos     && echo "$P_DATOS"
    tiene mando     && echo "$P_MANDO"
}

texto_resumen() {
    local t="Indicativo: $INDICATIVO

Se va a montar:"
    tiene reflector && t="$t
  · Reflector, puerto $P_REFLECTOR"
    tiene datos     && t="$t
  · Nodo de datos, puerto $P_DATOS"
    tiene registro  && t="$t
  · Censo de la red"
    tiene igate     && t="$t
  · Igate APRS: ${IGATE_LLAMADA:-?}$([ "$IGATE_ENVIAR" = si ] && echo ", PUBLICANDO" || echo ", sólo mirar")"
    tiene mando     && t="$t
  · Relevo de mando, puerto $P_MANDO"
    tiene reflector || t="$t

Colgado del reflector: $REFLECTOR_REMOTO"
    printf '%s' "$t"
}

# ====================================================== la instalación ======

crea_usuario() {
    [ -n "$PREFIJO" ] && return 0
    id "$USUARIO" >/dev/null 2>&1 && return 0
    local nl=/usr/sbin/nologin; [ -x $nl ] || nl=/sbin/nologin; [ -x $nl ] || nl=/bin/false
    useradd --system --home-dir "$DIR_DATOS" --no-create-home --shell "$nl" "$USUARIO" 2>/dev/null \
        || adduser -S -H -h "$DIR_DATOS" -s "$nl" "$USUARIO" 2>/dev/null
}

# Una unidad por pieza. El endurecimiento es el que cualquier servicio de red
# sin privilegios debería llevar: sin root, sin poder escribir en el sistema.
escribe_unidad() {   # escribe_unidad nombre "descripción" "orden" [después-de]
    local n=$1 desc=$2 orden=$3 tras=${4:-}
    mkdir -p "$DIR_UNID"
    cat >"$DIR_UNID/pttlora-$n.service" <<EOF
# Escrito por el asistente de PTT LoRa (sudo pttlora-servidor). Si lo editas a
# mano, la próxima vez que pases el asistente se sobrescribe.
[Unit]
Description=PTT LoRa - $desc
After=network-online.target${tras:+ pttlora-$tras.service}
Wants=network-online.target${tras:+ pttlora-$tras.service}

[Service]
Type=simple
User=$USUARIO
Group=$USUARIO
WorkingDirectory=$DIR_PROG
ExecStart=$orden
Restart=always
RestartSec=5
Environment=PYTHONUNBUFFERED=1 PYTHONDONTWRITEBYTECODE=1
StateDirectory=pttlora
NoNewPrivileges=yes
PrivateTmp=yes
ProtectSystem=strict
ProtectHome=yes
ProtectKernelTunables=yes
ProtectControlGroups=yes
RestrictAddressFamilies=AF_INET AF_INET6 AF_UNIX

[Install]
WantedBy=multi-user.target
EOF
}

escribe_igate_conf() {
    local fich="$DIR_CONF/igate.conf" host puerto
    if tiene reflector; then host=127.0.0.1; puerto=$P_REFLECTOR
    else host=${REFLECTOR_REMOTO%:*}; puerto=${REFLECTOR_REMOTO##*:}; fi
    [ -f "$fich" ] && ! grep -q '^# asistente' "$fich" && cp -p "$fich" "$fich.anterior"
    {
        echo "# asistente: escrito por «sudo pttlora-servidor». Si lo editas a mano, quita"
        echo "# esta línea y el asistente te preguntará antes de tocarlo."
        echo "# Todas las opciones explicadas: tools/igate.conf.ejemplo en el repositorio."
        echo
        echo "[reflector]"
        echo "host = $host"
        echo "puerto = $puerto"
        echo
        echo "[aprs]"
        echo "llamada = $IGATE_LLAMADA"
        echo "passcode = auto"
        echo "servidor = $IGATE_SERVIDOR"
        echo "puerto = 14580"
        echo "comentario = ${IGATE_FREC}MHz Voice over LoRa"
        echo "extras = nombre"
        echo "estado = PTT LoRa: by {indicativo} github.com/pyopower/pttLoRa"
        echo "estado_cada = 1800"
        echo "# no = sólo mirar (escribe en el registro lo que publicaría)"
        echo "enviar = $IGATE_ENVIAR"
        echo
        echo "[nodos]"
        echo "$IGATE_NODOS" | tr ';' '\n' | sed -n 's/^\(.*\)=\(.*\)$/\1 = \2/p'
    } >"$fich"
}

instala() {
    local grupo_ok=1 reflector_local="127.0.0.1:$P_REFLECTOR" refl
    PY=$(command -v python3)
    tiene reflector && refl=$reflector_local || refl=$REFLECTOR_REMOTO

    [ "$MODO_UI" = ventanas ] && clear
    echo
    di "Instalando PTT LoRa..."
    echo

    crea_usuario && bien "usuario de sistema «$USUARIO»" || { falla "no se ha podido crear el usuario $USUARIO"; exit 1; }
    copia_programas && bien "programas en $DIR_PROG" || exit 1
    mkdir -p "$DIR_CONF" "$DIR_DATOS"
    [ -z "$PREFIJO" ] && chown "$USUARIO:$USUARIO" "$DIR_DATOS"

    # Para no dejar servicios viejos colgando si se desmarca una pieza.
    local n
    for n in $TODAS; do
        if ! tiene "$n" && [ -f "$DIR_UNID/pttlora-$n.service" ]; then
            [ -z "$PREFIJO" ] && systemctl disable --now "pttlora-$n.service" >/dev/null 2>&1
            rm -f "$DIR_UNID/pttlora-$n.service"
            bien "quitado: $n (lo has desmarcado)"
        fi
    done

    if tiene reflector; then
        escribe_unidad reflector "reflector (punto de reunión de las celdas)" \
            "$PY $DIR_PROG/nodovirtual.py --escucha $P_REFLECTOR"
    fi
    if tiene datos; then
        local ex=""; [ "$EXIGE_IND" = si ] && ex=" --exige-indicativo"
        escribe_unidad datos "nodo de datos para las apps" \
            "$PY $DIR_PROG/nododatos.py --escucha $P_DATOS --reflector $refl$ex" \
            "$(tiene reflector && echo reflector)"
    fi
    if tiene registro; then
        escribe_unidad registro "censo de la red" \
            "$PY $DIR_PROG/registro.py --reflector $refl --fichero $DIR_DATOS/registro.json --cada 30" \
            "$(tiene reflector && echo reflector)"
    fi
    if tiene igate; then
        [ -z "${IGATE_A_MANO:-}" ] && escribe_igate_conf
        chmod 0640 "$DIR_CONF/igate.conf"
        [ -z "$PREFIJO" ] && chgrp "$USUARIO" "$DIR_CONF/igate.conf"
        escribe_unidad igate "pasarela de posiciones a APRS-IS" \
            "$PY $DIR_PROG/igate.py --conf $DIR_CONF/igate.conf" \
            "$(tiene reflector && echo reflector)"
    fi
    if tiene mando; then
        if [ -n "${SECRETO_NUEVO:-}" ]; then
            ( umask 077; printf '%s\n' "$SECRETO_NUEVO" >"$DIR_CONF/secreto" )
        fi
        chmod 0640 "$DIR_CONF/secreto"
        [ -z "$PREFIJO" ] && chgrp "$USUARIO" "$DIR_CONF/secreto"
        escribe_unidad mando "relevo de mando saliente" \
            "$PY $DIR_PROG/mandovirtual.py --nodos $P_MANDO --operador $P_OPERADOR --ranuras $RANURAS --secreto $DIR_CONF/secreto --exige"
    fi
    bien "origen: $ORIGEN_TXT"
    bien "servicios escritos: $(for n in $TODAS; do tiene "$n" && printf 'pttlora-%s ' "$n"; done)"

    guarda_ajustes
    # La orden para volver: el propio instalador, que ya sabe leer lo guardado.
    if [ -n "${ORIGEN_INST:-}" ] && [ -f "$ORIGEN_INST" ]; then
        install -D -m 0755 "$ORIGEN_INST" "$ORDEN" && bien "orden «sudo pttlora-servidor» para volver aquí"
    fi

    if [ "$ABRIR_FIREWALL" = si ] && [ -z "$PREFIJO" ]; then
        local p
        for p in $(puertos_publicos); do
            case ${FW:-} in
                ufw)       ufw allow "$p/tcp" comment 'PTT LoRa' >/dev/null 2>&1 ;;
                firewalld) firewall-cmd -q --permanent --add-port="$p/tcp" && firewall-cmd -q --reload ;;
            esac
        done
        [ -n "${FW:-}" ] && bien "cortafuegos ($FW): abiertos $(puertos_publicos | tr '\n' ' ')"
    fi

    [ -n "$PREFIJO" ] && { bien "(prueba en $PREFIJO: no se arranca nada)"; return 0; }

    systemctl daemon-reload
    for n in $TODAS; do
        tiene "$n" || continue
        systemctl enable "pttlora-$n.service" >/dev/null 2>&1
        systemctl restart "pttlora-$n.service"
    done
    sleep 4
    echo
    estado_breve || grupo_ok=0
    echo
    return $(( grupo_ok ? 0 : 1 ))
}

estado_breve() {
    local n ok=0 p
    for n in $TODAS; do
        tiene "$n" || continue
        if systemctl is-active --quiet "pttlora-$n.service"; then
            bien "pttlora-$n en marcha"
        else
            falla "pttlora-$n NO arranca. Mira: journalctl -u pttlora-$n -n 30"
            ok=1
        fi
    done
    if command -v ss >/dev/null; then
        for p in $(puertos_publicos); do
            ss -ltn 2>/dev/null | grep -q ":$p\b" && bien "escuchando en el puerto $p" \
                || { aviso "nadie escucha en el puerto $p todavía"; ok=1; }
        done
    fi
    return $ok
}

ips_locales() {
    hostname -I 2>/dev/null | tr ' ' '\n' | grep -v '^$' | grep -v ':' | head -3 | tr '\n' ' '
}

final() {
    local ip t privada=""
    ip=$(ips_locales)
    for t in $ip; do
        case $t in 10.*|192.168.*|172.1[6-9].*|172.2[0-9].*|172.3[01].*) privada=1 ;; esac
    done
    local nombre=${ip%% *}; [ -z "$nombre" ] && nombre="IP-de-esta-máquina"

    t="¡Hecho! Tu servidor PTT LoRa está en marcha y arrancará solo cada vez que se encienda la máquina.

IP de esta máquina: ${ip:-?}"
    if tiene reflector || tiene datos; then
        t="$t

EN LA APP (Ajustes):"
        tiene reflector && t="$t
  · Enlace con otros nodos → $nombre  $P_REFLECTOR
    (va en la CELDA, que es la que lo pasa a la antena)"
        tiene datos && t="$t
  · Camino de datos → $nombre  $P_DATOS"
    fi
    if tiene mando; then
        t="$t

RELEVO DE MANDO — en tu firmware, fichero firmware/src/secreto.h:
  #define MANDO_SECRETO \"$(cat "$DIR_CONF/secreto" 2>/dev/null)\"
(compila y flashea las celdas con él; no lo publiques)
Para administrar una celda: ssh -L $P_OPERADOR:127.0.0.1:$P_OPERADOR esta-máquina"
    fi
    tiene igate && [ "$IGATE_ENVIAR" = no ] && t="$t

IGATE en modo «sólo mirar». Mira lo que publicaría con:
  journalctl -u pttlora-igate -f"
    if [ -n "$privada" ] && { tiene reflector || tiene datos || tiene mando; }; then
        t="$t

⚠️ Esta máquina está detrás de un router (IP privada). Para que te lleguen desde Internet, abre en el router y redirige a ${nombre} los puertos TCP: $(puertos_publicos | tr '\n' ' ')
Y usa en la app tu IP pública o un nombre DNS dinámico."
    fi
    [ -n "$NUBE" ] && t="$t

⚠️ Estás en $NUBE: además del cortafuegos de la máquina, abre los puertos TCP $(puertos_publicos | tr '\n' ' ') en el panel de la nube (security list / grupo de seguridad)."
    t="$t

Volver a este asistente: sudo pttlora-servidor
Ver qué pasa: journalctl -u 'pttlora-*' -f"

    printf '%s\n' "$t" >"$DIR_CONF/LEEME.txt"
    chmod 0600 "$DIR_CONF/LEEME.txt"
    ui_msg "$t

(Esto queda guardado en $DIR_CONF/LEEME.txt)"
    [ "$MODO_UI" = ventanas ] && { clear; printf '%s\n\n' "$t" | fold -s -w "$(ancho_texto)"; }
    return 0
}

# ====================================================== otras órdenes =======

estado() {
    carga_ajustes
    [ -f "$AJUSTES" ] || { echo "  PTT LoRa no está instalado en esta máquina."; return 1; }
    echo
    di "PTT LoRa — $(head -1 "$DIR_PROG/VERSION" 2>/dev/null) ($(sed -n 2p "$DIR_PROG/VERSION" 2>/dev/null))"
    echo
    estado_breve
    echo
    di "Últimas líneas:"
    journalctl -u 'pttlora-*' -n 12 --no-pager -o cat 2>/dev/null | sed 's/^/    /'
    echo
}

actualizar() {
    carga_ajustes
    [ -f "$AJUSTES" ] || { echo "  PTT LoRa no está instalado: lanza el asistente sin opciones."; return 1; }
    PTTLORA_DESCARGA=1 consigue_programas || return 1
    copia_programas || return 1
    [ -n "${ORIGEN_INST:-}" ] && install -D -m 0755 "$ORIGEN_INST" "$ORDEN"
    bien "programas actualizados"
    local n
    for n in $TODAS; do tiene "$n" && systemctl restart "pttlora-$n.service"; done
    sleep 3
    estado_breve
}

desinstalar() {
    carga_ajustes
    ui_si "¿Quitar PTT LoRa de esta máquina?

Se paran y borran los servicios y los programas. Los ajustes ($DIR_CONF), el secreto y el censo se BORRAN también." no || return 0
    local n
    for n in $TODAS; do
        systemctl disable --now "pttlora-$n.service" >/dev/null 2>&1
        rm -f "$DIR_UNID/pttlora-$n.service"
    done
    systemctl daemon-reload 2>/dev/null
    if [ "${ABRIR_FIREWALL:-no}" = si ] && command -v ufw >/dev/null; then
        for n in $(puertos_publicos); do ufw delete allow "$n/tcp" >/dev/null 2>&1; done
    fi
    rm -rf "$DIR_PROG" "$DIR_CONF" "$DIR_DATOS" "$ORDEN"
    userdel "$USUARIO" >/dev/null 2>&1
    [ "$MODO_UI" = ventanas ] && clear
    echo; bien "PTT LoRa desinstalado."; echo
}

asistente() {
    que_maquina
    carga_ajustes
    consigue_programas || exit 1

    if [ -f "$AJUSTES" ] && [ "$MODO_UI" != desatendido ]; then
        local q
        ui_menu q "PTT LoRa ya está instalado en esta máquina. ¿Qué quieres hacer?" \
            cambiar     "Cambiar ajustes" \
            actualizar  "Actualizar programas" \
            estado      "Ver cómo está" \
            desinstalar "Desinstalar" \
            salir       "Salir" || cancelado
        case $q in
            actualizar)  [ "$MODO_UI" = ventanas ] && clear; actualizar; exit $? ;;
            estado)      [ "$MODO_UI" = ventanas ] && clear; estado; exit $? ;;
            desinstalar) desinstalar; exit 0 ;;
            salir)       [ "$MODO_UI" = ventanas ] && clear; exit 0 ;;
        esac
    else
        paso_bienvenida
    fi

    paso_indicativo
    paso_piezas
    paso_reflector_remoto
    paso_puertos
    paso_datos
    paso_mando
    paso_igate
    [ -z "$PREFIJO" ] && paso_firewall

    ui_si "RESUMEN — revisa antes de instalar:

$(texto_resumen)

¿Instalar ahora?" si || cancelado

    if instala; then
        final
    else
        echo
        aviso "Algo no ha arrancado. Los detalles de arriba y «sudo pttlora-servidor»"
        aviso "para el estado. Los ajustes quedan guardados: puedes volver a lanzarlo."
        exit 1
    fi
}

# ============================================================== arranque ====
# Todo va dentro de una función que se llama en la ÚLTIMA línea, y no es
# estética: con «curl ... | sudo bash» bash va leyendo el script del tubo según
# lo ejecuta. Si algo leyera la entrada antes de tiempo (un apt-get, un
# `exec </dev/tty`) se comería el resto del script o, peor, bash lo leería
# del teclado. Así el fichero entero está leído antes de hacer nada.

principal() {
    local a _v

    # Lo que venga en el entorno para el modo desatendido se aparta antes de cargar
    # los ajustes guardados, para que mande sobre ellos.
    for _v in INDICATIVO PIEZAS P_REFLECTOR P_DATOS P_MANDO P_OPERADOR RANURAS EXIGE_IND \
              REFLECTOR_REMOTO IGATE_LLAMADA IGATE_SERVIDOR IGATE_FREC IGATE_ENVIAR \
              IGATE_NODOS ABRIR_FIREWALL SECRETO_NUEVO; do
        [ -n "${!_v+x}" ] && eval "_ENV_$_v=\${$_v}"
    done

    ACCION=asistente
    for a in "$@"; do
        case $a in
            --actualizar)  ACCION=actualizar ;;
            --estado)      ACCION=estado ;;
            --desinstalar) ACCION=desinstalar ;;
            --texto)       [ "$MODO_UI" = ventanas ] && MODO_UI=texto ;;
            -h|--ayuda|--help) sed -n '2,26p' "${BASH_SOURCE[0]}" 2>/dev/null | sed 's/^# \{0,1\}//'; exit 0 ;;
            *) echo "Opción desconocida: $a (prueba --ayuda)"; exit 2 ;;
        esac
    done

    # Lanzado con «curl ... | sudo bash» la entrada es el propio script, no el
    # teclado: las preguntas se leen de la terminal.
    # (--estado y --actualizar no preguntan nada: valen desde cron o un script.)
    if [ "$MODO_UI" != desatendido ] && [ ! -t 0 ] && { [ $ACCION = asistente ] || [ $ACCION = desinstalar ]; }; then
        if ( : </dev/tty ) 2>/dev/null; then exec </dev/tty
        else echo "  Sin terminal para preguntar. Descárgalo y lánzalo con: sudo bash instalar.sh"; exit 1; fi
    fi

    requisitos
    case $ACCION in
        asistente)   asistente ;;
        actualizar)  actualizar ;;
        estado)      estado ;;
        desinstalar) desinstalar ;;
    esac
}

principal "$@"; exit $?
