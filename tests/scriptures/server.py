"""
tests/scriptures/server.py - A stand-in for churchofjesuschrist.org's content
API, for make scriptures-test (tools/scripturestest.py): over HTTPS with a
certificate from a test CA that start() adds to the disk's /etc/ssl/roots.

  GET /study/api/v3/language-pages/type/dynamic?lang=eng&uri=/scriptures/VOL
      a volume's contents (a book of one or two chapters)
  GET /study/api/v3/language-pages/type/content?lang=eng&uri=/scriptures/VOL/BOOK/N
      a chapter: its HTML (verses, a footnote) and footnotes
  anything else: 404

The five standard works, each tiny: Genesis 1-2 and John 1 (King James
Version, public domain: a few verses) and made-up pages for the others.
start() -> (the port, the list of requests seen).

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
"""
import http.server, json, os, ssl, subprocess, tempfile, threading, urllib.parse

VERSES = {
    "/scriptures/ot/gen/1": ("Genesis 1", "The First Book of Moses Called Genesis", [
        "In the {beginning} God created the heaven and the earth.",
        "And the earth was without form, and void; and darkness was upon the face of the deep. And the Spirit of God moved upon the face of the waters.",
        "And God said, Let there be light: and there was light.",
        "And God saw the light, that it was good: and God divided the light from the darkness.",
        "And God called the light Day, and the darkness he called Night. And the evening and the morning were the first day."]),
    "/scriptures/ot/gen/2": ("Genesis 2", "The First Book of Moses Called Genesis", [
        "Thus the heavens and the earth were finished, and all the host of them.",
        "And on the seventh day God ended his work which he had made; and he rested on the seventh day from all his work which he had made.",
        "And God blessed the seventh day, and sanctified it: because that in it he had rested from all his work which God created and made."]),
    "/scriptures/nt/john/1": ("John 1", "The Gospel According to St John", [
        "In the beginning was the Word, and the Word was with God, and the Word was God.",
        "The same was in the beginning with God.",
        "All things were made by him; and without him was not any thing made that was made.",
        "In him was life; and the life was the light of men.",
        "And the light shineth in darkness; and the darkness comprehended it not."]),
    "/scriptures/bofm/test/1": ("Test 1", "A Test Book", [
        "This page stands in for the Book of Mormon in the test library.",
        "Its words are made up, so that the test searches something known."]),
    "/scriptures/dc-testament/dc/1": ("Section 1", "A Test Section", [
        "This page stands in for the Doctrine and Covenants.",
        "Searching for lantern finds this verse."]),
    "/scriptures/pgp/test/1": ("Test 1", "A Test Book", [
        "This page stands in for the Pearl of Great Price."]),
}
VOLUMES = {"ot": "Old Testament", "nt": "New Testament", "bofm": "Book of Mormon",
           "dc-testament": "Doctrine and Covenants", "pgp": "Pearl of Great Price"}

def toc(vol):
    books = {}
    for uri, (title, book, _) in VERSES.items():
        if uri.split("/")[2] == vol:
            books.setdefault(book, []).append((uri, title))
    h = '<nav class="manifest"><ul class="doc-map">'
    for book, chs in books.items():
        h += f'<li><h2 class="label"><p class="title">{book}</p></h2><ul class="doc-map">'
        for uri, title in chs:
            h += f'<li><a class="list-tile" href="/study{uri}?lang=eng"><p class="title">{title}</p></a></li>'
        h += "</ul></li>"
    return h + "</ul></nav>"

def chapter(uri):
    title, book, verses = VERSES[uri]
    num = uri.rsplit("/", 1)[1]
    body = (f'<header><h1 data-aid="100" id="title1">{book}</h1>'
            f'<p class="title-number" data-aid="101" id="title_number1">Chapter {num}</p>'
            f'<p class="study-summary" data-aid="102" id="study_summary1">A test page of {title}.</p></header><div class="body-block">')
    notes = {}
    for i, v in enumerate(verses, 1):
        if "{" in v:                          # a footnote on the word in braces
            w = v[v.index("{") + 1:v.index("}")]
            nid = f"note{i}_a"
            v = v.replace("{" + w + "}", f'<a class="study-note-ref" href="#{nid}"><sup class="marker" data-value="a"></sup>{w}</a>')
            notes[nid] = {"id": nid, "marker": f"{i}a", "pid": str(200 + i), "context": w,
                          "text": f'<p data-aid="9{i}" id="{nid}_p1"><a class="scripture-ref" href="/study/scriptures/nt/john/1?lang=eng&amp;id=p1#p1">John 1:1</a></p>'}
        body += f'<p class="verse" data-aid="{200 + i}" id="p{i}"><span class="verse-number">{i} </span>{v}</p>'
    body += "</div>"
    return {"meta": {"title": title}, "content": {"body": body, "footnotes": notes}}

def start(image, siefs):
    d = tempfile.mkdtemp()
    def ossl(*a): subprocess.run(["openssl", *a], cwd=d, check=True, capture_output=True)
    ossl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", "ca.key", "-out", "ca.pem", "-days", "30",
         "-subj", "/CN=SIEOS Scriptures test CA", "-addext", "basicConstraints=critical,CA:TRUE")
    ossl("req", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256", "-nodes", "-keyout", "leaf.key",
         "-out", "leaf.csr", "-subj", "/CN=test")
    open(os.path.join(d, "ext"), "w").write("subjectAltName=IP:10.0.2.2\n")
    ossl("x509", "-req", "-in", "leaf.csr", "-CA", "ca.pem", "-CAkey", "ca.key", "-CAcreateserial",
         "-out", "leaf.pem", "-days", "30", "-extfile", "ext")
    ca = subprocess.run(["openssl", "x509", "-in", "ca.pem", "-outform", "DER"], cwd=d, capture_output=True, check=True).stdout
    roots = subprocess.run([siefs, image, "cat", "/etc/ssl/roots"], capture_output=True).stdout
    open(os.path.join(d, "roots"), "wb").write(roots + ca)
    subprocess.run([siefs, image, "put", os.path.join(d, "roots"), "/etc/ssl/roots"], check=True)
    seen = []

    class H(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"
        def log_message(self, *a): pass
        def do_GET(self):
            u = urllib.parse.urlparse(self.path)
            qs = urllib.parse.parse_qs(u.query)
            uri = qs.get("uri", [""])[0]
            seen.append((u.path, uri, self.headers.get("User-Agent", "")))
            obj = None
            if u.path.endswith("/type/dynamic") and uri.startswith("/scriptures/") and uri[12:] in VOLUMES:
                obj = {"content": {"meta": {"title": VOLUMES[uri[12:]]}, "content": {"body": toc(uri[12:])}}}
            elif u.path.endswith("/type/content") and uri in VERSES:
                obj = chapter(uri)
            b = json.dumps(obj if obj else {"error": "not found"}).encode()
            self.send_response(200 if obj else 404)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(b)))
            self.end_headers()
            self.wfile.write(b)

    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
    c = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    c.load_cert_chain(os.path.join(d, "leaf.pem"), os.path.join(d, "leaf.key"))
    srv.socket = c.wrap_socket(srv.socket, server_side=True)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv.server_address[1], seen
