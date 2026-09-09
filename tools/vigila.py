#!/usr/bin/env python3
# vigila.py — escucha por el CABLE lo que dice un nodo, sin molestarle.
#
#   ./vigila.py /dev/ttyACM1
#
# Sirve sobre todo para una cosa: **leer el código de acceso de 6 cifras** que
# el nodo enseña cuando alguien abre una sesión por Bluetooth o por WiFi. El
# nodo lo saca por su pantalla y por el cable, nunca por el enlace que está
# autorizando (sería dar la llave por debajo de la puerta), así que con el cable
# se puede autorizar un móvil aunque no se tenga la placa delante.
#
# NO reinicia la placa: no toca DTR/RTS. Abrir el puerto sin ese cuidado
# reiniciaría el ESP32 y se perdería justo lo que se quiere ver.

import sys
import time

import serial

FEND, FESC, TFEND, TFESC = 0xC0, 0xDB, 0xDC, 0xDD
NOMBRES = {0x81: 'INICIO', 0x82: 'VOZ', 0x83: 'FIN', 0x84: 'HOLA',
           0x85: 'ESTADO', 0x86: 'CANAL', 0x87: 'EMPAREJA', 0x88: 'OTA',
           0x89: 'PTT', 0x8A: 'RED', 0x8F: 'LOG'}


def main():
    puerto = sys.argv[1] if len(sys.argv) > 1 else '/dev/ttyACM1'
    segundos = float(sys.argv[2]) if len(sys.argv) > 2 else 600.0

    s = serial.Serial()
    s.port = puerto
    s.baudrate = 115200
    s.timeout = 0.3
    # Lo importante: NADA de DTR/RTS, o el ESP32 se reinicia al abrir.
    s.dtr = False
    s.rts = False
    s.open()

    print('vigilando %s (%.0f s). El código saldrá aquí en cuanto alguien '
          'conecte.' % (puerto, segundos))
    buf = bytearray()
    dentro = escape = False
    t0 = time.time()
    while time.time() - t0 < segundos:
        for c in s.read(512):
            if c == FEND:
                if dentro and buf:
                    tipo, cuerpo = buf[0], bytes(buf[1:])
                    nombre = NOMBRES.get(tipo, hex(tipo))
                    if tipo in (0x85, 0x87, 0x8F):
                        txt = cuerpo.decode('utf8', 'replace')
                        print('[%s] %s' % (nombre, txt), flush=True)
                        if 'codigo de acceso' in txt or 'codigo para' in txt:
                            print('>>> CÓDIGO: %s' % ''.join(
                                ch for ch in txt if ch.isdigit())[-6:], flush=True)
                    elif tipo == 0x89 and cuerpo:
                        print('[PTT] estado=%d %s' % (
                            cuerpo[0], cuerpo[1:].decode('ascii', 'replace')),
                            flush=True)
                dentro, escape = True, False
                buf = bytearray()
                continue
            if not dentro:
                continue
            if c == FESC:
                escape = True
                continue
            if escape:
                c = FEND if c == TFEND else FESC
                escape = False
            buf.append(c)
    s.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
