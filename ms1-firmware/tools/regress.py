#!/usr/bin/env python3
"""Run every scenario of tools/scenarios against the original with ../ms1-emu's difftest.

Each scenario's '# run[ LABEL]: OPTIONS' lines are its runs: difftest options, with ORIGINAL
standing for the original's image and other *.txt words for scenario files. Per run:

- the function test: difftest with the scenario, every listed function compared call by call;
- the whole-firmware comparison: the rebuild run as the whole machine (difftest -W) with the
  same options, its text lines, decoder events and CAN frames compared with the original's,
  times left out and numbers with a fractional part masked (timing-derived values);
- the call times of each side (difftest -y).

Then: totals, the functions no run calls, and the functions where the rebuild spends the
most extra time. Exit status 1 if a call differs or a whole-firmware comparison fails.

usage: python3 tools/regress.py [-j JOBS] [-n CALLS] [--emu DIR] [--keep DIR] [RUN...]
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

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SCENARIOS = os.path.join(HERE, "scenarios")
CAN_RING = ["-r", "1929:10"]


def runs():
    """(name, scenario path, option words) for every run line, in file order."""
    out = []
    for f in sorted(os.listdir(SCENARIOS)):
        if not f.endswith(".txt"):
            continue
        path = os.path.join(SCENARIOS, f)
        for line in open(path, encoding="utf-8"):
            m = re.match(r"#\s*run(?:\s+(\w+))?:\s*(.*)", line)
            if m:
                out.append((m.group(1) or f[:-4], path, shlex.split(m.group(2))))
    return out


def resolve(words, orig):
    return [orig if w == "ORIGINAL" else os.path.join(SCENARIOS, w) if w.endswith(".txt") else w for w in words]


def table(text):
    """{function: (calls, direct, tested, match, differ)} from a difftest report."""
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
            rows[w[0]] = (int(w[3]), "called directly" in line, int(w[4]), int(w[5]), int(w[6]))
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


EVENT = re.compile(r"^\s*\d+\.\d+ ((?:mm|mm1|mfx):\S+ .*)$")


def observables(text, can_log):
    """What a scenario shows, without times: text lines, decoder events, the final LCD, then the
    CAN frames grouped by direction and ID (in order within a group: frames of different IDs
    interleave by timing)."""
    out = []
    for line in text.splitlines():
        m = EVENT.match(line)
        if m:
            out.append(re.sub(r"\d+\.\d+", "#", m.group(1)))
        elif line.startswith("t="):
            out.append("lcd " + line.split(" ", 1)[1])
        elif re.match(r"^(original|rebuild) at t=", line):
            out.append("end LCD " + line.rsplit("LCD ", 1)[1])
    frames = []
    for line in open(can_log, encoding="utf-8", errors="replace"):
        w = line.split()
        if len(w) >= 3:
            frames.append(w[1:])
    frames.sort(key=lambda f: (f[0], f[1]))         # stable: per-ID order stays
    out += ["can " + " ".join(f) for f in frames]
    return out


def run(cmd):
    p = subprocess.run(cmd, capture_output=True, text=True)
    return p.stdout + p.stderr


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("-j", type=int, default=os.cpu_count() or 4, help="parallel difftest runs")
    ap.add_argument("-n", type=int, default=100000, help="calls tested per function and run")
    ap.add_argument("--emu", default=os.path.join(ROOT, "..", "ms1-emu"), help="the ms1-emu checkout")
    ap.add_argument("--keep", help="keep the images, logs and reports in this directory")
    ap.add_argument("only", nargs="*", help="run names (default: all)")
    a = ap.parse_args()

    emu = os.path.abspath(a.emu)
    difftest = os.path.join(emu, "build", "difftest")
    base = os.path.join(emu, "build", "ms1_4011.s19")
    for need in (difftest, base, os.path.join(ROOT, "build", "app.s19")):
        if not os.path.exists(need):
            sys.exit(f"missing {need}: build ms1-emu (make build/ms1emu build/difftest, the image) and this firmware")
    work = a.keep or tempfile.mkdtemp(prefix="regress.")
    os.makedirs(work, exist_ok=True)
    orig, rebuild = os.path.join(work, "orig.s19"), os.path.join(work, "rebuild.s19")
    py = [sys.executable, "-I"]
    subprocess.run(py + [os.path.join(HERE, "mkimage.py"), os.path.join(ROOT, "build", "app.s19"), base,
                         rebuild], check=True, capture_output=True)
    shutil.copy(base, orig)
    sym, tsv = os.path.join(work, "ms1.sym"), os.path.join(work, "functions.tsv")
    shutil.copy(os.path.join(ROOT, "build", "ms1.sym"), sym)
    shutil.copy(os.path.join(HERE, "functions.tsv"), tsv)

    todo = [r for r in runs() if not a.only or r[0] in a.only]
    if not todo:
        sys.exit("no such run")
    jobs = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.j) as pool:
        for name, path, words in todo:
            opts = resolve(words, orig)
            files = [orig, rebuild, sym, tsv]
            log_o, log_r = os.path.join(work, name + ".can.orig"), os.path.join(work, name + ".can.rebuild")
            jobs[name] = (
                pool.submit(run, [difftest, "-y", "-n", str(a.n)] + CAN_RING + ["-s", path, "-c", log_o] + opts + files),
                pool.submit(run, [difftest, "-W"] + CAN_RING + ["-s", path, "-c", log_r] + opts + files),
                log_o, log_r)
        results = {name: (t.result(), w.result(), lo, lr) for name, (t, w, lo, lr) in jobs.items()}

    failed = False
    calls, timing = {}, {}
    print(f"{'run':<14} {'match':>8} {'differ':>7}  whole firmware")
    for name, _, _ in todo:
        test, whole, log_o, log_r = results[name]
        if a.keep:
            open(os.path.join(work, name + ".test.txt"), "w").write(test)
            open(os.path.join(work, name + ".whole.txt"), "w").write(whole)
        t = table(test)
        ok = sum(r[3] for r in t.values())
        bad = sum(r[4] for r in t.values())
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
        print(f"{name:<14} {ok:>8} {bad:>7}  {verdict}")
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
