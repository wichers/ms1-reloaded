#!/usr/bin/env python3
"""Run every scenario of tools/scenarios against the original with ../ms1-emu's difftest.

Each scenario's '# run[ LABEL]: OPTIONS' lines are its runs: difftest options, with ORIGINAL
standing for the original's image and other *.txt words for scenario files. Per run:

- the function test: difftest with the scenario, every listed function compared call by call;
- the whole-firmware comparison: the rebuild run as the whole machine (difftest -W) with the
  same options, its text lines, decoder events and CAN frames compared with the original's,
  times left out and numbers with a fractional part masked (timing-derived values), decoder
  events per decoder with a run of speed events in one direction as its last, CAN frames per
  direction and ID with a frame repeated unchanged counted once;
- the call times of each side (difftest -y).
These runs take the rebuild with the original's application version at 0xF704 (INFO AP:, CAN
identification; tools/mkimage.py --version-from), the rest as built.

'# expect[ LABEL]: OPTIONS' lines are runs of behaviour the original does not have (DCC): the
rebuild, as built, runs alone (difftest -W) and what it shows is compared with the file
tools/scenarios/LABEL.expected (LABEL defaults to the scenario's name; version 4010 takes
LABEL.4010.expected where that exists). --update writes those files from the rebuild instead.

Then: totals, the functions no run calls, and the functions where the rebuild spends the
most extra time. Exit status 1 if a call differs or a comparison fails. Skipped
calls are those of functions flagged pN in the function table that got a string of the
original's read-only data, which the rebuild keeps elsewhere (difftest).

--release 4010 tests the version 4010 build (build/4010, `make RELEASE=4010`) against the
release 4010 image (--firmware), made bootable with the device dump's bootloader by
tools/mkimage.py --original; the 4011 original is ms1-emu's build/ms1_4011.s19.

usage: python3 tools/regress.py [--release R] [-j JOBS] [-n CALLS] [--emu DIR] [--firmware DIR]
                                [--keep DIR] [--update] [RUN...]
"""
import argparse
import concurrent.futures
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))   # releases.py, also under python3 -I
import releases

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SCENARIOS = os.path.join(HERE, "scenarios")
CAN_TX_RING = 0x1929          # 4011's address (tools/releases.py maps it), 10 frames
DUMP = "firmware_20260317_192757_usbdm.s19"


def runs():
    """(name, scenario path, option words, expect) for every run and expect line, in file
    order"""
    out = []
    for f in sorted(os.listdir(SCENARIOS)):
        if not f.endswith(".txt"):
            continue
        path = os.path.join(SCENARIOS, f)
        for line in open(path, encoding="utf-8"):
            m = re.match(r"#\s*(run|expect)(?:\s+(\w+))?:\s*(.*)", line)
            if m:
                out.append((m.group(2) or f[:-4], path, shlex.split(m.group(3)), m.group(1) == "expect"))
    return out


def expected_path(name, release):
    """the expected observables of an expect run"""
    own = os.path.join(SCENARIOS, f"{name}.{release}.expected")
    return own if os.path.exists(own) else os.path.join(SCENARIOS, f"{name}.expected")


def resolve(words, orig):
    return [orig if w == "ORIGINAL" else os.path.join(SCENARIOS, w) if w.endswith(".txt") else w for w in words]


def table(text):
    """{function: (calls, direct, tested, match, differ, skipped)} from a difftest report."""
    rows, on = {}, False
    for line in text.splitlines():
        if line.startswith("function "):
            on = True
            continue
        if not on or line.startswith("total"):
            if line.startswith("total"):
                on = False
            continue
        w = line.split()
        if len(w) >= 7 and re.fullmatch(r"[0-9A-F]{4}", w[1] or ""):
            rows[w[0]] = (int(w[3]), "called directly" in line, int(w[4]), int(w[5]), int(w[6]), int(w[7]))
    return rows


def times(text):
    """{function: (calls timed, original us, rebuild us)} from the -y section."""
    rows, on = {}, False
    for line in text.splitlines():
        if line.split()[:3] == ["function", "timed", "orig"]:
            on = True
            continue
        w = line.split()
        if on and len(w) == 5:
            rows[w[0]] = (int(w[1]), float(w[2]), float(w[3]))
    return rows


EVENT = re.compile(r"^\s*\d+\.\d+ ((?:mm|mm1|mfx|dcc|dccacc):\S+ .*)$")


def observables(text, can_log):
    """What a scenario shows, without times: text lines and the final LCD, then the decoder
    events grouped by decoder, a run of speed events in one direction as its last, then the
    CAN frames grouped by direction and ID, a frame repeated unchanged counted once. What
    timing decides is left out: the order of events of different decoders and of frames of
    different IDs, the speed steps a ramp passes, how often a state repeats (such as the mfx
    search's status frame after each search that finds no decoder)."""
    out, events = [], []
    for line in text.splitlines():
        m = EVENT.match(line)
        if m:
            events.append(re.sub(r"\d+\.\d+", "#", m.group(1)))
        elif line.startswith("t="):
            out.append("lcd " + line.split(" ", 1)[1])
        elif re.match(r"^(original|rebuild) at t=", line):
            out.append("end LCD " + line.rsplit("LCD ", 1)[1])
    events.sort(key=lambda e: e.split(" ", 1)[0])    # stable: per-decoder order stays
    # a run of a decoder's speed events in one direction counts as its last: the steps on the
    # way depend on when the knob is read
    def ramp(e):
        w = e.split(" ")
        return w[:2] + [w[2][:1]] if len(w) > 2 and w[1] == "speed" else None
    out += [e for i, e in enumerate(events)
            if not (ramp(e) and i + 1 < len(events) and ramp(events[i + 1]) == ramp(e))]
    frames = []
    for line in open(can_log, encoding="utf-8", errors="replace"):
        w = line.split()
        if len(w) >= 3:
            frames.append(w[1:])
    frames.sort(key=lambda f: (f[0], f[1]))         # stable: per-ID order stays
    out += ["can " + " ".join(f) for i, f in enumerate(frames) if not i or f != frames[i - 1]]
    return out


def run(cmd):
    p = subprocess.run(cmd, capture_output=True, text=True)
    return p.stdout + p.stderr


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("-j", type=int, default=os.cpu_count() or 4, help="parallel difftest runs")
    ap.add_argument("-n", type=int, default=100000, help="calls tested per function and run")
    ap.add_argument("--release", default=releases.DEFAULT, choices=sorted(releases.RELEASES),
                    help="the application version tested (default %(default)s)")
    ap.add_argument("--emu", default=os.path.join(ROOT, "..", "ms1-emu"), help="the ms1-emu checkout")
    ap.add_argument("--firmware", default=os.path.join(ROOT, "..", "original-firmware"),
                    help="the original firmware: release images and the device dump (for --release 4010)")
    ap.add_argument("--keep", help="keep the images, logs and reports in this directory")
    ap.add_argument("--update", action="store_true", help="write the expect runs' files from the rebuild")
    ap.add_argument("only", nargs="*", help="run names (default: all)")
    a = ap.parse_args()

    rel = releases.get(a.release)
    emu = os.path.abspath(a.emu)
    difftest = os.path.join(emu, "build", "difftest")
    if rel.name == "4011":
        base = os.path.join(emu, "build", "ms1_4011.s19")
    else:
        base = os.path.join(os.path.abspath(a.firmware), rel.originals[0] + ".bin")
    dump = os.path.join(os.path.abspath(a.firmware), DUMP)
    app = os.path.join(rel.build, "app.s19")
    page3c = os.path.join(rel.build, "page3c.bin")
    ident = os.path.join(rel.build, "ident", rel.name + ".s19")      # its first release's
    for need in (difftest, base, app, page3c, ident) + ((dump,) if rel.name != "4011" else ()):
        if not os.path.exists(need):
            sys.exit(f"missing {need}: build ms1-emu (make build/ms1emu build/difftest, the image) and this firmware")
    work = a.keep or tempfile.mkdtemp(prefix="regress.")
    os.makedirs(work, exist_ok=True)
    orig, rebuild = os.path.join(work, "orig.s19"), os.path.join(work, "rebuild.s19")
    compared = os.path.join(work, "rebuild_compared.s19")
    py = [sys.executable, "-I"]
    mkimage = py + [os.path.join(HERE, "mkimage.py"), "--release", rel.name]
    if rel.name == "4011":
        shutil.copy(base, orig)
    else:
        subprocess.run(mkimage + ["--original", base, dump, orig], check=True, capture_output=True)
    subprocess.run(mkimage + ["--bootloader", orig, "--s19", rebuild, app, page3c, ident], check=True,
                   capture_output=True)
    # compared with the original: with its version (mkimage.py --version-from)
    subprocess.run(mkimage + ["--bootloader", orig, "--s19", compared, "--version-from", orig, app, page3c,
                              ident], check=True, capture_output=True)
    sym, tsv = os.path.join(work, "ms1.sym"), os.path.join(work, "functions.tsv")
    shutil.copy(os.path.join(rel.build, "ms1.sym"), sym)
    shutil.copy(rel.functions, tsv)
    can_ring = ["-r", f"{rel.ram(CAN_TX_RING):04X}:10"]

    todo = [r for r in runs() if not a.only or r[0] in a.only]
    if not todo:
        sys.exit("no such run")
    jobs = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.j) as pool:
        for name, path, words, expect in todo:
            opts = resolve(words, orig)
            files = [orig, rebuild if expect else compared, sym, tsv]
            log_o, log_r = os.path.join(work, name + ".can.orig"), os.path.join(work, name + ".can.rebuild")
            jobs[name] = (
                None if expect else
                pool.submit(run, [difftest, "-y", "-n", str(a.n)] + can_ring + ["-s", path, "-c", log_o] + opts + files),
                pool.submit(run, [difftest, "-W"] + can_ring + ["-s", path, "-c", log_r] + opts + files),
                log_o, log_r)
        results = {name: (t.result() if t else None, w.result(), lo, lr) for name, (t, w, lo, lr) in jobs.items()}

    failed = False
    calls, timing = {}, {}
    print(f"{'run':<14} {'match':>8} {'differ':>7} {'skipped':>7}  whole firmware")
    for name, _, _, expect in todo:
        test, whole, log_o, log_r = results[name]
        if a.keep:
            if test is not None:
                open(os.path.join(work, name + ".test.txt"), "w").write(test)
            open(os.path.join(work, name + ".whole.txt"), "w").write(whole)
        if expect:
            got = observables(whole, log_r)
            path = expected_path(name, rel.name)
            if a.update:
                open(path, "w", encoding="utf-8").write("".join(line + "\n" for line in got))
                verdict = f"written ({len(got)} lines, {os.path.relpath(path, ROOT)})"
            elif not os.path.exists(path):
                failed = True
                verdict = f"no {os.path.relpath(path, ROOT)} (--update writes it)"
            else:
                want = open(path, encoding="utf-8").read().splitlines()
                if want == got:
                    verdict = f"as expected ({len(got)} lines)"
                else:
                    failed = True
                    k = next((i for i, (x, y) in enumerate(zip(want, got)) if x != y), min(len(want), len(got)))
                    verdict = (f"DIFFERS at line {k + 1} of {len(want)}/{len(got)}:\n"
                               f"    expected: {want[k] if k < len(want) else '(end)'}\n"
                               f"    rebuild:  {got[k] if k < len(got) else '(end)'}")
            print(f"{name:<14} {'-':>8} {'-':>7} {'-':>7}  {verdict}")
            continue
        t = table(test)
        ok = sum(r[3] for r in t.values())
        bad = sum(r[4] for r in t.values())
        skipped = sum(r[5] for r in t.values())
        if not t:
            print(f"{name:<14} difftest failed:\n{test[-2000:]}")
            failed = True
            continue
        for fn, r in t.items():
            c = calls.setdefault(fn, [0, False])
            c[0] += r[0]
            c[1] |= r[1]
        for fn, (n, uo, ur) in times(test).items():
            s = timing.setdefault(fn, [0, 0.0, 0.0])
            s[0] += n
            s[1] += n * uo
            s[2] += n * ur
        so, sr = observables(test, log_o), observables(whole, log_r)
        if so == sr:
            verdict = f"same ({len(so)} lines)"
        else:
            failed = True
            k = next((i for i, (x, y) in enumerate(zip(so, sr)) if x != y), min(len(so), len(sr)))
            verdict = (f"DIFFERS at line {k + 1} of {len(so)}/{len(sr)}:\n"
                       f"    original: {so[k] if k < len(so) else '(end)'}\n    rebuild:  {sr[k] if k < len(sr) else '(end)'}")
        if bad:
            failed = True
        print(f"{name:<14} {ok:>8} {bad:>7} {skipped:>7}  {verdict}")
        block = False
        for line in test.splitlines() if bad else []:
            if re.match(r"^\S+\s+call \d+ at t=", line):
                block = True
            elif not line.startswith("  "):
                block = False
            if block:
                print("    " + line)

    never = [fn for fn, (c, d) in calls.items() if c == 0 and not d]
    direct = [fn for fn, (c, d) in calls.items() if c == 0 and d]
    print(f"\n{len(calls)} functions; called directly only: {', '.join(direct) or '-'}")
    print(f"never called: {', '.join(never) or '-'}")
    extra = sorted(((s[2] - s[1], fn, s) for fn, s in timing.items() if s[0]), reverse=True)
    tot_o = sum(s[1] for s in timing.values())
    tot_r = sum(s[2] for s in timing.values())
    if tot_o:
        print(f"\ntime in tested calls: original {tot_o / 1e6:.3f} s, rebuild {tot_r / 1e6:.3f} s ({tot_r / tot_o:.2f}x)")
        print(f"{'most extra time':<36} {'calls':>7} {'orig us':>9} {'rebuild us':>10} {'ratio':>6}")
        for d, fn, (n, so_, sr_) in extra[:12]:
            print(f"{fn:<36} {n:>7} {so_ / n:>9.1f} {sr_ / n:>10.1f} {sr_ / so_:>6.2f}")
    if not a.keep:
        shutil.rmtree(work)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
