# net.py - sockets (tools/pythontest.py): python net.py HOST PORT TLSPORT
# fetches /hello.txt over HTTP, then a page over TLS; python net.py serve PORT
# answers one connection in upper case.
# Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
import socket, ssl, sys

def get(s, host):
    s.write(b"GET /hello.txt HTTP/1.0\r\nHost: " + host.encode() + b"\r\n\r\n")
    data = b""
    while True:
        b = s.read(512)
        if not b:
            break
        data += b
    s.close()
    return data

if sys.argv[1] == "serve":
    srv = socket.socket()
    srv.bind(("", int(sys.argv[2])))
    srv.listen(1)
    print("listening")
    c, addr = srv.accept()
    c.send(c.recv(100).upper())
    c.close()
    srv.close()
    print("served", addr[0])
else:
    host = sys.argv[1]
    ai = socket.getaddrinfo(host, int(sys.argv[2]))[0][-1]
    s = socket.socket()
    s.connect(ai)
    head, body = get(s, host).split(b"\r\n\r\n", 1)
    print("http", head.split(b"\r\n")[0], body)
    s = socket.socket()
    s.connect((host, int(sys.argv[3])))
    t = ssl.wrap_socket(s, server_hostname=host, cert_reqs=ssl.CERT_NONE)
    print("https", get(t, host).split(b"\r\n")[0])
