#!/usr/bin/env python3
"""registro.py — el CENSO de la red PTT LoRa: que celulas hay, desde cuando y
donde.

    ./registro.py [--reflector 127.0.0.1:4461] [--fichero registro.json]
                  [--cada 30] [--muerta 180]

⚠️ ESTO ES UN OBSERVADOR, Y NO PUEDE SER OTRA COSA. Un censo global es, por
definicion, una funcion de Internet: mira y anota, y si se cae, en la radio no
se entera nadie. En el momento en que algo de la operacion dependa de el,
habriamos hecho de Internet la piedra angular por la puerta de atras — que es
justo lo que este proyecto no quiere. No manda nada, no arbitra nada y no
escribe en ningun nodo.

⚠️ Y ES UNA LISTA APARTE, NUNCA la de "a la vista". Son dos cosas distintas y
mezclarlas ya costo un agujero en la malla (HOJA-DE-RUTA §4.17):

    a la vista  = lo mantiene cada nodo con su antena  = "me oyes por radio"
    en la red   = lo mantiene esto, por los enlaces    = "existes"

La primera funciona sin Internet y decide si un nodo repite. La segunda
desaparece con Internet y no decide nada.

QUE SE GUARDA: aparatos. Nombre, indicativo de la estacion, papel, posicion,
bateria, cuando se vio por primera y ultima vez, y cada cuanto baliza. De las
PERSONAS no se guarda nada: la actividad de voz se cuenta agregada —cuantas
transmisiones y cuantos segundos— sin quien hablo ni desde donde. Las balizas
describen un CACHARRO; el INICIO describe una PERSONA, y ese no entra aqui.

LO QUE ESTO **NO** PUEDE SABER, y conviene tenerlo claro antes de mirar el
fichero: **quien oye a quien**. El reflector si sabe por que conexion entro
cada baliza, pero no se lo pasa a sus clientes, y aunque lo hiciera una
IP:puerto no identifica a un nodo. Lo unico que se deduce aqui es por CUANTOS
enlaces distintos entra cada baliza (`testigos`), que ya dice si hay
solapamiento aunque no diga entre quienes.

El grafo de verdad —con el dBm de cada arista, que es lo unico que sirve para
planificar sombras— necesita que el nodo mande un informe por su enlace. Eso es
la fase siguiente y toca firmware. Un grafo sin dBm dice que A oye a X, pero no
si el enlace esta holgado o al limite, que es justo la pregunta.
"""

import json, os, socket, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
# Se reutiliza el desentramado y el lector de balizas del igate en vez de
# reescribirlos: `lee_baliza` tiene dentro la trampa de la posicion —8 bytes
# binarios que NO se pueden partir por ceros— y duplicarla seria repetir el
# fallo del `hash_indicativo` de §4.9.
from igate import (desentrama, lee_baliza, CMD_AIRE, CAB_LEN, PROTO_MAGIC,
                   T_HOLA, T_INICIO,
                   HOLA_REPETIDOR, HOLA_PUENTE, HOLA_CELDA)

T_FIN, T_INFORME = 3, 5


def papel_de(flags):
    """La CELDA primero: es infraestructura, y es el dato que decide donde va
    la siguiente. Ojo, `HOLA_REPETIDOR` dice si repite DE VERDAD, no que perfil
    lleva puesto (ver la nota del firmware: la baliza mentia hasta la v1.29)."""
    if flags & HOLA_CELDA:
        return 'celda'
    if flags & HOLA_REPETIDOR:
        return 'repetidor'
    return 'cliente'


class Censo:
    def __init__(self, fichero, muerta_s):
        self.fichero = fichero
        self.muerta_s = muerta_s
        self.nodos = {}          # src -> ficha
        self.actividad = []      # transmisiones, SIN quien
        self.hablando = None     # (src, stream, cuando)
        self.arranque = time.time()
        self._lee()

    # ------------------------------------------------------------ persistir --
    def _lee(self):
        """Al arrancar se recupera lo que habia: un censo que se olvida de todo
        cada vez que se reinicia el servicio no es un censo, es una foto."""
        try:
            with open(self.fichero) as f:
                d = json.load(f)
            self.nodos = {int(k): v for k, v in d.get('nodos', {}).items()}
            self.actividad = d.get('actividad', [])
        except (OSError, ValueError):
            pass

    def guarda(self):
        ahora = time.time()
        d = {'generado': ahora,
             'generado_txt': time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(ahora)),
             'nodos': {str(k): v for k, v in self.nodos.items()},
             'actividad': self.actividad[-500:]}
        # Escritura atomica: el fichero lo puede estar leyendo un panel web
        # justo ahora, y medio JSON es peor que ninguno.
        tmp = self.fichero + '.tmp'
        with open(tmp, 'w') as f:
            json.dump(d, f, indent=1, ensure_ascii=False)
        os.replace(tmp, self.fichero)

    # ---------------------------------------------------------------- censo --
    def baliza(self, b, ahora):
        f = self.nodos.get(b['src'])
        if f is None:
            f = self.nodos[b['src']] = {
                'src': '%06x' % b['src'], 'nombre': '', 'indicativo': '',
                'papel': '', 'lat': None, 'lon': None, 'bateria': 0,
                'primera': ahora, 'ultima': 0, 'balizas': 0,
                'testigos': 1, 'cadencia_s': None}
        # El nombre y la posicion solo se pisan si vienen: una baliza de un
        # firmware viejo no debe borrar lo que ya se sabia del nodo.
        if b['nombre']:
            f['nombre'] = b['nombre']
        if b['indicativo']:
            f['indicativo'] = b['indicativo']
        if b['lat'] is not None:
            f['lat'], f['lon'] = b['lat'], b['lon']
        f['papel'] = papel_de(b['flags'])
        f['puente'] = bool(b['flags'] & HOLA_PUENTE)
        if b['bateria']:
            f['bateria'] = b['bateria']

        # TESTIGOS: la misma baliza llega una vez POR CADA enlace que la mete,
        # asi que contar las copias que caen juntas dice por cuantos nodos con
        # Internet se la oye. No dice quienes son —eso necesita el informe del
        # nodo, ver la cabecera— pero un 1 constante en una celda que deberia
        # solaparse con otra ya es una respuesta.
        if f['ultima'] and ahora - f['ultima'] < 5:
            f['testigos'] = f.get('testigos', 1) + 1
            return                      # copia de la misma baliza: no se cuenta
        if f['ultima']:
            hueco = ahora - f['ultima']
            # Media corrida, para que un hueco suelto no descoloque la cadencia.
            f['cadencia_s'] = round(hueco if f['cadencia_s'] is None
                                    else f['cadencia_s'] * 0.7 + hueco * 0.3, 1)
        f['testigos'] = 1
        f['ultima'] = ahora
        f['balizas'] = f.get('balizas', 0) + 1

    # ------------------------------------------------------------- informe --
    def informe(self, src, texto, ahora, seq=0):
        """La baliza que no sale al aire (fw v1.43). Trae lo que la baliza de
        RF no puede permitirse: ajustes de radio, salud y **el grafo con dBm**.

        Texto `clave=valor`, y las claves que no se conozcan se ignoran a
        proposito: es lo que permite añadir campos sin romper este lector."""
        campos = {}
        for trozo in texto.split(' '):
            k, _, v = trozo.partition('=')
            if k and v:
                campos[k] = v
        f = self.nodos.get(src)
        if f is None:
            # Un nodo puede informar antes de que le hayamos oido balizar.
            f = self.nodos[src] = {
                'src': '%06x' % src, 'nombre': '', 'indicativo': '',
                'papel': '', 'lat': None, 'lon': None, 'bateria': 0,
                'primera': ahora, 'ultima': 0, 'balizas': 0,
                'testigos': 1, 'cadencia_s': None}
        for k, destino in (('v', 'version'), ('pl', 'placa'), ('nom', 'nombre'),
                           ('ind', 'indicativo'), ('fq', 'frecuencia'),
                           ('sf', 'sf'), ('bw', 'ancho_khz'), ('pot', 'potencia'),
                           ('ch', 'canal'), ('up', 'uptime_s')):
            if k in campos:
                f[destino] = campos[k]
        # ⚠️ Solo con la trama 0. Las de vecinos no traen contadores, y
        # escribirlos igual los dejaria a cero en cada informe.
        cont = {k: campos[k] for k in
                ('rx', 'net', 'tx', 'rep', 'dup', 'mal', 'heap') if k in campos}
        if cont:
            f['contadores'] = cont
        # ⚠️ ESTE NODO TIENE INTERNET, y por eso puede informar. Se anota
        # explicitamente: un nodo que informa es, por definicion, uno con enlace.
        f['enlace'] = True
        f['informe'] = ahora
        # EL GRAFO, que viene en tramas APARTE (fw v1.46): la 0 trae el estado
        # del nodo y reinicia la lista; las siguientes (`seq` 1, 2, 3...) traen
        # vecinos y se ACUMULAN. Antes iba pegado al estado y se cortaba en
        # silencio a los dos vecinos — ver la nota de `manda_informe`.
        oye = [] if seq == 0 else list(f.get('oye_a', []))
        for v in campos.get('vec', '').split(','):
            if not v:
                continue
            trozos = v.split(':')
            if len(trozos) != 3:
                continue
            oye.append({'src': trozos[0],
                        'rssi': None if trozos[1] == '-' else int(trozos[1]),
                        'papel': 'celda' if trozos[2] == 'c' else 'nodo'})
        f['oye_a'] = oye

    # ----------------------------------------------------------- actividad --
    def voz(self, tipo, src, stream, ahora):
        """Cuenta transmisiones y segundos, SIN quien. Ver la cabecera."""
        if tipo == T_INICIO:
            self.hablando = [src, stream, ahora]
        elif tipo == T_FIN and self.hablando and self.hablando[1] == stream:
            seg = round(ahora - self.hablando[2], 1)
            if 0 < seg < 300:
                self.actividad.append({'cuando': ahora, 'segundos': seg})
            self.hablando = None

    def vivos(self, ahora):
        return [f for f in self.nodos.values()
                if ahora - f['ultima'] < self.muerta_s]

    def resumen(self, ahora):
        v = self.vivos(ahora)
        celdas = [f for f in v if f['papel'] == 'celda']
        hoy = [a for a in self.actividad if ahora - a['cuando'] < 86400]
        aristas = sum(len(f.get('oye_a', [])) for f in v)
        return ('%d nodos vivos (%d celdas, %d con enlace) de %d censados · '
                '%d enlaces de radio conocidos · '
                '%d transmisiones y %.0f s de voz en 24 h'
                % (len(v), len(celdas), sum(1 for f in v if f.get('enlace')),
                   len(self.nodos), aristas,
                   len(hoy), sum(a['segundos'] for a in hoy)))


def main():
    a = sys.argv[1:]

    def opt(nombre, defecto):
        return a[a.index(nombre) + 1] if nombre in a else defecto

    destino = opt('--reflector', '127.0.0.1:4461')
    fichero = opt('--fichero', 'registro.json')
    cada = float(opt('--cada', '30'))
    muerta = float(opt('--muerta', '180'))
    host, _, puerto = destino.partition(':')

    censo = Censo(fichero, muerta)
    print('-- censo de la red; %s -> %s' % (destino, fichero), flush=True)
    print('-- ' + censo.resumen(time.time()), flush=True)

    espera = 3
    while True:
        try:
            s = socket.create_connection((host, int(puerto or 4461)), 10)
        except OSError as e:
            print('-- sin reflector (%s); reintento en %ds' % (e, espera), flush=True)
            time.sleep(espera)
            espera = min(espera * 2, 60)
            continue
        print('-- enganchado al reflector', flush=True)
        espera = 3
        resto = b''
        ultimo_guardado = 0
        try:
            # El timeout no es para detectar nada: es para que el guardado
            # periodico ocurra tambien cuando la red esta callada, que es
            # precisamente cuando interesa ver desde cuando lo esta.
            s.settimeout(5)
            while True:
                try:
                    trozo = s.recv(4096)
                    if not trozo:
                        break
                except socket.timeout:
                    trozo = b''
                ahora = time.time()
                tramas, resto = desentrama(trozo, resto)
                for tipo_k, p in tramas:
                    if tipo_k != CMD_AIRE or len(p) < CAB_LEN or p[0] != PROTO_MAGIC:
                        continue
                    t = p[2] >> 4
                    src = (p[3] << 16) | (p[4] << 8) | p[5]
                    if t == T_HOLA:
                        b = lee_baliza(p)
                        if b:
                            censo.baliza(b, ahora)
                    elif t == T_INFORME:
                        censo.informe(src, p[CAB_LEN:].decode('ascii', 'ignore'),
                                      ahora, p[7])
                    elif t in (T_INICIO, T_FIN):
                        censo.voz(t, src, p[6], ahora)
                if ahora - ultimo_guardado >= cada:
                    ultimo_guardado = ahora
                    censo.guarda()
                    print('   %s' % censo.resumen(ahora), flush=True)
        except OSError as e:
            print('-- se corto (%s)' % e, flush=True)
        finally:
            censo.guarda()
            try: s.close()
            except OSError: pass


if __name__ == '__main__':
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        pass
