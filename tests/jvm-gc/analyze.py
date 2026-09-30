# Summarises tests/jvm-gc/probe.sh output: GC pauses and safepoints, each detector's per-second
# worst detect time, and whether slow seconds line up with GC pauses.
#   python3 analyze.py /tmp/pv-gc-copy.log /tmp/pv-stats.log
import re, sys, datetime, statistics as st
gc, stats = sys.argv[1], sys.argv[2]
pauses, safepoints = [], []
for line in open(gc):
    m = re.match(r"\[([^\]]+)\]\[[\d.]+s\]", line)
    if not m: continue
    t = datetime.datetime.fromisoformat(m.group(1).replace("+0000", "+00:00")).timestamp()
    p = re.search(r"Pause (\S+).* ([\d.]+)ms$", line.strip())
    if p and "GC(" in line: pauses.append((t, p.group(1), float(p.group(2))))
    s = re.search(r'Safepoint "(\w+)".*Total: (\d+) ns', line)
    if s: safepoints.append((t, s.group(1), int(s.group(2)) / 1e6))
dur = None
rows = []
for line in open(stats):
    t = float(line.split()[0])
    m = re.search(r"971 stats (h\d+) .* max ([\d.]+) ms", line)
    if m: rows.append((t, m.group(1), float(m.group(2))))
t0, t1 = rows[0][0], rows[-1][0]
dur = t1 - t0
def pct(v, q): v = sorted(v); return v[min(len(v) - 1, int(q * len(v)))]
print(f"window {dur:.0f} s")
if pauses:
    ms = [p[2] for p in pauses]
    kinds = {}
    for p in pauses: kinds[p[1]] = kinds.get(p[1], 0) + 1
    print(f"GC pauses: {len(pauses)} ({len(pauses)/dur:.2f}/s) {kinds}; median {st.median(ms):.2f} ms, p99 {pct(ms,.99):.2f}, max {max(ms):.2f}")
if safepoints:
    ms = [s[2] for s in safepoints]
    kinds = {}
    for s in safepoints: kinds[s[1]] = kinds.get(s[1], 0) + 1
    print(f"safepoints: {len(safepoints)}, {kinds}; median {st.median(ms):.2f} ms, max {max(ms):.2f}")
per = {}
for t, h, mx in rows: per.setdefault(h, []).append((t, mx))
for h, v in sorted(per.items()):
    m = [x[1] for x in v]
    print(f"{h}: per-second worst detect median {st.median(m):.2f} ms, p95 {pct(m,.95):.2f}, max {max(m):.2f}, seconds over 3 ms: {sum(x>3 for x in m)}/{len(m)}")
# Each stats line covers the second before it. Does a spike second contain a GC pause?
spikes = [(t, h, mx) for t, h, mx in rows if mx > 3]
def gc_in(t): return any(t - 1.0 <= p[0] <= t for p in pauses)
if spikes:
    hit = sum(gc_in(t) for t, _, _ in spikes)
    base = sum(gc_in(t) for t, _, _ in rows)
    print(f"spike seconds with a GC pause in them: {hit}/{len(spikes)} ({100*hit/len(spikes):.0f}%), against {100*base/len(rows):.0f}% of all seconds")
    # Simultaneous: spikes on several detectors within the same 1 s
    secs = {}
    for t, h, mx in spikes: secs.setdefault(int(t), set()).add(h)
    together = sum(1 for s in secs.values() if len(s) >= 2)
    print(f"seconds where 2+ cameras spiked together: {together} of {len(secs)} spike seconds")
