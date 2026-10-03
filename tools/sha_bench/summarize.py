"""Summarise a serial capture of the SHA bench (-D SHA_BENCH=2).

    python tools/sha_bench/summarize.py capture.log [sweep.py]

One line per sweep tag, one entry per variant: "x:cycles" when every pass was
clean, otherwise "x:cycles[bad/passes quiet, bad/passes stressed m<modes>]"
with the stress modes (see stress_task in src/sha_bench_var.cpp) that broke it.
"""
import collections
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_variants import HERE, load


def main():
    log = sys.argv[1]
    sweep = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, "sweep_default.py")
    variants = load(sweep)
    res = collections.defaultdict(lambda: {0: [], 1: []})
    modes = collections.defaultdict(set)
    mode = 0
    for line in open(log, errors="replace"):
        m = re.search(r"P pass=(\d+) stress=(\d+)", line)
        if m:
            mode = int(m.group(2))
            continue
        m = re.search(r"V\s+(\d+) (OK |BAD) (\d+\.\d+) A(\d+) B(\d+) C(\d+)", line)
        if m:
            i = int(m.group(1))
            bad = any(int(m.group(k)) for k in (4, 5, 6))
            res[i][1 if mode else 0].append((float(m.group(3)), bad))
            if bad:
                modes[i].add(mode)
    groups = collections.OrderedDict()
    for i, (tag, x, _v) in enumerate(variants):
        if i not in res:
            continue
        quiet, stress = res[i][0], res[i][1]
        cyc = min(c for c, _ in quiet + stress)
        qb, sb = sum(b for _, b in quiet), sum(b for _, b in stress)
        mark = "" if not (qb or sb) else "[%d/%d,%d/%d m%s]" % (
            qb, len(quiet), sb, len(stress), "".join(str(k) for k in sorted(modes[i])))
        groups.setdefault(tag, []).append("%s:%.0f%s" % ("-" if x is None else x, cyc, mark))
    for tag, items in groups.items():
        print("%-10s %s" % (tag, " ".join(items)))
    print("passes: quiet %d, stressed %d" % (
        max((len(v[0]) for v in res.values()), default=0),
        max((len(v[1]) for v in res.values()), default=0)))


if __name__ == "__main__":
    main()
