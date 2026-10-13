# Realign 2016 OpenEnroth address map to the local mm7.exe using Ghidra function starts.
# ponytail: piecewise-constant delta via neighbour voting; assumes same function order in both builds.
import bisect, collections, sys
starts = sorted(int(l, 16) for l in open('ghidra_funcs.txt'))
sset = set(starts)
rows = [l.rstrip('\n').split('\t') for l in open('mm7_addr_map.tsv', encoding='latin-1')]
rows = sorted((int(r[0], 16), r[1], r[2]) for r in rows)
R = 0x3000
cands = []
for a, _, _ in rows:
    i = bisect.bisect_left(starts, a - R)
    ds = set()
    while i < len(starts) and starts[i] <= a + R:
        ds.add(starts[i] - a); i += 1
    cands.append(ds)
W = 12
out = []
for k, (a, f, sig) in enumerate(rows):
    votes = collections.Counter()
    for j in range(max(0, k - W), min(len(rows), k + W + 1)):
        votes.update(cands[j])
    if not votes: out.append((a, None, 0, f, sig)); continue
    d, n = votes.most_common(1)[0]
    hit = d in cands[k]
    out.append((a, a + d if hit else None, n, f, sig))
ok = [o for o in out if o[1] is not None and o[2] >= 4]
with open('mm7_addr_map_local.tsv', 'w', encoding='latin-1') as fo:
    for a, b, n, f, sig in out:
        fo.write('0x%08X\t%s\t%d\t%s\t%s\n' % (a, '0x%08X' % b if b and n >= 4 else '?', n, f, sig))
print('mapped with confidence>=4:', len(ok), 'of', len(rows))
