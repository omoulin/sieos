#!/usr/bin/env python3
# Copyright (C) 2026 Olivier Moulin
# Part of SIEOS, released under the GNU General Public License version 3
# (GPL-3.0); see the LICENSE file.
"""
mklocales.py SRC OUT - SIEOS's locale database, from glibc's locale sources.

SRC is a directory of glibc locale definitions (/usr/share/i18n/locales on a
Debian build host: the "locales" package; their data carries no copyright
claim).  For each locale (fr_FR, de_DE, ja_JP, sr_RS@latin, ...) OUT/NAME is
written: a gettext catalog (.mo), which SIEOS's C library maps when a program
sets that locale (libc/port/src/locale/sieos64).  Its keys:

    @1.I        nl_langinfo(LC_NUMERIC item I)   RADIXCHAR, THOUSEP
    @2.I        nl_langinfo(LC_TIME item I)      days, months, AM/PM, formats,
                                                 eras and alternative digits
                                                 (lists: semicolon-separated)
    @4.15       nl_langinfo(CRNCYSTR)
    @5.I        nl_langinfo(LC_MESSAGES item I)  YESEXPR, NOEXPR, YESSTR, NOSTR
    @lc.FIELD   localeconv(): struct lconv's fields (numbers in decimal, -1
                for CHAR_MAX; groupings as their bytes)
    @collate    1: strcoll and strxfrm order as Unicode does (letters by
                their base letter, then accents, then case)

The locales are UTF-8 ones: fr_FR, fr_FR.UTF-8 and fr_FR.utf8 name the same.
"""
import os
import re
import struct
import sys

NAME_RX = re.compile(r'^[a-z]{2,3}(_[A-Z]{2})?(@[a-z0-9]+)?$')

TIME_LISTS = [('abday', 7), ('day', 7), ('abmon', 12), ('mon', 12)]
TIME_ITEMS = {40: 'd_t_fmt', 41: 'd_fmt', 42: 't_fmt', 43: 't_fmt_ampm', 46: 'era_d_fmt',
              48: 'era_d_t_fmt', 49: 'era_t_fmt'}
LCONV_STR = [('decimal_point', 'LC_NUMERIC'), ('thousands_sep', 'LC_NUMERIC'),
             ('int_curr_symbol', 'LC_MONETARY'), ('currency_symbol', 'LC_MONETARY'),
             ('mon_decimal_point', 'LC_MONETARY'), ('mon_thousands_sep', 'LC_MONETARY'),
             ('positive_sign', 'LC_MONETARY'), ('negative_sign', 'LC_MONETARY')]
LCONV_GROUP = [('grouping', 'LC_NUMERIC'), ('mon_grouping', 'LC_MONETARY')]
LCONV_NUM = ['int_frac_digits', 'frac_digits', 'p_cs_precedes', 'p_sep_by_space', 'n_cs_precedes',
             'n_sep_by_space', 'p_sign_posn', 'n_sign_posn', 'int_p_cs_precedes', 'int_p_sep_by_space',
             'int_n_cs_precedes', 'int_n_sep_by_space', 'int_p_sign_posn', 'int_n_sign_posn']
CATS = ('LC_NUMERIC', 'LC_MONETARY', 'LC_TIME', 'LC_MESSAGES')


class Source:
    """The locale definitions in a directory, parsed as they are needed."""

    def __init__(self, src):
        self.src = src
        self.cache = {}

    def parse(self, name):
        if name in self.cache:
            return self.cache[name]
        self.cache[name] = {}                     # (a copy cycle ends here)
        path = os.path.join(self.src, name)
        if not os.path.isfile(path):
            return {}
        comment, escape = '%', '\\'
        lines, cur = [], ''
        for raw in open(path, encoding='utf-8', errors='replace'):
            line = raw.rstrip('\n')
            m = re.match(r'^\s*(comment_char|escape_char)\s+(\S)\s*$', line)
            if m:
                if m.group(1) == 'comment_char':
                    comment = m.group(2)
                else:
                    escape = m.group(2)
                continue
            if not cur and line.lstrip().startswith(comment):
                continue
            if line.endswith(escape) and not line.endswith(escape * 2):
                cur += line[:-1]
                continue
            lines.append(cur + line)
            cur = ''
        cats, cat = {}, None
        for line in lines:
            s = line.strip()
            if not s:
                continue
            if cat is None:
                if s in CATS:
                    cat = s
                    cats[cat] = {}
                continue
            if s == 'END ' + cat:
                cat = None
                continue
            kw, rest = (re.split(r'\s+', s, maxsplit=1) + [''])[:2]
            if kw.startswith(comment):
                continue
            cats[cat][kw] = values(rest.strip(), escape)
        self.cache[name] = cats
        return cats

    def category(self, name, cat, depth=0):
        """A category's keywords, with what it copies from other locales."""
        d = self.parse(name).get(cat, {})
        out = {}
        if 'copy' in d and depth < 8:
            out.update(self.category(d['copy'][0], cat, depth + 1))
        out.update({k: v for k, v in d.items() if k != 'copy'})
        return out


def decode(s, escape):
    """A string's characters: <Uxxxx> code points and escaped ones."""
    out, i = [], 0
    while i < len(s):
        m = re.match(r'<U([0-9A-Fa-f]+)>', s[i:])
        if m:
            out.append(chr(int(m.group(1), 16)))
            i += len(m.group(0))
        elif s[i] == escape and i + 1 < len(s):
            out.append(s[i + 1])
            i += 2
        else:
            out.append(s[i])
            i += 1
    return ''.join(out)


def values(rest, escape):
    """A keyword's values: strings (quoted) or numbers, separated by semicolons."""
    vals, i = [], 0
    while i < len(rest):
        c = rest[i]
        if c == '"':
            j, buf = i + 1, []
            while j < len(rest) and rest[j] != '"':
                if rest[j] == escape and j + 1 < len(rest):
                    buf.append(rest[j:j + 2])
                    j += 2
                    continue
                buf.append(rest[j])
                j += 1
            vals.append(decode(''.join(buf), escape))
            i = j + 1
        elif c in '; \t':
            i += 1
        else:
            j = i
            while j < len(rest) and rest[j] not in '; \t':
                j += 1
            tok = rest[i:j]
            vals.append(int(tok) if re.match(r'^-?\d+$', tok) else decode(tok, escape))
            i = j
    return vals


def first(d, kw, default=''):
    v = d.get(kw)
    return v[0] if v else default


def grouping(v):
    """glibc's grouping list (3;3, -1) as struct lconv's bytes."""
    out = []
    for n in v or []:
        if not isinstance(n, int):
            continue
        if n < 0:
            out.append(127)                       # CHAR_MAX: no more grouping
            break
        if n == 0:
            break
        out.append(min(n, 126))
    return ''.join(chr(n) for n in out)


def entries(src, name):
    num = src.category(name, 'LC_NUMERIC')
    mon = src.category(name, 'LC_MONETARY')
    tim = src.category(name, 'LC_TIME')
    msg = src.category(name, 'LC_MESSAGES')
    if not (num or tim):
        return None
    e = {'@collate': '1'}
    e['@1.0'] = str(first(num, 'decimal_point', '.'))
    e['@1.1'] = str(first(num, 'thousands_sep'))
    idx = 0
    for kw, n in TIME_LISTS:
        v = [str(x) for x in tim.get(kw, [])]
        if len(v) == n:
            for k in range(n):
                e['@2.%d' % (idx + k)] = v[k]
        idx += n
    ampm = [str(x) for x in tim.get('am_pm', [])]
    if len(ampm) == 2:
        e['@2.38'], e['@2.39'] = ampm
    for i, kw in TIME_ITEMS.items():
        if kw in tim:
            e['@2.%d' % i] = str(first(tim, kw))
    if tim.get('era'):
        e['@2.44'] = ';'.join(str(x) for x in tim['era'])
    if tim.get('alt_digits'):
        e['@2.47'] = ';'.join(str(x) for x in tim['alt_digits'])
    sym = str(first(mon, 'currency_symbol'))
    if sym:
        e['@4.15'] = ('-' if first(mon, 'p_cs_precedes', 1) == 1 else '+') + sym
    for i, kw in enumerate(('yesexpr', 'noexpr', 'yesstr', 'nostr')):
        if kw in msg:
            e['@5.%d' % i] = str(first(msg, kw))
    for f, cat in LCONV_STR:
        d = num if cat == 'LC_NUMERIC' else mon
        e['@lc.' + f] = str(first(d, f, '.' if f == 'decimal_point' else ''))
    for f, cat in LCONV_GROUP:
        d = num if cat == 'LC_NUMERIC' else mon
        e['@lc.' + f] = grouping(d.get(f))
    for f in LCONV_NUM:
        v = first(mon, f, -1)
        e['@lc.' + f] = str(v if isinstance(v, int) else -1)
    return e


def write_mo(path, e):
    keys = sorted(e, key=lambda k: k.encode())
    ids = [k.encode() for k in keys]
    strs = [e[k].encode() for k in keys]
    n = len(keys)
    o = 28
    t = o + 8 * n
    data = t + 8 * n
    blob, otab, ttab = b'', [], []
    for s in ids:
        otab.append((len(s), data + len(blob)))
        blob += s + b'\0'
    for s in strs:
        ttab.append((len(s), data + len(blob)))
        blob += s + b'\0'
    with open(path, 'wb') as f:
        f.write(struct.pack('<7I', 0x950412de, 0, n, o, t, 0, data))
        for ln, off in otab + ttab:
            f.write(struct.pack('<2I', ln, off))
        f.write(blob)


def main():
    src, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    s = Source(src)
    n = 0
    for name in sorted(os.listdir(src)):
        if not NAME_RX.match(name):
            continue
        e = entries(s, name)
        if e is None:
            continue
        write_mo(os.path.join(out, name), e)
        n += 1
    print('mklocales: %d locales' % n)


if __name__ == '__main__':
    main()
