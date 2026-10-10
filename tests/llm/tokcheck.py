#!/usr/bin/env python3
"""tokcheck.py - The engine's tokenizer against the model's published one
(the `tokenizers` library, used here as a test reference only): token ids for
many kinds of text, then decoding back to the exact same bytes.

  tokcheck.py llm-info-dir model.gguf tokenizer.json

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
"""
import subprocess, sys
from tokenizers import Tokenizer
tools, model, tj = sys.argv[1:4]
ref = Tokenizer.from_file(tj)
texts = [
    "Hello world", "Hello, world! How are you?", "  two leading spaces", "trailing spaces   ",
    "line one\nline two\n\nline four", "tabs\tand\tmore\t\ttabs", "numbers 12345 and 3.14159 and 1,000,000",
    "a 5 b  6 c\n7", "It's, they're, I've, we'll, she'd, I'm", "CamelCaseWords and snake_case_words",
    "code: for (int i = 0; i < n; i++) { x[i] += 1; }", "emoji 😀 and accents: café, naïve, Ærø",
    "Ελληνικά, русский, 中文, 日本語, 한국어", "symbols: ~!@#$%^&*()_+-=[]{}|;':\",./<>?",
    "URL https://example.org/path?q=1&b=2", "   ", "\n\n\n", "a\r\nb", "mixed  \n  spacing\t\n",
    "The quick brown fox jumps over the lazy dog. " * 3, "x" * 300, "1234567890" * 5,
]
bad = 0
for t in texts:
    want = ref.encode(t, add_special_tokens=False).ids
    out = subprocess.run([tools + '/llm-run', '-m', model, '--tokens', t, '-t', '1'], capture_output=True, text=True).stdout.split()
    got = [int(x) for x in out]
    if got != want:
        bad += 1
        print('DIFFERENT for %r\n  engine %s\n  ref    %s' % (t[:60], got[:20], want[:20]))
print('tokenizer: %d of %d texts identical' % (len(texts) - bad, len(texts)))
sys.exit(1 if bad else 0)
