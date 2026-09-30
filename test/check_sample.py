#!/usr/bin/env python3
"""Play testdata/ recordings through ubersdr-hdradio and check what comes out.

  check_sample.py <binary> <samples.txt> [name...]

Each recording (16-bit stereo IQ WAV) is fed to the binary's stdin as raw
samples, exactly as UberSDR's wrapper feeds it, with --input-sample-rate taken
from the WAV header, at up to PACE times real time. Status is collected from
fd 3 and commands are given on fd 4, so the side channels are exercised too.
A command of the form "wait <text>" waits for that text on stderr before the
next is sent. See samples.txt for the format.
"""
import fcntl
import json
import os
import struct
import subprocess
import sys
import tempfile
import threading
import time

here = os.path.dirname(os.path.abspath(__file__))
testdata = os.path.join(os.path.dirname(here), "testdata")
OUT_RATE = 48000
# Input is fed at up to this many times real time.
PACE = 20


def read_wav(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise SystemExit(f"{path}: not a WAV file")
    pos, rate, pcm = 12, None, None
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4], struct.unpack("<I", data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        if cid == b"fmt ":
            fmt, ch, rate, _, _, bits = struct.unpack("<HHIIHH", body[:16])
            if fmt != 1 or ch != 2 or bits != 16:
                raise SystemExit(f"{path}: want 16-bit stereo PCM, got format {fmt}, {ch} ch, {bits} bit")
        elif cid == b"data":
            pcm = body
        pos += 8 + size + (size & 1)
    if rate is None or pcm is None:
        raise SystemExit(f"{path}: no fmt or data chunk")
    return rate, pcm


def run(binary, name, rec, program, control, min_audio, max_audio, expected):
    rate, pcm = read_wav(os.path.join(testdata, rec))
    commands = [] if control == "-" else [c.strip() for c in control.split(";")]
    ctl_r, ctl_w = os.pipe()
    with tempfile.TemporaryFile() as out, tempfile.TemporaryFile() as status:
        sfd = status.fileno()

        def side_channels():
            # Out of the way first: either may already sit on 3 or 4, and the
            # first dup2 would otherwise close the other.
            s = fcntl.fcntl(sfd, fcntl.F_DUPFD, 10)
            c = fcntl.fcntl(ctl_r, fcntl.F_DUPFD, 10)
            os.dup2(s, 3)
            os.dup2(c, 4)

        proc = subprocess.Popen(
            [binary, "--input-sample-rate", str(rate), "--output-sample-rate", str(OUT_RATE),
             "--program", program],
            stdin=subprocess.PIPE, stdout=out, stderr=subprocess.PIPE, preexec_fn=side_channels,
            # Kept open through the close that follows preexec_fn.
            pass_fds=(3, 4))
        os.close(ctl_r)

        log = []
        log_changed = threading.Condition()

        def read_log():
            for raw in proc.stderr:
                with log_changed:
                    log.append(raw.decode("utf-8", "replace").rstrip("\n"))
                    log_changed.notify_all()

        def feed():
            # At most PACE times real time, so a command sent after something
            # shows up in the log lands while there is input left to act on.
            step = int(rate * 0.1) * 4
            try:
                for off in range(0, len(pcm), step):
                    proc.stdin.write(pcm[off:off + step])
                    proc.stdin.flush()
                    time.sleep(0.1 / PACE)
                proc.stdin.close()
            except BrokenPipeError:
                pass

        control_errors = []

        def drive():
            with os.fdopen(ctl_w, "w") as ctl:
                for c in commands:
                    if c.startswith("wait "):
                        want = c[5:]
                        with log_changed:
                            if not log_changed.wait_for(lambda: any(want in l for l in log), timeout=120):
                                control_errors.append(f"never saw '{want}' in the log to act on")
                                return
                    else:
                        ctl.write(c + "\n")
                        ctl.flush()

        threads = [threading.Thread(target=f) for f in (read_log, feed, drive)]
        for t in threads:
            t.start()
        proc.wait(timeout=600)
        for t in threads:
            t.join()
        out.seek(0)
        audio = out.read()
        status.seek(0)
        lines = status.read().decode("utf-8", "replace").splitlines()
    log = "\n".join(log)

    problems = []
    if proc.returncode != 0:
        problems.append(f"exit status {proc.returncode}")
    for i, line in enumerate(lines):
        try:
            json.loads(line)
        except ValueError as e:
            problems.append(f"status line {i + 1} is not JSON ({e}): {line[:120]}")
            break
    problems += control_errors
    text = "\n".join(lines)
    missing = [e for e in expected if not e.startswith("log:") and e not in text]
    if missing:
        problems.append("status never showed: " + ", ".join(missing))
    # log: expectations must appear in stderr in the order given.
    pos = 0
    for e in expected:
        if not e.startswith("log:"):
            continue
        at = log.find(e[4:], pos)
        if at < 0:
            problems.append(f"stderr did not show '{e[4:]}' (in order, after the ones before it)")
            break
        pos = at + len(e) - 4

    seconds = len(audio) / (4 * OUT_RATE)
    if len(audio) % 4:
        problems.append(f"{len(audio)} bytes of audio is not whole stereo frames")
    if seconds < min_audio:
        problems.append(f"{seconds:.1f} s of audio, want at least {min_audio:g}")
    if max_audio is not None and seconds > max_audio:
        problems.append(f"{seconds:.1f} s of audio, want at most {max_audio:g}")
    if audio:
        n = len(audio) // 2
        samples = struct.unpack(f"<{n}h", audio[:2 * n])
        rms = (sum(s * s for s in samples) / n) ** 0.5
        if rms < 30:
            problems.append(f"audio is near silent (RMS {rms:.1f})")
    else:
        rms = 0

    result = "ok" if not problems else "FAIL"
    print(f"{name:32s} {rate:6d} Hz in: {seconds:5.1f} s audio, RMS {rms:6.0f}, {len(lines):3d} status lines  {result}")
    for p in problems:
        print(f"    {p}")
    if problems:
        print("    stderr:\n" + "".join(f"      {l}\n" for l in log.splitlines()))
        if lines:
            print(f"    last status: {lines[-1]}")
    return not problems


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    binary, table, only = sys.argv[1], sys.argv[2], set(sys.argv[3:])
    ok = True
    ran = 0
    with open(table) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            name, rec, program, control, min_a, max_a, *expected = line.split("|")
            if only and name not in only:
                continue
            ran += 1
            ok &= run(binary, name, rec, program, control, float(min_a),
                      None if max_a == "-" else float(max_a), expected)
    if ran == 0:
        raise SystemExit("no samples matched")
    print("PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


main()
