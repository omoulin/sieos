#!/bin/sh
# bench.sh - Speed of every model in models/, by threads and kernels (make llm-bench).
# Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
T=$1
for m in models/*.gguf; do
    echo "== $(basename $m)"
    for t in 1 4 8 16; do $T/llm-run -m $m --bench -t $t 2>/dev/null; done
    $T/llm-run -m $m --bench -t 16 --kernel avx2 2>/dev/null
    $T/llm-run -m $m --bench -t 16 --kernel scalar 2>/dev/null
done
