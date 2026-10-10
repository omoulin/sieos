#!/bin/sh
# run.sh - The engine's tests (make llm-test). TOOLS = build/llm.
#  1. kernels: every number format, plain C vs AVX2 vs floats
#  2. fuzz: damaged model files are refused, under the memory checkers
#  3. the reference: logits and greedy decoding vs tests/llm/ref.py (numpy)
#  4. the tokenizer vs the model's published one (if available)
#  5. chats: several turns, budget refusal, a full context
# Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
set -e
T=$1; B=$(dirname "$T"); M=models; PY=$B/llm-conda/bin/python
S135=$M/SmolLM2-135M-Instruct-Q8_0.gguf; K135=$M/SmolLM2-135M-Instruct-Q4_K_M.gguf; S360=$M/smollm2-360m-instruct-q8_0.gguf
fail=0
CF="-std=gnu11 -O2 -Wall -Wextra -fno-math-errno -Iinclude"
echo "== kernels"
gcc $CF -o $T/kernels tests/llm/kernels.c $T/quant.o $T/math.o -lm && $T/kernels | tail -1 || fail=1
echo "== fuzz (address and undefined-behaviour checkers)"
mkdir -p $T/asan
gcc $CF -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -o $T/asan/fuzz tests/llm/fuzz.c llm/gguf.c llm/quant.c llm/model.c llm/tok.c llm/chat.c llm/math.c lib/string.c 2>/dev/null
$T/asan/fuzz $S135 200 || fail=1
P='<|im_start|>user
Write one sentence about the sea.<|im_end|>
<|im_start|>assistant
'
if [ -x "$PY" ]; then
    for m in $S135 $K135 $S360; do
        echo "== reference: $(basename $m)"
        TMPDIR_LLM=$T $PY tests/llm/compare.py $T $PY $m "$P" 8 || fail=1
    done
    if [ -f $B/llm-ref/tokenizer.json ] && $PY -c 'import tokenizers' 2>/dev/null; then
        echo "== tokenizer"; $PY tests/llm/tokcheck.py $T $S135 $B/llm-ref/tokenizer.json || fail=1
    fi
else echo "(no $PY: reference checks skipped; see docs/llm.md)"; fi
echo "== chats"
out=$(printf 'My name is Ada.\nWhat is my name?\n\n' | $T/llm-run -m $S360 -i -t 8 -n 40 --temp 0 -s "You are a helpful assistant." 2>/dev/null)
echo "$out" | grep -q Ada && echo "  remembers across turns: ok" || { echo "  remembers across turns: FAIL"; echo "$out"; fail=1; }
$T/llm-run -m $S360 --budget 300 -p hi 2>&1 | grep -q "budget" && echo "  over-budget model refused: ok" || { echo "  budget: FAIL"; fail=1; }
out=$(printf 'Tell me a long story about a dragon.\nAnd another one, about a knight.\nOne more, about a ship.\n\n' | $T/llm-run -m $S135 -i -t 8 -n 100 --ctx 160 2>&1)
[ $(echo "$out" | grep -c 'answer') -eq 3 ] && echo "  full context: starts again, keeps answering: ok" || { echo "  full context: FAIL"; fail=1; }
[ $fail = 0 ] && echo "llm tests: all passed" || { echo "llm tests: FAILED"; exit 1; }
