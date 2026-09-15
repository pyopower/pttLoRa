#!/usr/bin/env python3
"""admin.py — la guardia del reflector: quien esta, aprobar, bloquear.

    pttlora-admin                         quien esta conectado ahora
    pttlora-admin pendientes              reflectores que esperan aprobacion
    pttlora-admin aprueba <ID> [nota]     que un reflector enlazado hable
    pttlora-admin desaprueba <ID>         que vuelva a solo escuchar
    pttlora-admin bloquea <quien> [horas] [motivo...]
    pttlora-admin desbloquea <quien>
    pttlora-admin perdona <ID|IP>         quita una sancion automatica
    pttlora-admin lista                   bloqueos, aprobados y sanciones
    pttlora-admin politica                como esta
    pttlora-admin politica nuevos   hablan|escuchan|fuera
    pttlora-admin politica gracia   <horas>     periodo de prueba (0 = sin él)
    pttlora-admin politica anonimas hablan|escuchan
    pttlora-admin politica contacto <texto>
    pttlora-admin aprueba-src <src> [nota]    una celda sin identidad que habla

<quien> puede ser:
    un ID          ab12cd34        (el principio de la clave de un reflector)
    un indicativo  EA1ABC          (sin SSID: todos sus SSID; con SSID: solo ese)
    una IP o rango 203.0.113.7  ·  203.0.113.0/24
    una estacion   src:09d562      (el src que sale en el registro)
    (si hay duda entre ID e indicativo: id:ab12cd34 · ind:EB3ACE)
    `0` horas o nada = para siempre.

El reflector relee la politica solo, en un par de segundos, sin reiniciar.
Ficheros: --politica (defecto /etc/pttlora/politica.json) y --estado
(defecto /var/lib/pttlora/estado.json), o PTTLORA_POLITICA / PTTLORA_ESTADO.
"""
import ipaddress
import json
import os
import re
import sys
import time

POLITICA = os.environ.get('PTTLORA_POLITICA', '/etc/pttlora/politica.json')
ESTADO = os.environ.get('PTTLORA_ESTADO', '/var/lib/pttlora/estado.json')
IND_RE = re.compile(r'^([A-Z0-9]{1,3}/)?[A-Z0-9]{1,3}[0-9][A-Z0-9]{0,4}[A-Z]'
                    r'(-([1-9]|[1-9][0-9]))?(/[A-Z0-9]{1,4})?$')
DEFECTO = {'nuevos': 'escuchan', 'gracia_horas': 72, 'anonimas': 'hablan', 'contacto': '',
           'aprobadas': {}, 'aprobadas_src': {}, 'bloqueos': [], 'perdones': {}}


def fecha(t):
    return time.strftime('%d-%b %H:%M', time.localtime(t)) if t else 'para siempre'


def hace(t):
    s = int(time.time() - t)
    if s < 120:
        return '%d s' % s
    if s < 7200:
        return '%d min' % (s // 60)
    if s < 172800:
        return '%d h' % (s // 3600)
    return '%d d' % (s // 86400)


def lee(f, defecto):
    try:
        with open(f) as fh:
            d = json.load(fh)
        return d if isinstance(d, dict) else dict(defecto)
    except FileNotFoundError:
        return json.loads(json.dumps(defecto))
    except ValueError as e:
        sys.exit('✘ %s esta roto (%s). Arreglalo a mano o borralo.' % (f, e))


def escribe_politica(p):
    tmp = POLITICA + '.tmp'
    try:
        os.makedirs(os.path.dirname(POLITICA) or '.', exist_ok=True)
        with open(tmp, 'w') as fh:
            json.dump(p, fh, indent=1, ensure_ascii=False)
        os.chmod(tmp, 0o644)
        os.replace(tmp, POLITICA)
    except PermissionError:
        sys.exit('✘ Sin permiso para escribir %s. Usa sudo.' % POLITICA)


def politica():
    p = lee(POLITICA, DEFECTO)
    for k, v in DEFECTO.items():
        p.setdefault(k, json.loads(json.dumps(v)))
    return p


def estado():
    e = lee(ESTADO, {})
    if e and time.time() - e.get('cuando', 0) > 30:
        print('⚠ el estado es de hace %s: ¿está parado el reflector?' % hace(e['cuando']))
    return e


def claves_conocidas(e, p):
    ks = set(e.get('vistos', {})) | set(p['aprobadas'])
    ks |= {c['id'] for c in e.get('conexiones', []) if c.get('id')}
    return ks


def resuelve_id(texto, e, p, exacto=False):
    t = texto.lower()
    if t.startswith('id:'):
        t = t[3:]
    if not re.fullmatch(r'[0-9a-f]{6,64}', t):
        return None
    ks = [k for k in claves_conocidas(e, p) if k.startswith(t)]
    if len(ks) > 1:
        sys.exit('✘ «%s» vale para varios reflectores: escribe más cifras.' % texto)
    if ks:
        return ks[0]
    return t if (len(t) == 64 or not exacto) else None


def que_es(texto, e, p):
    """(tipo, valor) de lo que se quiere bloquear."""
    if texto.lower().startswith('src:'):
        v = texto[4:].lower()
        if not re.fullmatch(r'[0-9a-f]{6}', v):
            sys.exit('✘ un src son 6 cifras hex, como src:09d562')
        return 'src', v
    try:
        red = ipaddress.ip_network(texto, strict=False)
        return 'ip', str(red) if '/' in texto else texto
    except ValueError:
        pass
    if texto.lower().startswith('id:'):
        k = resuelve_id(texto[3:], e, p, exacto=True)
        if not k:
            sys.exit('✘ No conozco ningún reflector con ID «%s».' % texto[3:])
        return 'id', k
    if texto.lower().startswith('ind:'):
        u = texto[4:].upper()
        if not IND_RE.match(u):
            sys.exit('✘ «%s» no es un indicativo.' % texto[4:])
        return 'ind', u
    k = resuelve_id(texto, e, p, exacto=True)
    u = texto.upper()
    # Un ID son cifras hex, y hay IDs con forma de indicativo (5c10aeef =
    # 5C1 0 AEEF). Con 8 cifras o más y un reflector conocido, es el ID: nadie
    # tiene un indicativo así. Con menos, se pregunta.
    if k and (len(texto) >= 8 or not IND_RE.match(u)):
        return 'id', k
    if IND_RE.match(u):
        if k:
            sys.exit('✘ «%s» puede ser un ID o un indicativo. Escribe id:%s o ind:%s.'
                     % (texto, texto, texto))
        return 'ind', u
    sys.exit('✘ No sé qué es «%s»: ni ID, ni indicativo, ni IP, ni src:xxxxxx.' % texto)


def nombre_de(tipo, valor, e):
    if tipo == 'id':
        v = e.get('vistos', {}).get(valor, {})
        return 'reflector %s%s' % (valor[:8], (' (%s %s)' % (v.get('ind', ''), v.get('nombre', ''))).rstrip()
                                    if v else '')
    return {'ind': 'indicativo %s', 'ip': 'IP %s', 'src': 'estación src %s'}[tipo] % valor


# ----------------------------------------------------------------- órdenes --
def o_estado(_):
    e = estado()
    p = politica()
    if not e:
        print('No hay estado todavía (%s). ¿Arrancó el reflector con --estado?' % ESTADO)
        return
    con = e.get('conexiones', [])
    print('Reflector :%s · %d conexiones · nuevos %s · anónimas %s'
          % (e.get('puerto'), len(con), p['nuevos'], p['anonimas']))
    en = e.get('enlace') or {}
    if en.get('a'):
        print('Enlace a %s (%s): %s%s' % (en['a'], en.get('modo'), en.get('estado', '?'),
                                         (' — ' + en['motivo']) if en.get('motivo') else ''))
        if en.get('id'):
            print('  ID de este reflector: %s' % en['id'][:8])
    print()
    marca = {'habla': '🟢', 'escucha': '🟡', 'fuera': '🔴', '': '⚪'}
    for c in sorted(con, key=lambda c: c.get('desde', 0)):
        tipo = {'local': 'local', 'anonima': 'sin identificar', 'verificando': 'verificando',
                'enlace': 'reflector'}.get(c['clase'], c['clase'])
        if c['quien'].startswith('ENLACE'):
            tipo = 'nuestro enlace'
        linea = '%s %s  %s' % (marca.get(c.get('veredicto', ''), '⚪'), c['ip'], tipo)
        if c.get('id'):
            linea += ' ID %s' % c['id'][:8]
        if c.get('ind'):
            linea += ' %s' % c['ind']
        print(linea)
        extra = '   hace %s · %d tramas, %d pasadas' % (hace(c['desde']), c['tramas'], c['pasadas'])
        if c.get('nombre'):
            extra += ' · %s' % c['nombre']
        print(extra)
        if c.get('motivo') and c['motivo'] != 'aprobada':
            print('   %s' % c['motivo'])
    vig = vigentes(e, p)
    if vig:
        print('\nSanciones automáticas en curso:')
        for k, s in vig:
            print('  %s %s: %s, hasta %s' % (k[:11], s.get('ind', ''), s['motivo'], fecha(s['hasta'])))
    pend = pendientes_de(e, p)
    if pend:
        print('\n%d reflector(es) esperando aprobación: pttlora-admin pendientes' % len(pend))


def vigentes(e, p):
    """Sanciones automaticas que siguen en pie: ni caducadas ni perdonadas."""
    ahora = time.time()
    return [(k, v['vigente']) for k, v in e.get('sanciones', {}).items()
            if v.get('vigente') and v['vigente']['hasta'] > ahora
            and p.get('perdones', {}).get(k, 0) <= v['vigente']['cuando']]


def pendientes_de(e, p):
    bloq = {b['valor'] for b in p['bloqueos'] if b.get('que') == 'id'
            and (not b.get('hasta') or b['hasta'] > time.time())}
    return [(k, v) for k, v in e.get('vistos', {}).items()
            if k not in p['aprobadas'] and not any(k.startswith(b) for b in bloq)]


def o_pendientes(_):
    e, p = estado(), politica()
    pend = pendientes_de(e, p)
    if not pend:
        print('Nadie espera aprobación.')
        return
    conectados = {c['id']: c for c in e.get('conexiones', []) if c.get('id')}
    for k, v in sorted(pend, key=lambda x: -x[1].get('ultima', 0)):
        c = conectados.get(k)
        prueba = c and c.get('motivo', '').startswith('periodo de prueba')
        print('%s %s  %s %s' % ('🟢' if prueba else '🟡' if c else '⚪', k[:8],
                                v.get('ind', ''), v.get('nombre', '')))
        print('   IP %s · primera vez hace %s · última hace %s'
              % (v.get('ip', '?'), hace(v.get('primera', 0)), hace(v.get('ultima', 0))))
        if c and c.get('motivo'):
            print('   %s' % c['motivo'])
    print('\nPara que hable:   pttlora-admin aprueba <ID>')
    print('Para echarlo:     pttlora-admin bloquea <ID> [horas] [motivo]')


def o_aprueba(a):
    if not a:
        sys.exit('uso: pttlora-admin aprueba <ID> [nota]')
    e, p = estado(), politica()
    k = resuelve_id(a[0], e, p, exacto=True)
    if not k:
        sys.exit('✘ No conozco ningún reflector con ID «%s». Mira: pttlora-admin pendientes' % a[0])
    p['aprobadas'][k] = {'nota': ' '.join(a[1:]), 'cuando': time.time()}
    escribe_politica(p)
    print('✔ Aprobado el %s: ya habla en la red.' % nombre_de('id', k, e))
    if any(b.get('que') == 'id' and k.startswith(b['valor']) for b in p['bloqueos']):
        print('⚠ Ojo: además está BLOQUEADO, y el bloqueo manda. pttlora-admin desbloquea %s' % k[:8])


def o_desaprueba(a):
    if not a:
        sys.exit('uso: pttlora-admin desaprueba <ID>')
    e, p = estado(), politica()
    k = resuelve_id(a[0], e, p, exacto=True)
    if not k or k not in p['aprobadas']:
        sys.exit('✘ «%s» no está aprobado.' % a[0])
    del p['aprobadas'][k]
    escribe_politica(p)
    print('✔ El %s vuelve a lo que diga la política para nuevos (%s).'
          % (nombre_de('id', k, e), p['nuevos']))


def o_bloquea(a):
    if not a:
        sys.exit('uso: pttlora-admin bloquea <ID|indicativo|IP|src:xxxxxx> [horas] [motivo]')
    e, p = estado(), politica()
    tipo, valor = que_es(a[0], e, p)
    horas, resto = 0.0, a[1:]
    if resto:
        try:
            horas = float(resto[0].replace(',', '.'))
            resto = resto[1:]
        except ValueError:
            pass
    motivo = ' '.join(resto)
    ahora = time.time()
    p['bloqueos'] = [b for b in p['bloqueos'] if not (b.get('que') == tipo and b.get('valor') == valor)]
    p['bloqueos'].append({'que': tipo, 'valor': valor, 'motivo': motivo, 'cuando': ahora,
                          'hasta': ahora + horas * 3600 if horas > 0 else None})
    escribe_politica(p)
    print('✔ Bloqueado: %s, %s%s.' % (nombre_de(tipo, valor, e),
                                     'hasta ' + fecha(ahora + horas * 3600) if horas > 0 else 'para siempre',
                                     (' («%s»)' % motivo) if motivo else ''))
    if tipo == 'ip':
        print('  Una IP de casa cambia sola: para un reflector, bloquea mejor su ID.')
    if tipo == 'ind':
        print('  Se cortan las transmisiones y balizas con ese indicativo, entren por donde entren.')


def o_desbloquea(a):
    if not a:
        sys.exit('uso: pttlora-admin desbloquea <ID|indicativo|IP|src:xxxxxx>')
    e, p = estado(), politica()
    tipo, valor = que_es(a[0], e, p)
    antes = len(p['bloqueos'])
    p['bloqueos'] = [b for b in p['bloqueos']
                     if not (b.get('que') == tipo and (b.get('valor') == valor or
                             (tipo == 'id' and valor.startswith(b.get('valor', '-')))))]
    if len(p['bloqueos']) == antes:
        sys.exit('✘ %s no estaba bloqueado.' % nombre_de(tipo, valor, e))
    escribe_politica(p)
    print('✔ Desbloqueado: %s.' % nombre_de(tipo, valor, e))


def o_perdona(a):
    if not a:
        sys.exit('uso: pttlora-admin perdona <ID|IP|todo>')
    e, p = estado(), politica()
    vig = [k for k, _ in vigentes(e, p)]
    if a[0] == 'todo':
        sujetos = vig
    else:
        tipo, valor = que_es(a[0], e, p)
        if tipo == 'id':
            sujetos = ['id:' + valor]
        elif tipo == 'ip':
            sujetos = ['ip:' + valor]
        else:
            sys.exit('✘ Las sanciones van por ID o por IP.')
    if not sujetos:
        sys.exit('No hay sanciones en curso.')
    ahora = time.time()
    for s in sujetos:
        p['perdones'][s] = ahora
    escribe_politica(p)
    print('✔ Perdonado: %s' % ', '.join(s[:11] for s in sujetos))


def o_aprueba_src(a):
    if not a or not re.fullmatch(r'(src:)?[0-9a-fA-F]{6}', a[0]):
        sys.exit('uso: pttlora-admin aprueba-src <src> [nota]')
    p = politica()
    v = a[0].lower().replace('src:', '')
    p['aprobadas_src'][v] = {'nota': ' '.join(a[1:]), 'cuando': time.time()}
    escribe_politica(p)
    print('✔ Una conexión sin identidad que balice como src %s podrá hablar aunque '
          'las anónimas solo escuchen.' % v)


def o_lista(_):
    e, p = estado(), politica()
    ahora = time.time()
    bl = [b for b in p['bloqueos'] if not b.get('hasta') or b['hasta'] > ahora]
    print('BLOQUEOS (%d)' % len(bl))
    for b in bl:
        print('  🔴 %s · %s%s' % (nombre_de(b['que'], b['valor'], e), fecha(b.get('hasta')),
                                  (' · ' + b['motivo']) if b.get('motivo') else ''))
    print('APROBADOS (%d)' % len(p['aprobadas']))
    for k, v in p['aprobadas'].items():
        print('  🟢 %s%s' % (nombre_de('id', k, e), (' · ' + v['nota']) if v.get('nota') else ''))
    if p['aprobadas_src']:
        print('CELDAS APROBADAS (%d)' % len(p['aprobadas_src']))
        for k, v in p['aprobadas_src'].items():
            print('  🟢 src %s%s' % (k, (' · ' + v['nota']) if v.get('nota') else ''))
    vig = vigentes(e, p)
    print('SANCIONES AUTOMÁTICAS (%d)' % len(vig))
    for k, s in vig:
        print('  🟡 %s %s · %s · hasta %s' % (k[:11], s.get('ind', ''), s['motivo'], fecha(s['hasta'])))


def o_politica(a):
    p = politica()
    if not a:
        print('nuevos   : %s   (reflectores identificados sin aprobar)' % p['nuevos'])
        print('gracia   : %s h (periodo de prueba en el que un reflector nuevo habla)'
              % p.get('gracia_horas', 72))
        print('anonimas : %s   (conexiones sin identidad: celdas, reflectores viejos)' % p['anonimas'])
        print('contacto : %s' % (p['contacto'] or '(nada)'))
        return
    k = a[0]
    if k == 'nuevos' and len(a) == 2 and a[1] in ('hablan', 'escuchan', 'fuera'):
        p['nuevos'] = a[1]
    elif k == 'gracia' and len(a) == 2 and re.fullmatch(r'\d+([.,]\d+)?', a[1]):
        p['gracia_horas'] = float(a[1].replace(',', '.'))
    elif k == 'anonimas' and len(a) == 2 and a[1] in ('hablan', 'escuchan'):
        p['anonimas'] = a[1]
        if a[1] == 'escuchan':
            print('⚠ Las celdas que se conectan directamente (sin identidad) dejarán de '
                  'hablar, salvo las de pttlora-admin aprueba-src.')
    elif k == 'contacto':
        p['contacto'] = ' '.join(a[1:])[:80]
    else:
        sys.exit('uso: pttlora-admin politica [nuevos hablan|escuchan|fuera | '
                 'gracia <horas> | anonimas hablan|escuchan | contacto <texto>]')
    escribe_politica(p)
    print('✔ Hecho.')
    o_politica([])


ORDENES = {'estado': o_estado, 'pendientes': o_pendientes, 'aprueba': o_aprueba,
           'desaprueba': o_desaprueba, 'bloquea': o_bloquea, 'desbloquea': o_desbloquea,
           'perdona': o_perdona, 'lista': o_lista, 'politica': o_politica,
           'aprueba-src': o_aprueba_src}


def main():
    global POLITICA, ESTADO
    a = sys.argv[1:]
    for op in ('--politica', '--estado'):
        if op in a:
            i = a.index(op)
            if op == '--politica':
                POLITICA = a[i + 1]
            else:
                ESTADO = a[i + 1]
            del a[i:i + 2]
    if a and a[0] in ('-h', '--ayuda', '--help', 'ayuda'):
        print(__doc__.strip())
        return
    orden = a[0] if a else 'estado'
    if orden not in ORDENES:
        print('Orden desconocida: %s\n' % orden)
        print(__doc__.strip())
        sys.exit(2)
    ORDENES[orden](a[1:])


if __name__ == '__main__':
    main()
