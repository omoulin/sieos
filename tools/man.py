#!/usr/bin/env python3
"""The manual's host side (man/, written in the man(7) subset man(7) tells).

  man.py install SRC DST         pages for the disk: DST/<section>/<name>.<section>
                                 without their comment lines, and DST/whatis (man -k)
  man.py html SRC OUT            the manual as HTML (OUT/index.html, a page each, style.css)
  man.py check SRC ABI NAME...   every program NAME and every SYS_* of ABI (mk/abi.h)
                                 has a page; every option a page shows is in its
                                 program's source (best effort); SEE ALSO points to pages

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
"""
import html, os, re, sys

SECTIONS = {"1": "General Commands", "2": "System Calls", "4": "Servers and Devices",
            "5": "File Formats", "7": "Overviews", "8": "System Administration"}
SPECIAL = {"bu": "•", "em": "—", "mu": "×", "dq": '"', "lq": "“", "rq": "”", "aq": "'"}


def pages(src):
    """[(section, name, path)] of every page under src, in order."""
    out = []
    for sec in sorted(SECTIONS):
        d = os.path.join(src, sec)
        for f in sorted(os.listdir(d)) if os.path.isdir(d) else []:
            if f.endswith("." + sec):
                out.append((sec, f[:-len(sec) - 1], os.path.join(d, f)))
    return out


def plain(s):
    """A line's text without its escapes."""
    s = re.sub(r'\\".*', "", s)
    s = re.sub(r"\\f[BIRP]", "", s)
    s = re.sub(r"\\\((..)", lambda m: SPECIAL.get(m.group(1), "'"), s)
    return s.replace("\\-", "-").replace("\\&", "").replace("\\e", "\\").replace("\\ ", " ").replace("\\\\", "\\")


def macro_text(line):
    """A macro line's arguments as text ('.BR sh (1),' -> 'sh(1),')."""
    name, _, rest = line[1:].partition(" ")
    args = re.findall(r'"([^"]*)"|(\S+)', rest)
    args = [a or b for a, b in args]
    alt = len(name) == 2 and all(c in "BIR" for c in name)
    return ("" if alt else " ").join(args)


def name_line(path):
    """(names, summary) from the page's NAME section."""
    lines, inside = [], False
    for l in open(path, encoding="utf-8"):
        l = l.rstrip("\n")
        if l.startswith(".SH"):
            if inside:
                break
            inside = plain(l[3:]).strip().strip('"') == "NAME"
        elif inside and not l.startswith('.\\"'):
            lines.append(macro_text(l) if l.startswith(".") else l)
    text = " ".join(lines)
    names, _, summary = text.partition("\\-")
    return [plain(n).strip() for n in names.split(",") if n.strip()], plain(summary).strip()


# ---- install
def install(src, dst):
    whatis = []
    for sec, name, path in pages(src):
        os.makedirs(os.path.join(dst, sec), exist_ok=True)
        with open(path, encoding="utf-8") as f:
            body = "".join(l for l in f if not l.startswith('.\\"'))
        with open(os.path.join(dst, sec, f"{name}.{sec}"), "w", encoding="utf-8") as f:
            f.write(body)
        names, summary = name_line(path)
        whatis.append(f"{sec}\t{sec}/{name}.{sec}\t{','.join(names or [name])}\t{summary}\n")
    with open(os.path.join(dst, "whatis"), "w", encoding="utf-8") as f:
        f.write("".join(whatis))


# ---- html
CSS = """body{max-width:46em;margin:2em auto;padding:0 1em;font:16px/1.45 Georgia,serif;color:#222;background:#fdfdf8}
@media (prefers-color-scheme:dark){body{color:#ddd;background:#1b1b1b}a{color:#8cf}}
header{display:flex;justify-content:space-between;font:14px monospace;border-bottom:1px solid #999;margin-bottom:1em}
h2{font:bold 15px sans-serif;letter-spacing:.05em;margin:1.4em 0 .4em}
h3{font:bold 15px sans-serif;margin:1em 0 .3em 1em}
p,dl,pre,.rs{margin:.4em 0 .4em 2.5em}.rs .rs,.rs p,.rs dl,.rs pre,dd p,dd dl,dd pre{margin-left:0}
.rs .rs{margin-left:2.5em}dt{font-weight:normal;margin-top:.5em}dd{margin-left:3em}
pre{font:14px/1.35 monospace;white-space:pre-wrap}b{font-family:monospace;font-size:15px}
table{margin-left:2.5em;border-collapse:collapse}td{padding:.1em 1em .1em 0;vertical-align:top}
"""


def inline(s, font):
    """A line's text into HTML; font: [current] carried from line to line."""
    out, i = [], 0
    def tag(f, on):
        return {"B": ("<b>", "</b>"), "I": ("<i>", "</i>")}.get(f, ("", ""))[0 if on else 1]
    out.append(tag(font[0], True))
    while i < len(s):
        c = s[i]
        if c != "\\":
            out.append(html.escape(c)); i += 1; continue
        n = s[i + 1:i + 2]
        if n == "f" and i + 2 < len(s):
            f = s[i + 2]
            f = font[1] if f == "P" else f if f in "BI" else "R"
            out.append(tag(font[0], False)); font[1], font[0] = font[0], f; out.append(tag(f, True))
            i += 3
        elif n == '"':
            break
        elif n == "(":
            out.append(html.escape(SPECIAL.get(s[i + 2:i + 4], "'"))); i += 4
        elif n == "&":
            i += 2
        elif n == "e":
            out.append("\\"); i += 2
        elif n == " ":
            out.append("&nbsp;"); i += 2
        else:
            out.append(html.escape(n)); i += 2
    out.append(tag(font[0], False))
    return "".join(out)


def macro_html(line, font):
    name, _, rest = line[1:].partition(" ")
    args = [a or b for a, b in re.findall(r'"([^"]*)"|(\S+)', rest)]
    if len(name) == 2:
        return "".join(inline(a, [name[k & 1], "R"]) for k, a in enumerate(args))
    return inline(" ".join(args), [name, "R"])


def to_html(path, sec, name, links):
    font = ["R", "R"]
    body, stack, nf, tp, next1 = [], [], False, 0, None
    title = f"{name.upper()}({sec})"
    def close(upto=("p", "dd", "dl")):
        while stack and stack[-1] in upto:
            body.append(f"</{stack.pop()}>")
    def para():
        if not stack or stack[-1] not in ("p", "dd", "pre"):
            body.append("<p>"); stack.append("p")
    for l in open(path, encoding="utf-8"):
        l = l.rstrip("\n")
        if l.startswith('.\\"'):
            continue
        m = l[1:].split(" ")[0] if l.startswith(".") else None
        if m == "TH":
            a = re.findall(r'"([^"]*)"|(\S+)', l[3:])
            title = f"{(a[0][0] or a[0][1]) if a else name}({sec})"
            continue
        if m in ("SH", "SS"):
            close(("p", "dd", "dl", "div"))
            h = "h2" if m == "SH" else "h3"
            body.append(f"<{h}>{inline(plain(l[3:]).strip().strip(chr(34)), ['R', 'R'])}</{h}>")
            continue
        if m in ("PP", "LP", "P"):
            close(); continue
        if m in ("TP", "IP"):
            close(("p", "dd"))
            if not stack or stack[-1] != "dl":
                body.append("<dl>"); stack.append("dl")
            if m == "TP":
                body.append("<dt>"); tp = 1
            else:
                a = [x or y for x, y in re.findall(r'"([^"]*)"|(\S+)', l[3:])]
                body.append(f"<dt>{inline(a[0], ['R', 'R']) if a else ''}</dt><dd>"); stack.append("dd")
            continue
        if m == "RS":
            body.append('<div class="rs">'); stack.append("div"); continue
        if m == "RE":
            close();
            if stack and stack[-1] == "div":
                body.append("</div>"); stack.pop()
            continue
        if m == "nf":
            para() if stack and stack[-1] == "dd" else close(("p",))
            body.append("<pre>"); stack.append("pre"); nf = True; continue
        if m == "fi":
            if stack and stack[-1] == "pre":
                body.append("</pre>"); stack.pop()
            nf = False; continue
        if m in ("br",):
            body.append("<br>"); continue
        if m == "sp":
            close(("p",)); continue
        if m and re.fullmatch(r"[BIR]|[BIR][BIR]", m) and m != "R":
            if l.strip() == "." + m:
                next1 = m; continue
            text = macro_html(l, font)
        elif m:
            continue                                  # (other macros: ignored)
        elif not l.strip() and not nf:
            close(("p",)); continue
        elif next1:
            text, next1 = inline(l, [next1, "R"]), None
        else:
            text = inline(l, font)
        if tp:
            body.append(text + "</dt><dd>"); stack.append("dd"); tp = 0; continue
        if not nf:
            para()
        body.append(text + ("\n" if nf else ""))
    close(("p", "dd", "dl", "div", "pre"))
    out = " ".join(body).replace("\n </pre>", "</pre>").replace("<pre> ", "<pre>").replace("\n ", "\n")
    def link(mm):
        n, s = html.unescape(re.sub(r"<[^>]+>", "", mm.group(1))), mm.group(2)
        f = links.get((n, s))
        return f'<a href="{f}.html">{mm.group(1)}({s})</a>' if f else mm.group(0)
    out = re.sub(r"(<b>[^<]+</b>|<i>[^<]+</i>)\s?\(([1-8])\)", link, out)
    return (f'<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">'
            f'<title>{html.escape(title)} - SIEOS</title><link rel="stylesheet" href="style.css">'
            f'<header><span>{html.escape(title)}</span><a href="index.html">SIEOS manual</a><span>{html.escape(title)}</span></header>\n'
            f'{out}\n</html>\n')


def build_html(src, dst):
    os.makedirs(dst, exist_ok=True)
    all_, links = pages(src), {}
    for sec, name, path in all_:
        for n in name_line(path)[0] + [name]:
            links.setdefault((n, sec), f"{name}.{sec}")
    idx = []
    for sec, title in SECTIONS.items():
        rows = []
        for s, name, path in all_:
            if s != sec:
                continue
            with open(os.path.join(dst, f"{name}.{sec}.html"), "w", encoding="utf-8") as f:
                f.write(to_html(path, sec, name, links))
            names, summary = name_line(path)
            rows.append(f'<tr><td><a href="{name}.{sec}.html">{html.escape(", ".join(names or [name]))}</a></td><td>{html.escape(summary)}</td></tr>')
        idx.append(f'<h2 id="s{sec}">{sec} &middot; {title}</h2>\n<table>\n' + "\n".join(rows) + "\n</table>")
    with open(os.path.join(dst, "style.css"), "w") as f:
        f.write(CSS)
    with open(os.path.join(dst, "index.html"), "w", encoding="utf-8") as f:
        f.write('<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">'
                '<title>SIEOS manual</title><link rel="stylesheet" href="style.css">'
                '<header><span>SIEOS</span><span>The SIEOS manual</span><span>SIEOS</span></header>\n'
                f'<p>{len(all_)} pages. Start with <a href="intro.7.html">intro(7)</a>.</p>\n' + "\n".join(idx) + "\n</html>\n")
    print(f"man-html: {len(all_)} pages in {dst}/index.html")


# ---- check
def sources(src_root, sec, name, path):
    """The source files whose code parses the page's options."""
    paths = []
    for l in open(path, encoding="utf-8"):
        m = re.match(r'\.\\"\s*source:\s*(.*)', l)
        if m:
            paths += m.group(1).split()
    if not paths and os.path.isdir(os.path.join(src_root, "user", name)):
        paths = [f"user/{name}"]
    files = []
    for p in paths:
        p = os.path.join(src_root, p)
        if os.path.isdir(p):
            files += [os.path.join(p, f) for f in sorted(os.listdir(p)) if f.endswith((".c", ".h", ".py"))]
        elif os.path.isfile(p):
            files.append(p)
    return files


def options(path):
    """The options a page shows: in its SYNOPSIS, and the tags of .TP/.IP."""
    opts, sect, tag = set(), "", False
    for l in open(path, encoding="utf-8"):
        l = l.rstrip("\n")
        if l.startswith('.\\"'):
            continue
        if l.startswith(".SH"):
            sect = plain(l[3:]).strip().strip('"'); continue
        if l.startswith(".TP"):
            tag = True; continue
        text = l if tag or sect == "SYNOPSIS" else l[3:] if l.startswith(".IP") else None
        tag = False
        if text is None:
            continue
        text = text.replace("\\fI", " ")            # (-D\fIname\fR: the option is -D)
        if text.startswith("."):
            name, _, rest = text[1:].partition(" ")
            args = [a or b for a, b in re.findall(r'"([^"]*)"|(\S+)', rest)]
            text = " ".join(a for k, a in enumerate(args) if not (len(name) == 2 and name[k & 1] == "I"))
        t = plain(text)
        for o in re.findall(r"(?<![\w/.\-])(--?[A-Za-z][\w-]*)", t):
            opts.add(o)
    return opts


def check(src, abi, names):
    root = os.getcwd()                                # (the sources: where make runs)
    bad, all_ = [], pages(src)
    have = {}
    for sec, name, path in all_:
        names_, summary = name_line(path)
        if not summary:
            bad.append(f"{path}: no NAME line ('name \\- summary')")
        for n in names_ + [name]:
            have.setdefault(n, []).append(sec)
    for n in names:
        if n not in have:
            bad.append(f"no page for the program {n}")
    nl2 = " ".join(" ".join(name_line(p)[0]) + " " + name_line(p)[1] for s, _, p in all_ if s == "2")
    for sysn in re.findall(r"^\s*(SYS_[A-Z0-9_]+)", open(abi).read(), re.M):
        if not re.search(r"\b%s\b" % sysn, nl2):
            bad.append(f"no page in section 2 names {sysn}")
    for sec, name, path in all_:
        text = open(path, encoding="utf-8").read()
        for n, s in re.findall(r"^\.BR\s+(\S+)\s+\(([1-8])\)", text, re.M):
            n = plain(n)
            if s not in have.get(n, []):
                bad.append(f"{path}: SEE ALSO {n}({s}): no such page")
        if sec not in "18":
            continue
        files = sources(root, sec, name, path)
        opts = options(path)
        if opts and not files:
            bad.append(f"{path}: options {' '.join(sorted(opts))} but no source to check them in")
            continue
        code = "".join(open(f, encoding="utf-8", errors="replace").read() for f in files)
        for o in sorted(opts):
            letter = o[1:] if len(o) == 2 else None
            if re.search(r"(?<![\w-])%s(?![\w-])" % re.escape(o), code):
                continue
            if letter and re.search(r"(case\s*|==\s*)'%s'" % re.escape(letter), code):
                continue
            if '"%s"' % o.lstrip("-") in code:             # (kill -USR1: a table of names)
                continue
            if o[1] != "-" and len(o) > 2 and all(re.search(r"(case\s*|==\s*)'%s'|(?<![\w-])-%s(?![\w-])" % (c, c), code) for c in o[1:]):
                continue                                  # ([-nksBir]: each letter)
            bad.append(f"{path}: option {o} is not in {', '.join(os.path.relpath(f, root) for f in files[:3])}{' ...' if len(files) > 3 else ''}")
    if bad:
        print("man-check: FAIL\n  " + "\n  ".join(bad))
        return 1
    count = {s: sum(1 for x in all_ if x[0] == s) for s in SECTIONS}
    print(f"man-check: ok, {len(all_)} pages ({', '.join(f'{s}: {n}' for s, n in count.items())}), "
          f"{len(set(names))} programs, every SYS_* documented")
    return 0


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "install" and len(sys.argv) == 4:
        install(sys.argv[2], sys.argv[3])
    elif cmd == "html" and len(sys.argv) == 4:
        build_html(sys.argv[2], sys.argv[3])
    elif cmd == "check" and len(sys.argv) >= 4:
        sys.exit(check(sys.argv[2], sys.argv[3], sys.argv[4:]))
    else:
        sys.exit(__doc__)
