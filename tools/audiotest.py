#!/usr/bin/env python3
"""Sound (docs/audio.md) on QEMU's Intel HDA controller and its hda-duplex
codec, whose output QEMU writes to a WAV file: as alice on the serial
console, `vol`, then `play` of a 1 kHz tone, which must come out in QEMU's
file as a 1 kHz tone; `vol mute` and a second play, which must add nothing
louder than silence; `rec` for 2 s from the codec's input, which must give
a WAV file of exactly that length.
Q35=1: the q35 machine and its ich9-intel-hda instead.
Usage: audiotest.py <cpus> <host tools> <disk image> <qemu command line...> (make audio-test)

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
"""
import array, math, os, select, shutil, subprocess, sys, time, wave
import qserial
SLOW = float(os.environ.get("SIEOS_SLOW", "1"))

cpus, host, image, cmdline = int(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4:]
d = os.path.dirname(os.path.abspath(image))
tone, captured, recorded = d + "/tone.wav", d + "/audio-out.wav", d + "/audio-rec.wav"
with wave.open(tone, "wb") as w:                  # 1 s of 1 kHz, both channels, half of full scale
    w.setnchannels(2); w.setsampwidth(2); w.setframerate(48000)
    w.writeframes(array.array("h", (int(16384 * math.sin(2 * math.pi * 1000 * i / 48000)) for i in range(48000) for _ in (0, 1))).tobytes())
subprocess.run([host + "/siefs", image, "put", tone, "/tmp/t/tone.wav"], check=True)
subprocess.run([host + "/siefs", image, "chmod", "644", "/tmp/t/tone.wav"], check=True)
if os.path.exists(captured): os.remove(captured)
out, fails, qemu = b"", 0, None
import atexit
atexit.register(lambda: qemu and qemu.poll() is None and qemu.kill())

def boot(audiodev):
    global qemu
    hda = ["-M", "q35", "-device", "ich9-intel-hda"] if os.environ.get("Q35") else ["-device", "intel-hda"]
    qemu = subprocess.Popen(cmdline + ["-audiodev", audiodev] + hda + ["-device", "hda-duplex,audiodev=snd"],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    expect(f"mk: {cpus} CPU")

def expect(text, timeout=30):
    timeout *= SLOW
    global out
    end = time.time() + timeout
    while text.encode() not in out:
        left = end - time.time()
        if left <= 0 or qemu.poll() is not None:
            qemu.kill()
            sys.exit(f"FAIL: waiting for {text!r}\n--- output ---\n{out.decode(errors='replace')[-3000:]}")
        if select.select([qemu.stdout], [], [], left)[0]:
            out += qemu.stdout.read1(65536)
    i = out.index(text.encode())
    before, out = out[:i], out[i + len(text):]
    return before.decode(errors="replace")

def run(cmd, timeout=60):
    qserial.type_line(qemu, cmd)
    return expect("$ ", timeout)

def ok(name, cond, detail=""):
    global fails
    print(f"{'ok' if cond else 'FAIL'}: {name}{(' (' + detail + ')') if detail else ''}")
    fails += not cond

def login():
    expect("login: "); qserial.type_line(qemu, "alice")
    expect("Password: "); qserial.type_line(qemu, "alicepass1")
    expect("alice@sieos:"); expect("$ ")

def stop():                                       # (QEMU ends its WAV file properly on SIGTERM)
    run("sync")
    qemu.terminate(); qemu.wait()

def power(x, hz):                                 # Goertzel: the strength of one frequency
    k, s1, s2 = 2 * math.cos(2 * math.pi * hz / 48000), 0.0, 0.0
    for v in x: s1, s2 = v + k * s1 - s2, s1
    return (s1 * s1 + s2 * s2 - k * s1 * s2) / max(len(x), 1) ** 2

# 1: output to QEMU's WAV file (its "wav" audio backend has no input)
boot("wav,id=snd,out.frequency=48000,path=" + captured)   # (QEMU would resample to 44.1 kHz)
expect("first start")
for p in ("New password", "Again: "): expect(p); qserial.type_line(qemu, "rootpass1")
expect("user name"); qserial.type_line(qemu, "alice")
expect("full name"); qserial.type_line(qemu, "Alice")
for p in ("New password", "Again: "): expect(p); qserial.type_line(qemu, "alicepass1")
login()
r = run("vol")
ok("vol: the server starts, finds the codec", "volume 80%" in r and "audiod: codec" in r, r.strip()[-300:])
r = run("vol 100")
ok("vol 100", "volume 100%" in r, r.strip()[-200:])
r = run("play /tmp/t/tone.wav")
ok("play: 1 s of tone accepted", "play:" not in r.replace("play /tmp", ""), r.strip()[-200:])
time.sleep(1)                                     # (the last 683 ms of it are still in the ring)
r = run("vol mute")
ok("vol mute", "volume 100%, muted" in r, r.strip()[-200:])
run("play /tmp/t/tone.wav")
time.sleep(1)
r = run("vol 50")
ok("vol 50 unmutes", "volume 50%" in r and "muted" not in r, r.strip()[-200:])
stop()
with wave.open(captured) as w: x = array.array("h", w.readframes(w.getnframes()))[0::2]
loud = [i for i, v in enumerate(x) if abs(v) > 2000]
seg, rest = (x[loud[0]:loud[-1] + 1], x[loud[-1] + 1:]) if loud else ([], x)
f1k, f3k = power(seg, 1000), power(seg, 3000)
ok("QEMU's output: 1 s of a 1 kHz tone, unbroken", abs(len(seg) - 48000) < 100 and f1k > 1e6 * max(f3k, 1e-9) and
   max(seg) > 16000, f"{len(seg)} loud samples, peak {max(seg) if seg else 0}, 1 kHz/3 kHz power {f1k:.0f}/{f3k:.4f}")
ok("muted: the second play is silent", len(rest) > 44000 and max(map(abs, rest), default=0) < 100,
   f"{len(rest)} samples after the tone, peak {max(map(abs, rest), default=0)}")

# 2: recording (from QEMU's "none" backend: silence, in real time)
boot("none,id=snd,in.frequency=48000")
login()
t0 = time.time()
r = run("rec /tmp/t/r.wav 2", 30)
dt = time.time() - t0
ok("rec 2 s", "rec:" not in r.replace("rec /tmp", "") and 1.5 < dt < 10 * SLOW, f"{dt:.1f} s; {r.strip()[-200:]}")
stop()
subprocess.run([host + "/siefs", image, "get", "/tmp/t/r.wav", recorded], check=False)
try:
    with wave.open(recorded) as w:
        ok("the recording: a WAV file of 2 s, 48 kHz, 16-bit, stereo", w.getnframes() == 96000 and w.getframerate() == 48000 and
           w.getnchannels() == 2 and w.getsampwidth() == 2 and os.path.getsize(recorded) == 44 + 96000 * 4,
           f"{w.getnframes()} frames, {os.path.getsize(recorded)} bytes")
except Exception as e:
    ok("the recording: a WAV file", False, str(e))
print("PASS" if not fails else f"FAIL: {fails} check(s)")
sys.exit(1 if fails else 0)
