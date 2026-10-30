"""frames_to_ms.py RUN_DIR ROUTE.csv OUT.csv
Turns a frame-indexed route into a time-indexed one (SWR_TEST_SCRIPT_MS) using a run of that route at the original
timing: each row's guest frame -> milliseconds since the replay clock started ("test input clock" in the log), from the
run's presents.csv. With faster loading the game then reaches each screen early and waits for the input there."""
import csv, re, sys, bisect
run, route, out = sys.argv[1:4]
start = int(re.search(r'test input clock: start_ns=(\d+)', open(run + '/Metroid Dread.log', errors='ignore').read()).group(1))
rows = list(csv.DictReader(open(run + '/presents.csv')))
cnt = [int(r['guest_present_count']) for r in rows]
ns = [int(r['completed_ns']) for r in rows]
with open(out, 'w') as o:
    for line in open(route):
        if not line.strip() or line.startswith('#'):
            continue
        p = line.strip().split(',')
        i = min(bisect.bisect_left(cnt, int(p[0])), len(ns) - 1)
        p[0] = str(max(0, (ns[i] - start) // 1000000))
        o.write(','.join(p) + '\n')
print('last row at %s ms' % p[0])
