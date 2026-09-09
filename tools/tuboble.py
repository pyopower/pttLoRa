#!/usr/bin/env python3
# tuboble.py — el tubo BLE hacia un nodo de PTT LoRa, con cara de socket.
#
# Existe para que `nodo.py` (y cualquier otra herramienta de la raspi) hable
# con el nodo por BLE **sin cambiar una linea de su logica**: el protocolo KISS
# es el mismo por USB, por WiFi y por BLE, y lo unico que cambia es el tubo.
#
# Es ademas EL BANCO DE PRUEBAS del firmware v1.26: si esto funciona desde la
# raspi, lo que falle en el movil es de la app, no del nodo. Esa separacion es
# justo lo que faltaba la noche que se publicaron doce versiones seguidas.
#
# ⚠️ bleak es asincrono y el resto de las herramientas no. Se resuelve metiendo
# el bucle de asyncio en un hilo aparte y ofreciendo hacia fuera `sendall()` y
# `recv()` de toda la vida. No es elegante; es lo que evita reescribir
# `nodo.py` entero.

import asyncio
import atexit
import queue
import subprocess
import threading
import time

from bleak import BleakClient, BleakScanner

# Los mismos del firmware. Servicio "Nordic UART": los nombres RX/TX son desde
# el punto de vista del CLIENTE (nosotros escribimos en RX, recibimos por TX).
UUID_SERV = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
UUID_RX   = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
UUID_TX   = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

PREFIJO = "PTTLoRa"


def busca(segundos=8.0):
    """Devuelve [(mac, nombre)] de los nodos que se anuncian."""
    async def _b():
        vistos = await BleakScanner.discover(timeout=segundos)
        return [(d.address, d.name or "?") for d in vistos
                if (d.name or "").startswith(PREFIJO)]
    return asyncio.run(_b())


class TuboBle:
    """Cara de socket sobre BLE. `sendall`, `recv` y `close`, nada mas."""

    # ⚠️ CERRAR SIEMPRE, Y ESTO NO ES MANIA.
    #
    # Si el proceso se muere sin desconectar, **BlueZ deja el enlace abierto a
    # nivel de adaptador**: `bluetoothctl info` sigue diciendo `Connected: yes`
    # minutos despues. Y como el nodo admite un cliente a la vez y deja de
    # anunciarse mientras hay uno dentro, el resultado es un nodo que
    # desaparece de los escaneos y no acepta a nadie — con toda la pinta de ser
    # un fallo del firmware, que no lo es. Costo el primer intento de reconexion
    # de esta misma tarde.
    # Por eso: `atexit`, y ademas se puede usar con `with`.
    _abiertos = []

    def __init__(self, mac, plazo=25.0):
        self.mac = mac
        self.cola = queue.Queue()
        self.fin = threading.Event()
        self.listo = threading.Event()
        self.error = None
        self.mtu = 23
        self.loop = None
        self.cli = None
        TuboBle._abiertos.append(self)
        self.hilo = threading.Thread(target=self._corre, daemon=True)
        self.hilo.start()
        if not self.listo.wait(plazo):
            self.close()
            raise TimeoutError(
                "BLE: no se abrio el enlace a tiempo. Si el nodo no aparece en "
                "los escaneos, mira si BlueZ tiene el enlace colgado de una "
                "sesion anterior: `bluetoothctl info %s`" % mac)
        if self.error:
            self.close()
            raise self.error

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    # ------------------------------------------------------------ interno --
    def _corre(self):
        self.loop = asyncio.new_event_loop()
        asyncio.set_event_loop(self.loop)
        try:
            self.loop.run_until_complete(self._sesion())
        except Exception as e:          # noqa: BLE001 — se reenvia al llamante
            self.error = e
            self.listo.set()

    async def _sesion(self):
        self.cli = BleakClient(self.mac)
        await self.cli.connect()
        # Si veniamos de un enlace colgado, BlueZ lo reutiliza y aqui no se
        # nota; lo que no se puede es dejarlo abierto al salir (ver arriba).
        # Las notificaciones ANTES de avisar de que esta listo: quien reciba el
        # aviso manda ordenes inmediatamente y sin esto la respuesta se pierde.
        await self.cli.start_notify(UUID_TX, self._notif)
        # EL MTU DE VERDAD, NO EL DE MENTIRA.
        #
        # BlueZ negocia el MTU por su cuenta, pero no lo publica por D-Bus hasta
        # que alguien "adquiere" el canal de escritura; hasta entonces bleak
        # devuelve 23 y avisa con un warning. Con 23 se escribe de 20 en 20
        # bytes y una actualizacion de firmware de 1,1 MB tarda **media hora**
        # — que es lo que paso en la primera prueba de OTA por BLE del banco.
        # Con el MTU real (517 en un kernel moderno) son unos 500 bytes por
        # escritura y baja a minutos.
        # No es critico para la voz (un lote son 74 bytes) pero si para la OTA.
        try:
            adq = getattr(self.cli, '_acquire_mtu', None)
            if adq is not None:
                await adq()
            self.mtu = self.cli.mtu_size
        except Exception:
            self.mtu = 23
        self.listo.set()
        while not self.fin.is_set() and self.cli.is_connected:
            await asyncio.sleep(0.15)
        try:
            await self.cli.disconnect()
        except Exception:
            pass

    def _notif(self, _car, datos):
        self.cola.put(bytes(datos))

    # -------------------------------------------------------------- afuera --
    # CON RESPUESTA O SIN ELLA: NO ES UN DETALLE, DECIDE SI FUNCIONA.
    #
    # BLE tiene dos formas de escribir:
    #   - **sin respuesta** (write-no-response): rapida, la mitad de viajes por
    #     radio... y **sin ningun control de flujo**. Si el otro extremo o la
    #     propia pila no dan abasto, el paquete se tira y NADIE SE ENTERA.
    #   - **con respuesta**: cada escritura espera su confirmacion a nivel ATT.
    #     Mas lenta, pero no se puede perder nada.
    #
    # ⚠️ MEDIDO EN BANCO EL 8-sep-2026: la actualizacion de firmware por BLE
    # **fallaba en el primer trozo** mandando sin respuesta. El nodo recibia el
    # CMD_OTA_INI (una trama corta) y luego nada: 200 reenvios del trozo 1 y a
    # casa. Y no era el nodo atragantandose —`btatasco=0`, sin desbordar el
    # anillo, sin reiniciarse—, era que **los bytes no llegaban**: 512 bytes
    # troceados de 20 en 20 son 52 escrituras seguidas sin control de flujo.
    #
    # Asi que: la VOZ va sin respuesta (un lote son 74 bytes, 4 escrituras, y
    # ahi lo que importa es la latencia) y **todo lo gordo va con respuesta**.
    UMBRAL_FIABLE = 256

    def sendall(self, datos, fiable=None):
        """Trocea al MTU. Ver la nota de arriba sobre `fiable`."""
        if fiable is None:
            fiable = len(datos) > self.UMBRAL_FIABLE
        trozo = max(20, self.mtu - 3)
        for i in range(0, len(datos), trozo):
            cacho = datos[i:i + trozo]
            fut = asyncio.run_coroutine_threadsafe(
                self.cli.write_gatt_char(UUID_RX, cacho, response=fiable),
                self.loop)
            fut.result(10)

    def recv(self, _n=4096, plazo=0.2):
        try:
            return self.cola.get(timeout=plazo)
        except queue.Empty:
            return b''

    def conectado(self):
        return self.cli is not None and self.cli.is_connected

    def close(self):
        self.fin.set()
        if self.hilo.is_alive():
            self.hilo.join(5)
        if self in TuboBle._abiertos:
            TuboBle._abiertos.remove(self)

    @staticmethod
    def suelta_colgado(mac):
        """Fuerza a BlueZ a soltar un enlace que quedo de una sesion muerta.

        Es la salida de emergencia cuando el nodo "ha desaparecido": no se
        anuncia porque cree —con razon— que tiene un cliente dentro."""
        try:
            subprocess.run(['bluetoothctl', 'disconnect', mac],
                           capture_output=True, timeout=10)
        except Exception:
            pass


@atexit.register
def _cierra_todos():
    for t in list(TuboBle._abiertos):
        t.close()


if __name__ == '__main__':
    import sys
    if len(sys.argv) > 1 and sys.argv[1] == 'buscar':
        for mac, nombre in busca(float(sys.argv[2]) if len(sys.argv) > 2 else 8):
            print(f"{mac}  {nombre}")
    else:
        print(__doc__)
