# words.py - an example for /bin/python: lines, words and the most common
# words of text files.
#   python /src/python/words.py /etc/motd
# Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
import sys

counts = {}
for name in sys.argv[1:]:
    lines = words = 0
    with open(name) as f:
        for line in f:
            lines += 1
            for w in line.lower().split():
                w = w.strip(".,;:!?()[]\"'")
                if w:
                    words += 1
                    counts[w] = counts.get(w, 0) + 1
    print(f"{name}: {lines} lines, {words} words")
top = sorted(counts.items(), key=lambda kv: -kv[1])[:5]
print("most common:", ", ".join(f"{w} ({n})" for w, n in top))
