#!/usr/bin/env python3
"""Ask a UT server what it is running, the way a server browser does.

UT answers GameSpy-style queries on its query port (game port + 1, 7778 by
default): `\\basic\\`, `\\info\\`, `\\status\\`. A recompiled server that
answers with its map and game type is running UnrealScript (UdpServerQuery is
script) and IpDrv's sockets, not merely alive.

    py -3 tools/query.py                 # 127.0.0.1:7778
    py -3 tools/query.py 192.168.1.20 7778
Exit status 1 when nothing answers.
"""
import socket
import sys


def query(host='127.0.0.1', port=7778, what=b'\\info\\', timeout=3.0):
    """The server's reply to one query as {key: value}, or None."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    s.sendto(what, (host, port))
    text = b''
    try:
        while b'\\final\\' not in text:
            text += s.recvfrom(4096)[0]
    except (socket.timeout, ConnectionResetError):   # reset: Windows' word for port unreachable
        if not text:
            return None
    parts = text.decode('latin1').split('\\')[1:]
    return dict(zip(parts[0::2], parts[1::2]))


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else '127.0.0.1'
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 7778
    info = query(host, port)
    if not info:
        print('no answer from %s:%d' % (host, port))
        return 1
    for k in ('hostname', 'mapname', 'gametype', 'numplayers', 'maxplayers', 'gamever'):
        if k in info:
            print('%-11s %s' % (k, info[k]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
