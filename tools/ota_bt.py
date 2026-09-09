#!/usr/bin/env python3
# ota_bt.py — actualiza el firmware de un nodo por Bluetooth (o por cable).
#
#   ./ota_bt.py firmware.bin --ble AA:BB:CC:DD:EE:FF   (nodos con fw >= 1.26)
#   ./ota_bt.py firmware.bin --bt AA:BB:CC:DD:EE:FF    (SPP, fw <= 1.25)
#   ./ota_bt.py firmware.bin --puerto /dev/ttyACM0
#   ./ota_bt.py firmware.bin --tcp 192.168.1.50        (por WiFi)
#   ./ota_bt.py firmware.bin --tcp 127.0.0.1:4471      (por el relevo de mando:
#                                     asi se actualiza un nodo que esta en una
#                                     red donde no se pueden abrir puertos)
#
# Es lo mismo que hará la app: no hace falta WiFi ni red de ninguna clase.
# Sirve además para probar el camino de actualización sin depender del móvil.

import hashlib, os, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nodo import Nodo

CMD_OTA_INI, CMD_OTA_DAT, CMD_OTA_FIN = 0x0B, 0x0C, 0x0D
CMD_ESTADO = 0x05
# Trozos pequeños a proposito: cuanto menos ocupe cada trama, menos posibilidad
# de desbordar la cola de recepcion del Bluetooth del nodo mientras esta
# ocupado borrando un sector de flash.
TROZO = 512


_n = {}


def main():
    try:
        return _main()
    finally:
        # Cerrar SIEMPRE: si no, el nodo se queda con la ranura de Bluetooth
        # ocupada por un fantasma y rechaza el siguiente intento. Por BLE es
        # peor todavia: BlueZ mantiene el enlace abierto aunque muera el
        # proceso, y el nodo deja de anunciarse mientras cree tener cliente.
        try:
            if _n.get('n'): _n['n'].s.close()
        except Exception:
            pass


def placa_del_binario(datos):
    """Que placa espera este firmware. La cadena va compilada dentro (`PLACA` en
    main.cpp) precisamente para poder mirarla desde fuera. Si aparecieran las
    dos —no deberia— se devuelve None y no se comprueba nada."""
    hay = [n.decode() for n in (b'tbeam', b'lora32') if datos.find(n) >= 0]
    return hay[0] if len(hay) == 1 else None


def placa_del_nodo(n):
    """Que placa dice ser el nodo. El estado empieza por `v1.37(tbeam) ...`."""
    import re
    n.manda(CMD_ESTADO)
    for _, p in n.lee(3.0):
        m = re.match(rb'\s*v[0-9.]+\(([a-z0-9]+)\)', p)
        if m:
            return m.group(1).decode()
    return None


def _main():
    a = sys.argv[1:]
    if not a:
        print(__doc__.strip() if __doc__ else 'ver cabecera')
        return 1
    fichero = a[0]

    def opt(n, d=None):
        return a[a.index(n) + 1] if n in a else d

    bt = opt('--bt')
    ble = opt('--ble')
    tcp = opt('--tcp')
    puerto = opt('--puerto', '/dev/ttyACM0')


    datos = open(fichero, 'rb').read()
    md5 = hashlib.md5(datos).hexdigest()
    print('-- %s: %d bytes, md5 %s' % (os.path.basename(fichero), len(datos), md5))

    # POR TCP LOS TROZOS TIENEN QUE SER MAS PEQUEÑOS: el buffer de un tubo TCP
    # en el nodo es de 460 B (KISS_MAX_TCP), no los 1100 del cable. Con 512 la
    # trama no cabe y la actualizacion no arranca — y el sintoma seria un nodo
    # mudo, no un error claro.
    global TROZO
    if tcp:
        TROZO = 400
    n = (Nodo(ble=ble) if ble else
         Nodo(tcp=tcp) if tcp else
         Nodo(bt=bt) if bt else
         Nodo(puerto))
    _n['n'] = n
    time.sleep(2.5)

    # Se drena lo que el nodo tuviera que decir antes de empezar (avisos de
    # arranque, peticion de codigo...): si pide autorizacion, hay que darsela
    # antes o ignorara la actualizacion entera.
    for t, p in n.lee(1.5):
        if t == 0x87:
            print('!! el nodo pide codigo de acceso: autoriza primero')
            return 1

    # ⚠️ NO FLASHEAR EL FIRMWARE DE UNA PLACA EN LA OTRA.
    #
    # El 9-sep casi pasa: las dos placas se administran igual, los binarios se
    # llaman parecido y basta meterlas en el mismo bucle. Y el error no avisa —
    # el patillaje, el PMU y la alimentacion de la radio no tienen nada que ver,
    # asi que la placa arranca muerta y sin puerto por el que quejarse.
    # La placa va compilada dentro del binario y el nodo la dice en su estado.
    esperada = placa_del_binario(datos)
    dice = placa_del_nodo(n)
    if esperada and dice and esperada != dice:
        print('\n⛔ ESTE FIRMWARE NO ES DE ESTA PLACA: el fichero es para «%s» '
              'y el nodo es «%s». No se manda nada.' % (esperada, dice),
              file=sys.stderr)
        return 2
    if esperada and dice:
        print('-- placa %s: coincide' % dice)
    elif not dice:
        print('-- OJO: el nodo no dice su placa (firmware anterior a la v1.37); '
              'comprueba a mano que el binario es el suyo')

    n.manda(CMD_OTA_INI,
            len(datos).to_bytes(4, 'big') + md5.encode('ascii'))
    arranco = False
    for t, p in n.lee(4):
        txt = p.decode('utf-8', 'replace')
        print('   ', txt[:70])
        if 'iniciada' in txt:
            arranco = True
    if not arranco:
        print('!! el nodo no acepto la actualizacion')
        return 1

    # Se espera el acuse de CADA trozo. Mandar a ciegas con una pausa fija no
    # funciona: escribir en flash es mucho mas lento que recibir por SPP, el
    # buffer del nodo se llena y la transferencia se atasca sin dar error.
    # Parada y espera con REENVIO. Durante el borrado de un sector de flash el
    # nodo pierde bytes del Bluetooth y la trama se corrompe: sin reenviar, un
    # solo trozo perdido mata la actualizacion entera.
    t0 = time.time()
    trozos = [datos[i:i + TROZO] for i in range(0, len(datos), TROZO)]
    idx = 0
    ultimo_pct = -1
    reenvios = 0
    # ⚠️ DOS COSAS QUE COSTARON UNA MAÑANA (8-sep-2026), Y VAN JUNTAS:
    #
    # 1. **EL PLAZO ERA CORTO.** Estaba en 3 s y **borrar un sector de flash del
    #    ESP32 tarda casi 2 s**: medido, el acuse del primer trozo llego a los
    #    1,78 s. Basta con que un borrado se pase de 3 s para que este bucle
    #    reenvie un trozo que el nodo estaba procesando tan tranquilo.
    #
    # 2. Y en cuanto reenvia UNA vez, **esto ya no se recupera nunca**, que es
    #    lo de verdad grave. El nodo acaba acusando los dos envios, asi que a
    #    partir de ahi hay un acuse de sobra en la cola: se manda el trozo N y
    #    se lee el acuse del N-1, se manda el N+1 y se lee el del N... El
    #    transmisor va siempre un paso por detras, sin avanzar y sin dar error.
    #    Sintoma exacto: "0%, 0 reenvios" durante diez minutos y luego
    #    "demasiados reenvios en el trozo 1".
    #
    # El arreglo bueno no es alargar el plazo (que tambien): es **no aceptar
    # cualquier acuse**. Solo vale el que dice que el trozo que acabamos de
    # mandar ya esta escrito, o sea `siguiente > idx`. Los atrasados se tiran.
    PLAZO_ACUSE = 10
    while idx < len(trozos):
        n.manda(CMD_OTA_DAT, idx.to_bytes(2, 'big') + trozos[idx])
        siguiente = None
        for t, p in n.lee(PLAZO_ACUSE):
            if t == 0x88 and len(p) >= 3:
                cual = (p[1] << 8) | p[2]
                if cual <= idx:
                    continue            # acuse atrasado: no cuenta
                siguiente = cual
                if p[0] != ultimo_pct:
                    ultimo_pct = p[0]
                    print('    %3d%%  (%.0f kB/s, %d reenvios)'
                          % (p[0], idx * TROZO / 1024.0 /
                             max(0.1, time.time() - t0), reenvios), flush=True)
                break
            if t == 0x8F:
                print('   ', p.decode('utf-8', 'replace')[:70], flush=True)
        if siguiente is None:
            reenvios += 1
            if reenvios > 200:
                print('!! demasiados reenvios en el trozo %d' % idx)
                return 1
            continue                      # el mismo trozo otra vez
        idx = siguiente
    print('-- %d reenvios en total' % reenvios)

    n.manda(CMD_OTA_FIN)
    print('-- enviado en %.0f s, esperando verificacion...' % (time.time() - t0))
    for t, p in n.lee(12):
        if t in (0x8F, 0x88):
            print('   ', p.decode('utf-8', 'replace')[:70])
    return 0


if __name__ == '__main__':
    sys.exit(main())
