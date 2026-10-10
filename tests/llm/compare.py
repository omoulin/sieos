#!/usr/bin/env python3
"""compare.py - The engine against the reference (ref.py) on one model:
logits of the last prompt token (fast kernels and --exact), and the tokens
chosen by greedy decoding. Exit status 1 if a check fails.

  compare.py TOOLDIR PYTHON model.gguf "prompt" [steps]

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
"""
import subprocess, sys, os, numpy as np
tools, py, model, prompt = sys.argv[1:5]
steps = int(sys.argv[5]) if len(sys.argv) > 5 else 8
tmp = os.environ.get('TMPDIR_LLM', '/tmp')
run = lambda *a: subprocess.run([tools + '/llm-run', '-m', model, '-t', '8'] + list(a), capture_output=True, text=True, check=True).stdout
ids = [int(x) for x in run('--tokens', prompt).split()]
ref_path = tmp + '/ref.bin'
subprocess.run([py, os.path.dirname(__file__) + '/ref.py', model, 'logits', ref_path] + [str(i) for i in ids], check=True)
ref = np.fromfile(ref_path, np.float32)
ok = True
for mode in ([], ['--exact'], ['--kernel', 'scalar']):
    run(*mode, '--logits', prompt, tmp + '/eng.bin')
    eng = np.fromfile(tmp + '/eng.bin', np.float32)
    err = np.abs(eng - ref).max() / ref.std()
    corr = np.corrcoef(eng, ref)[0, 1]
    top5 = len(set(np.argsort(-eng)[:5]) & set(np.argsort(-ref)[:5]))
    # exact mode: float everywhere, must match closely; fast kernels quantize
    # activations to 8 bits, as is standard: judged by correlation and ranking
    good = (err < 0.02 if mode == ['--exact'] else corr > 0.995) and eng.argmax() == ref.argmax() and top5 >= 4
    ok &= good
    print('  %-16s correlation %.5f, max error %.3f of the spread, top-1 %s, top-5 %d/5  %s' %
          (' '.join(mode) or 'fast kernels', corr, err, 'same' if eng.argmax() == ref.argmax() else 'DIFFERENT', top5, 'ok' if good else 'FAIL'))
# greedy decoding: the reference recomputes the whole sequence at each step
eng_seq = [int(x) for x in run('--greedy', str(steps), prompt).split()]
seq = list(ids); ref_seq = []
for k in range(steps):
    subprocess.run([py, os.path.dirname(__file__) + '/ref.py', model, 'logits', ref_path] + [str(i) for i in seq], check=True)
    t = int(np.fromfile(ref_path, np.float32).argmax()); ref_seq.append(t); seq.append(t)
same = sum(1 for a, b in zip(eng_seq, ref_seq) if a == b)
print('  greedy %d steps: %d identical  %s' % (steps, same, 'ok' if eng_seq == ref_seq else 'differs: engine %s, reference %s' % (eng_seq, ref_seq)))
ok &= same >= steps - 1                 # a near-tie may flip one late token
sys.exit(0 if ok else 1)
