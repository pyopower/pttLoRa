#!/usr/bin/env python3
"""Banco de la guardia del reflector (identidad, lista negra, sanciones).

    ./pruebaguardia.py

Levanta reflectores de verdad en 127.0.0.1 con `--locales ninguna`, para que
la guardia trate a las conexiones de prueba como si vinieran de Internet, y
maneja la politica con `admin.py`, igual que lo haria una persona.
"""
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

AQUI = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, AQUI)
import identidad  # noqa: E402
from nodovirtual import (enmarcar, Desentrama, CMD_AIRE, CMD_IDENT, CMD_RETO,  # noqa: E402
                         CMD_FIRMA, CMD_AVISO, CONTEXTO_FIRMA, texto_kv, lee_kv)

T_VOZ, T_INICIO, T_FIN, T_HOLA = 1, 2, 3, 4
fallos = []
DIR = tempfile.mkdtemp(prefix='pttlora-guardia-')
POL = os.path.join(DIR, 'politica.json')
EST = os.path.join(DIR, 'estado.json')
procesos = []


def cab(tipo, src, stream, seq):
    return bytes([0xA1, 1, (tipo << 4) | 3, (src >> 16) & 255, (src >> 8) & 255,
                  src & 255, stream, seq])


def inicio(src, stream, ind):
    return cab(T_INICIO, src, stream, 0) + b'\x01' + ind.encode() + b'\0'


def voz(src, stream, seq):
    return cab(T_VOZ, src, stream, seq) + b'\x01\x05' + bytes([seq]) * 30


def fin(src, stream, seq):
    return cab(T_FIN, src, stream, seq)


def hola(src, ind, nombre):
    return cab(T_HOLA, src, 0, 0) + b'\x00\x64' + ind.encode() + b'\0' + nombre.encode() + b'\0'


def admin(*args, debe_ir=True):
    r = subprocess.run([sys.executable, os.path.join(AQUI, 'admin.py'),
                        '--politica', POL, '--estado', EST] + list(args),
                       capture_output=True, text=True)
    if r.returncode != 0 and debe_ir:
        comprueba('pttlora-admin %s' % ' '.join(args), False, (r.stdout + r.stderr).strip())
    return r.stdout + r.stderr


def arranca(puerto, *extra, log=None):
    p = subprocess.Popen([sys.executable, os.path.join(AQUI, 'nodovirtual.py'),
                          '--escucha', str(puerto)] + list(extra),
                         stdout=open(log, 'w') if log else subprocess.DEVNULL,
                         stderr=subprocess.STDOUT)
    procesos.append(p)
    return p


def espera_puerto(puerto):
    for _ in range(80):
        try:
            socket.create_connection(('127.0.0.1', puerto), 0.5).close()
            return
        except OSError:
            time.sleep(0.1)
    raise SystemExit('no arranca el puerto %d' % puerto)


class Par:
    """Un cliente que cuenta lo que le llega y, si se le da clave, se presenta."""
    def __init__(self, puerto, secreto=None, ind='', firma_mala=False):
        self.s = socket.create_connection(('127.0.0.1', puerto), 3)
        self.s.settimeout(None)
        self.recibidas, self.avisos = [], []
        self.cerrada = False
        self.secreto, self.firma_mala = secreto, firma_mala
        threading.Thread(target=self._lee, daemon=True).start()
        if secreto:
            clave = identidad.publica(secreto)
            self.s.sendall(enmarcar(CMD_IDENT, texto_kv({'v': 1, 'clave': clave.hex(),
                                                          'ind': ind, 'nombre': 'prueba'})))

    def _lee(self):
        d = Desentrama()
        try:
            while True:
                b = self.s.recv(4096)
                if not b:
                    break
                for tipo, p in d.mete(b):
                    if tipo == CMD_AIRE:
                        self.recibidas.append(p)
                    elif tipo == CMD_RETO:
                        sk = os.urandom(32) if self.firma_mala else self.secreto
                        self.s.sendall(enmarcar(CMD_FIRMA,
                                                identidad.firma(sk, CONTEXTO_FIRMA + p)))
                    elif tipo == CMD_AVISO:
                        self.avisos.append(lee_kv(p))
        except OSError:
            pass
        self.cerrada = True

    def manda(self, t):
        try:
            self.s.sendall(enmarcar(CMD_AIRE, t))
        except OSError:
            self.cerrada = True

    def habla(self, src, stream, ind='EA1ZZZ', lotes=2):
        self.manda(inicio(src, stream, ind))
        for i in range(lotes):
            self.manda(voz(src, stream, i + 1))
        self.manda(fin(src, stream, lotes + 1))

    def oyo(self, src, stream=None):
        return sum(1 for t in self.recibidas
                   if (t[3] << 16 | t[4] << 8 | t[5]) == src and (stream is None or t[6] == stream))

    def estado(self):
        return self.avisos[-1].get('estado') if self.avisos else None

    def cierra(self):
        try:
            self.s.close()
        except OSError:
            pass


def comprueba(nombre, ok, detalle=''):
    print('  %s %s %s' % ('OK  ' if ok else 'FALLO', nombre, detalle))
    if not ok:
        fallos.append(nombre)


def hasta(cond, s=5.0):
    t = time.time() + s
    while time.time() < t:
        if cond():
            return True
        time.sleep(0.05)
    return cond()


def turno_libre():
    time.sleep(2.3)


P = 24501


def main():
    with open(POL, 'w') as f:
        json.dump({'nuevos': 'escuchan', 'gracia_horas': 0, 'anonimas': 'hablan',
                   'contacto': 'admin@ejemplo'}, f)
    arranca(P, '--politica', POL, '--estado', EST, '--locales', 'ninguna',
            log=os.path.join(DIR, 'reflector.log'))
    espera_puerto(P)
    oido = Par(P)                       # alguien de la red que escucha
    src = 0x100000

    print('1. un reflector nuevo se identifica y solo escucha')
    sk = os.urandom(32)
    idx = identidad.id_corto(identidad.publica(sk))
    red = Par(P, sk, 'EA4MAD')
    comprueba('le dicen «escucha»', hasta(lambda: red.estado() == 'escucha'), str(red.estado()))
    comprueba('...y le dan el contacto', bool(red.avisos) and red.avisos[-1].get('contacto') == 'admin@ejemplo')
    red.habla(src + 1, 1)
    time.sleep(0.5)
    comprueba('lo suyo no sale', oido.oyo(src + 1) == 0)
    oido.habla(src + 2, 2)
    comprueba('pero oye la red', hasta(lambda: red.oyo(src + 2) >= 1))
    turno_libre()
    comprueba('sale en pendientes', hasta(lambda: idx in admin('pendientes'), 7))

    print('2. se aprueba y habla, sin reiniciar nada')
    print('   ' + admin('aprueba', idx, 'grupo de Madrid').strip())
    comprueba('le dicen «habla»', hasta(lambda: red.estado() == 'habla', 5), str(red.estado()))
    red.habla(src + 3, 3)
    comprueba('ahora lo suyo sale', hasta(lambda: oido.oyo(src + 3) >= 3, 2))
    turno_libre()

    print('3. firma falsa (alguien que se hace pasar por esa identidad)')
    falso = Par(P, sk, 'EA4MAD', firma_mala=True)
    comprueba('le cierran la puerta', hasta(lambda: falso.cerrada, 3))
    comprueba('el de verdad sigue dentro', not red.cerrada)

    print('4. se bloquea por ID')
    print('   ' + admin('bloquea', idx, '0', 'ruido constante').split('\n')[0])
    comprueba('le echan y le dicen por qué', hasta(lambda: red.cerrada, 5) and red.estado() == 'fuera',
              str(red.avisos[-1] if red.avisos else ''))
    otra_ip_misma_clave = Par(P, sk, 'EA4MAD')
    comprueba('vuelve (otra IP, misma clave): fuera otra vez', hasta(lambda: otra_ip_misma_clave.cerrada, 5))
    sk2 = os.urandom(32)
    disfrazado = Par(P, sk2, 'EA4MAD-2')
    comprueba('vuelve con clave NUEVA: solo escucha', hasta(lambda: disfrazado.estado() == 'escucha'))
    disfrazado.habla(src + 4, 4)
    time.sleep(0.5)
    comprueba('...y no se le oye', oido.oyo(src + 4) == 0)
    disfrazado.cierra()
    print('   ' + admin('desbloquea', idx).strip())

    print('4b. un ID con forma de indicativo se entiende como ID')
    e = {'vistos': {'5c10aeef' + '0' * 56: {'ind': 'X'}}}
    sys.path.insert(0, AQUI)
    import admin as A
    comprueba('5c10aeef = ID', A.que_es('5c10aeef', e, A.politica())[0] == 'id')
    comprueba('ind:5C10AEEF = indicativo', A.que_es('ind:5C10AEEF', e, A.politica())[0] == 'ind')
    comprueba('EB3ACE sin reflector con ese ID = indicativo', A.que_es('EB3ACE', e, A.politica())[0] == 'ind')

    print('5. lista negra por indicativo (entre por donde entre)')
    anon = Par(P)
    print('   ' + admin('bloquea', 'EA9BAD', '2', 'pruebas').split('\n')[0])
    time.sleep(1.5)
    anon.habla(src + 5, 5, ind='EA9BAD-7')
    time.sleep(0.5)
    comprueba('EA9BAD-7 no sale (bloqueado sin SSID = todos)', oido.oyo(src + 5) == 0)
    turno_libre()
    anon.habla(src + 6, 6, ind='EA1BIEN')
    comprueba('otro indicativo por la misma conexión sí', hasta(lambda: oido.oyo(src + 6) >= 3, 2))
    turno_libre()
    anon.manda(hola(src + 5, 'EA9BAD', 'celda'))
    time.sleep(0.4)
    comprueba('sus balizas tampoco', oido.oyo(src + 5) == 0)

    print('6. lista negra por src')
    print('   ' + admin('bloquea', 'src:%06x' % (src + 7)).split('\n')[0])
    time.sleep(1.5)
    anon.habla(src + 7, 7, ind='EA1OTRO')
    time.sleep(0.5)
    comprueba('ese src no sale', oido.oyo(src + 7) == 0)
    turno_libre()

    def ajeno(ind):
        """Un servidor ajeno identificado y aprobado."""
        k = os.urandom(32)
        par = Par(P, k, ind)
        hasta(lambda: par.estado() is not None, 3)
        admin('aprueba', 'id:' + identidad.id_corto(identidad.publica(k)))
        hasta(lambda: par.estado() == 'habla', 5)
        return par, k

    print('7. sanción sola a un servidor ajeno: ruido (pulsaciones cortas en ráfaga)')
    anon.cierra()
    ruidoso, k_ruido = ajeno('EA1RUIDO')
    for i in range(12):
        ruidoso.habla(src + 20, 20 + i, ind='EA1RUIDO', lotes=0)
        time.sleep(0.25)
    comprueba('le dicen «escucha» y por qué', hasta(lambda: ruidoso.estado() == 'escucha', 5),
              str(ruidoso.avisos[-1].get('motivo') if ruidoso.avisos else ''))
    comprueba('sale en las sanciones', hasta(lambda: 'ruido' in admin('lista'), 7))
    turno_libre()
    ruidoso.habla(src + 21, 60, ind='EA1RUIDO')
    time.sleep(0.5)
    comprueba('mientras dura, no se le oye', oido.oyo(src + 21) == 0)
    comprueba('...pero sigue escuchando', not ruidoso.cerrada)
    ruidoso.cierra()

    print('8. sanción sola a un servidor ajeno: tormenta, y perdón')
    tormenta, k_torm = ajeno('EA1TORM')
    for i in range(400):
        tormenta.manda(voz(src + 30, 30, i % 256))
    comprueba('le echan', hasta(lambda: tormenta.cerrada, 5))
    otra = Par(P, k_torm, 'EA1TORM')
    comprueba('vuelve con su identidad (sea cual sea su IP): fuera', hasta(lambda: otra.cerrada, 5))
    print('   ' + admin('perdona', 'id:' + identidad.id_corto(identidad.publica(k_torm))).strip())
    time.sleep(1.5)
    vuelve = Par(P, k_torm, 'EA1TORM')
    comprueba('perdonado, vuelve a hablar', hasta(lambda: vuelve.estado() == 'habla', 5))
    vuelve.cierra()

    print('9. basura de un servidor ajeno')
    basura, k_bas = ajeno('EA1BASURA')
    for i in range(40):
        basura.s.sendall(enmarcar(0x07, b'hola que tal'))
    comprueba('sancionado por basura', hasta(lambda: 'protocolo' in admin('lista'), 5))
    basura.cierra()
    admin('perdona', 'todo', debe_ir=False)

    print('9b. los usuarios de siempre (celdas sin identidad): como siempre')
    for i in range(25):
        Par(P).cierra()
    time.sleep(0.5)
    celda_floja = Par(P)
    time.sleep(0.5)
    comprueba('25 reconexiones seguidas: sigue entrando', not celda_floja.cerrada)
    turno_libre()
    celda_floja.habla(src + 35, 35, ind='EA3CELDA')
    comprueba('...y se la oye', hasta(lambda: oido.oyo(src + 35) >= 3, 2))
    loca = Par(P)
    time.sleep(0.2)
    for i in range(400):
        loca.manda(hola(src + 36, 'EA3LOCA', 'x%d' % i))
    comprueba('una celda que inunda: se corta esa conexión', hasta(lambda: loca.cerrada, 5))
    comprueba('...las demás de su misma IP siguen', not celda_floja.cerrada)
    turno_libre()
    otra_vez = Par(P)
    time.sleep(0.5)
    otra_vez.habla(src + 37, 37, ind='EA3LOCA')
    comprueba('...y ella puede volver en el acto', hasta(lambda: oido.oyo(src + 37) >= 3, 2))
    l = admin('lista')
    comprueba('sin sanciones apuntadas', 'SANCIONES AUTOMÁTICAS (0)' in l, l[l.find('SANCIONES'):].strip())
    otra_vez.cierra()
    celda_floja.cierra()

    print('10. bloqueo por IP (y que caduca)')
    time.sleep(1.5)
    with open(POL) as f:
        p = json.load(f)
    p['bloqueos'].append({'que': 'ip', 'valor': '127.0.0.0/8', 'hasta': time.time() + 3,
                          'motivo': 'rango de pruebas', 'cuando': time.time()})
    with open(POL, 'w') as f:
        json.dump(p, f)
    time.sleep(1.8)
    ip_mala = Par(P)
    comprueba('rechazada al conectar', hasta(lambda: ip_mala.cerrada, 3))
    time.sleep(3.0)
    ip_ok = Par(P)
    time.sleep(0.8)
    comprueba('caducado el bloqueo, entra', not ip_ok.cerrada)

    print('11. anónimas escuchan, salvo una celda aprobada por src')
    print('   ' + admin('politica', 'anonimas', 'escuchan').split('\n')[0])
    time.sleep(1.8)
    oido2 = ip_ok
    celda = Par(P)
    time.sleep(0.3)
    celda.habla(src + 40, 40, ind='EA1CELDA')
    time.sleep(0.5)
    comprueba('una celda sin aprobar no sale', oido2.oyo(src + 40) == 0)
    print('   ' + admin('aprueba-src', '%06x' % (src + 41), 'celda del cerro').split('\n')[0])
    time.sleep(1.5)
    celda.manda(hola(src + 41, 'EA1CELDA', 'celdaCERRO'))
    time.sleep(0.4)
    turno_libre()
    celda.habla(src + 42, 42, ind='EA1CELDA')
    comprueba('tras su baliza aprobada, sí sale', hasta(lambda: oido2.oyo(src + 42) >= 3, 2))

    print('12. el reflector enlazado de verdad (nodovirtual --identidad)')
    admin('politica', 'anonimas', 'hablan')
    fid = os.path.join(DIR, 'identidad')
    arranca(P + 1, '--enlace', '127.0.0.1:%d' % P, '--identidad', fid, '--indicativo', 'EA2ABC',
            '--estado', os.path.join(DIR, 'estado-abajo.json'),
            log=os.path.join(DIR, 'abajo.log'))
    espera_puerto(P + 1)
    abajo = Par(P + 1)
    time.sleep(0.3)
    ide = identidad.id_corto(identidad.carga_o_crea(fid)[1])
    comprueba('aparece en pendientes', hasta(lambda: ide in admin('pendientes'), 8))

    def estado_abajo():
        try:
            with open(os.path.join(DIR, 'estado-abajo.json')) as f:
                return json.load(f).get('enlace', {}).get('estado')
        except (OSError, ValueError):
            return None
    comprueba('abajo sabe que está pendiente', hasta(lambda: estado_abajo() == 'escucha', 8), str(estado_abajo()))
    abajo.habla(src + 50, 50)
    time.sleep(0.6)
    comprueba('lo de abajo no sube', oido2.oyo(src + 50) == 0)
    admin('aprueba', ide)
    comprueba('aprobado: abajo lo sabe', hasta(lambda: estado_abajo() == 'habla', 8), str(estado_abajo()))
    turno_libre()
    abajo.habla(src + 51, 51)
    comprueba('y ahora sube', hasta(lambda: oido2.oyo(src + 51) >= 3, 2))

    print('13. periodo de prueba')
    for par in (abajo,):
        par.cierra()
    admin('politica', 'gracia', '1')
    time.sleep(1.5)
    sk3 = os.urandom(32)
    nuevo = Par(P, sk3, 'EA5NUEVO')
    comprueba('una red nueva habla durante la prueba', hasta(lambda: nuevo.estado() == 'habla'),
              str(nuevo.avisos[-1] if nuevo.avisos else ''))
    comprueba('...y se le dice hasta cuándo', bool(nuevo.avisos) and
              'periodo de prueba' in nuevo.avisos[-1].get('motivo', ''))
    turno_libre()
    nuevo.habla(src + 60, 60)
    comprueba('se la oye', hasta(lambda: oido2.oyo(src + 60) >= 3, 2))
    turno_libre()
    sk4 = os.urandom(32)
    segundo = Par(P, sk4, 'EA5OTRO')
    comprueba('otra nueva desde la misma red, a la vez: escucha', hasta(lambda: segundo.estado() == 'escucha'),
              str(segundo.avisos[-1].get('motivo') if segundo.avisos else ''))
    segundo.cierra()
    id3 = identidad.id_corto(identidad.publica(sk3))
    admin('bloquea', 'id:' + id3, '0', 'pruebas')
    comprueba('bloqueada, fuera', hasta(lambda: nuevo.cerrada, 5))
    sk5 = os.urandom(32)
    vuelve = Par(P, sk5, 'EA5NUEVO-2')
    comprueba('vuelve con clave nueva desde la misma red: sin prueba, escucha',
              hasta(lambda: vuelve.estado() == 'escucha'),
              str(vuelve.avisos[-1].get('motivo') if vuelve.avisos else ''))
    vuelve.cierra()
    admin('desbloquea', 'id:' + id3)
    admin('politica', 'gracia', '0.001')        # 3,6 s
    time.sleep(1.2)
    sk6 = os.urandom(32)
    corto = Par(P, sk6, 'EA6CORTO')
    comprueba('prueba corta: empieza hablando', hasta(lambda: corto.estado() == 'habla', 3),
              str(corto.avisos[-1].get('motivo') if corto.avisos else ''))
    comprueba('se acaba sola y pasa a escuchar', hasta(lambda: corto.estado() == 'escucha', 12),
              str(corto.avisos[-1].get('motivo') if corto.avisos else ''))
    corto.cierra()
    time.sleep(0.5)
    corto2 = Par(P, sk6, 'EA6CORTO')
    comprueba('reconectar no reinicia la prueba', hasta(lambda: corto2.estado() == 'escucha', 3))
    print('\n' + admin().strip())


if __name__ == '__main__':
    try:
        main()
    finally:
        for p in procesos:
            p.kill()
    print()
    print('RESULTADO: %s' % ('TODO OK' if not fallos else
                             'FALLAN %d: %s' % (len(fallos), ', '.join(fallos))))
    if fallos:
        print('(registros en %s)' % DIR)
    else:
        shutil.rmtree(DIR, ignore_errors=True)
    sys.exit(1 if fallos else 0)
