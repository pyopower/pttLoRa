#!/usr/bin/env python3
"""identidad.py — la identidad de un reflector: una clave Ed25519.

    ./identidad.py <fichero>        crea la identidad si no existe y dice su ID

POR QUE UNA CLAVE Y NO UNA IP NI UN INDICATIVO
----------------------------------------------
Para poder echar de verdad a un reflector enlazado hace falta saber QUIEN es, y
ni la IP ni el indicativo lo dicen: la IP de una conexion de casa cambia sola, y
un indicativo lo teclea cualquiera. Una clave privada que no sale nunca de la
maquina, si: el reflector de arriba le manda un reto al azar, este lo firma, y
con la clave publica se comprueba sin que el secreto viaje. Robar la identidad
de una red aprobada escuchando la conexion no sirve de nada.

Ed25519 y no HMAC como el relevo de mando porque aqui el de arriba NO puede
conocer el secreto de antemano: cualquiera tiene que poder crearse una identidad
sin pedir nada a nadie, y que luego se le apruebe o se le bloquee.

Puro Python, sin dependencias —la regla de todo el servidor—, siguiendo la
implementacion de referencia de la RFC 8032 (seccion 6). Es lenta comparada con
una libreria en C, pero se usa UNA vez por conexion: firmar cuesta decimas de
segundo en una Raspberry y verificar lo mismo en el servidor.

Esto NO cifra nada. La voz sigue en claro, como debe ser en radioaficionados:
lo unico que se prueba es quien habla por el enlace.
"""
import hashlib
import os
import sys

_p = 2 ** 255 - 19
_q = 2 ** 252 + 27742317777372353535851937790883648493


def _inv(x):
    return pow(x, _p - 2, _p)


_d = -121665 * _inv(121666) % _p
_raiz_m1 = pow(2, (_p - 1) // 4, _p)


def _sha512_modq(s):
    return int.from_bytes(hashlib.sha512(s).digest(), 'little') % _q


def _suma(P, Q):
    A = (P[1] - P[0]) * (Q[1] - Q[0]) % _p
    B = (P[1] + P[0]) * (Q[1] + Q[0]) % _p
    C = 2 * P[3] * Q[3] * _d % _p
    D = 2 * P[2] * Q[2] % _p
    E, F, G, H = B - A, D - C, D + C, B + A
    return (E * F % _p, G * H % _p, F * G % _p, E * H % _p)


def _por(s, P):
    Q = (0, 1, 1, 0)
    while s > 0:
        if s & 1:
            Q = _suma(Q, P)
        P = _suma(P, P)
        s >>= 1
    return Q


def _iguales(P, Q):
    return ((P[0] * Q[2] - Q[0] * P[2]) % _p == 0 and
            (P[1] * Q[2] - Q[1] * P[2]) % _p == 0)


def _recupera_x(y, signo):
    if y >= _p:
        return None
    x2 = (y * y - 1) * _inv(_d * y * y + 1) % _p
    if x2 == 0:
        return None if signo else 0
    x = pow(x2, (_p + 3) // 8, _p)
    if (x * x - x2) % _p != 0:
        x = x * _raiz_m1 % _p
    if (x * x - x2) % _p != 0:
        return None
    if (x & 1) != signo:
        x = _p - x
    return x


_gy = 4 * _inv(5) % _p
_gx = _recupera_x(_gy, 0)
_G = (_gx, _gy, 1, _gx * _gy % _p)


def _comprime(P):
    zi = _inv(P[2])
    x, y = P[0] * zi % _p, P[1] * zi % _p
    return int.to_bytes(y | ((x & 1) << 255), 32, 'little')


def _descomprime(s):
    if len(s) != 32:
        return None
    y = int.from_bytes(s, 'little')
    signo = y >> 255
    y &= (1 << 255) - 1
    x = _recupera_x(y, signo)
    if x is None:
        return None
    return (x, y, 1, x * y % _p)


def _expande(secreto):
    h = hashlib.sha512(secreto).digest()
    a = int.from_bytes(h[:32], 'little')
    a &= (1 << 254) - 8
    a |= (1 << 254)
    return a, h[32:]


def publica(secreto):
    a, _ = _expande(secreto)
    return _comprime(_por(a, _G))


def firma(secreto, mensaje):
    a, prefijo = _expande(secreto)
    A = _comprime(_por(a, _G))
    r = _sha512_modq(prefijo + mensaje)
    Rs = _comprime(_por(r, _G))
    h = _sha512_modq(Rs + A + mensaje)
    return Rs + int.to_bytes((r + h * a) % _q, 32, 'little')


def verifica(clave_publica, mensaje, f):
    """True solo si la firma es buena. Nunca lanza con datos malos: lo que
    llega aqui viene de Internet."""
    try:
        if len(clave_publica) != 32 or len(f) != 64:
            return False
        A = _descomprime(clave_publica)
        R = _descomprime(f[:32])
        if not A or not R:
            return False
        s = int.from_bytes(f[32:], 'little')
        if s >= _q:
            return False
        h = _sha512_modq(f[:32] + clave_publica + mensaje)
        return _iguales(_por(s, _G), _suma(R, _por(h, A)))
    except Exception:
        return False


def id_corto(clave_publica):
    """Lo que se le enseña a una persona: 8 cifras hex de la clave publica.
    Para bloquear o aprobar basta con escribir ese principio."""
    return clave_publica.hex()[:8]


def carga_o_crea(fichero):
    """(secreto, publica). La crea la primera vez, legible solo por su dueño.
    Si se borra, la maquina pasa a ser OTRA identidad: en la red principal
    habra que aprobarla de nuevo."""
    try:
        with open(fichero, 'rb') as f:
            s = f.read().strip()
        secreto = bytes.fromhex(s.decode())
        if len(secreto) != 32:
            raise ValueError
    except FileNotFoundError:
        secreto = os.urandom(32)
        d = os.path.dirname(os.path.abspath(fichero))
        os.makedirs(d, exist_ok=True)
        viejo = os.umask(0o077)
        try:
            with open(fichero, 'w') as f:
                f.write(secreto.hex() + '\n')
        finally:
            os.umask(viejo)
    except ValueError:
        raise SystemExit('%s no es una identidad valida (64 cifras hex)' % fichero)
    return secreto, publica(secreto)


def _prueba_rfc():
    """Vector 1 de la RFC 8032, seccion 7.1."""
    sk = bytes.fromhex('9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60')
    pk = bytes.fromhex('d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a')
    sig = bytes.fromhex('e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155'
                        '5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b')
    return (publica(sk) == pk and firma(sk, b'') == sig and verifica(pk, b'', sig)
            and not verifica(pk, b'x', sig))


if __name__ == '__main__':
    if len(sys.argv) != 2:
        print(__doc__.strip().split('\n')[2])
        sys.exit(1)
    if sys.argv[1] == '--prueba':
        ok = _prueba_rfc()
        print('RFC 8032: %s' % ('OK' if ok else 'FALLO'))
        sys.exit(0 if ok else 1)
    _, pk = carga_o_crea(sys.argv[1])
    print('ID %s  (clave %s)' % (id_corto(pk), pk.hex()))
