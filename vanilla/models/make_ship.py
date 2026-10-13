#!/usr/bin/env python3
"""Procedural detailed sailing ship for MM7 (OBJ, Y-up as MMEditor expects; game X = obj x, game Y = -obj z, game Z = obj y).
Origin: middle of the ship at waterline level 0. All faces are triangles (MM7 facets must be planar)."""
import math, sys

L, B = 2600.0, 820.0          # length (game X), beam (game Y)
KEEL, DECK, RAIL = -60.0, 430.0, 530.0
NS, NR = 56, 14               # hull sections along X, rings per side (keel -> deck edge)

V, VT, F = [], [], []         # verts, uvs, faces (mtl, [(vi, ti)...])
def v(x, y, z, u=0.0, w=0.0):  # game coords -> obj
    V.append((x, z, -y)); VT.append((u, w)); return len(V)
def tri(m, a, b, c): F.append((m, (a, b, c)))
def quad(m, a, b, c, d): tri(m, a, b, c); tri(m, a, c, d)

def half_beam(t):             # t in [0,1] stern->bow
    s = math.sin(math.pi * min(1.0, t * 1.15)) ** 0.6
    return 0.5 * B * s * (1.0 - 0.35 * max(0.0, t - 0.75) / 0.25)
def sheer(t):                 # deck rises at bow and stern
    return DECK + 160 * (2 * t - 1) ** 6 + 40 * max(0, 0.2 - t) / 0.2

# hull: each section is a U-shaped curve from keel to deck edge, both sides
ring = []
for i in range(NS + 1):
    t = i / NS
    x = -L / 2 + L * t
    hb, top = half_beam(t), sheer(t)
    pts = []
    for side in (-1, 1):
        col = []
        for j in range(NR + 1):
            a = j / NR                                  # 0 keel .. 1 deck edge
            y = side * hb * math.sin(a * math.pi / 2) ** 0.7
            z = KEEL + (top - KEEL) * (1 - math.cos(a * math.pi / 2))
            col.append(v(x, y, z, t * 6, a * 2))
        pts.append(col)
    ring.append(pts)
for i in range(NS):
    for side in (0, 1):
        for j in range(NR):
            a, b = ring[i][side][j], ring[i + 1][side][j]
            c, d = ring[i + 1][side][j + 1], ring[i][side][j + 1]
            if side == 0: quad('hull', a, b, c, d)
            else: quad('hull', a, d, c, b)
# deck (strip between deck edges) and bulwarks
for i in range(NS):
    l0, l1 = ring[i][0][NR], ring[i + 1][0][NR]
    r0, r1 = ring[i][1][NR], ring[i + 1][1][NR]
    quad('deck', l0, r0, r1, l1)
    for side in (0, 1):
        e0, e1 = ring[i][side][NR], ring[i + 1][side][NR]
        x0, y0, z0 = V[e0 - 1][0], -V[e0 - 1][2], V[e0 - 1][1]
        x1, y1, z1 = V[e1 - 1][0], -V[e1 - 1][2], V[e1 - 1][1]
        t0, t1 = v(x0, y0, z0 + (RAIL - DECK)), v(x1, y1, z1 + (RAIL - DECK))
        if side == 0: quad('trim', e0, e1, t1, t0); quad('trim', e0, t0, t1, e1)
        else: quad('trim', e0, t0, t1, e1); quad('trim', e0, e1, t1, t0)
# stern transom
st = ring[0]
for j in range(NR):
    quad('hull', st[0][j], st[0][j + 1], st[1][j + 1], st[1][j])

def cylinder(m, x, y, z0, z1, r0, r1, n=12, h=10):
    rows = []
    for k in range(h + 1):
        z = z0 + (z1 - z0) * k / h; r = r0 + (r1 - r0) * k / h
        rows.append([v(x + r * math.cos(2 * math.pi * s / n), y + r * math.sin(2 * math.pi * s / n), z, s / n, k / h) for s in range(n)])
    for k in range(h):
        for s in range(n):
            quad(m, rows[k][s], rows[k][(s + 1) % n], rows[k + 1][(s + 1) % n], rows[k + 1][s])
def spar(m, x, z, half, r=14, n=8):   # horizontal yard along Y
    a = [v(x + r * math.cos(2 * math.pi * s / n), -half, z + r * math.sin(2 * math.pi * s / n)) for s in range(n)]
    b = [v(x + r * math.cos(2 * math.pi * s / n), half, z + r * math.sin(2 * math.pi * s / n)) for s in range(n)]
    for s in range(n): quad(m, a[s], a[(s + 1) % n], b[(s + 1) % n], b[s])
def sail(x, z0, z1, half, belly=120, nu=10, nv=8):
    g = [[None] * (nu + 1) for _ in range(nv + 1)]
    for r in range(nv + 1):
        for c in range(nu + 1):
            u, w = c / nu, r / nv
            y = -half + 2 * half * u
            z = z1 - (z1 - z0) * w
            dx = belly * math.sin(math.pi * u) * math.sin(math.pi * w)
            g[r][c] = v(x + dx, y, z, u * 2, w * 2)
    for r in range(nv):
        for c in range(nu):
            a, b, cc, d = g[r][c], g[r][c + 1], g[r + 1][c + 1], g[r + 1][c]
            quad('sail', a, b, cc, d); quad('sail', a, d, cc, b)   # double-sided

for mx, top in ((-420.0, 1550.0), (520.0, 1700.0)):
    cylinder('mast', mx, 0, DECK, top, 30, 18)
    for z, half in ((top - 150, 330), (top - 650, 420)):
        spar('mast', mx, z, half)
    sail(mx + 25, top - 620, top - 160, 300)
    sail(mx + 25, top - 1180, top - 680, 390)
# bowsprit
bx = L / 2 - 60
a = [v(bx + 0, 14 * math.cos(2 * math.pi * s / 8), sheer(1) + 14 * math.sin(2 * math.pi * s / 8)) for s in range(8)]
b = [v(bx + 620, 10 * math.cos(2 * math.pi * s / 8), sheer(1) + 260 + 10 * math.sin(2 * math.pi * s / 8)) for s in range(8)]
for s in range(8): quad('mast', a[s], a[(s + 1) % 8], b[(s + 1) % 8], b[s])

TEX = {'hull': 'T1beds', 'deck': 'TRIM2_32a', 'trim': 'TRIM2_32', 'mast': 'TRIM2_32a', 'sail': 'T1beds2'}
out = sys.argv[1] if len(sys.argv) > 1 else 'ship'
with open(out + '.mtl', 'w') as f:
    for m, t in TEX.items(): f.write('newmtl %s\nmap_Kd %s.bmp\n' % (m, t))
with open(out + '.obj', 'w') as f:
    f.write('mtllib %s.mtl\no ship\n' % out.split('/')[-1])
    for p in V: f.write('v %.2f %.2f %.2f\n' % p)
    for t in VT: f.write('vt %.4f %.4f\n' % t)
    cur = None
    for m, idx in F:
        if m != cur: f.write('usemtl %s\n' % m); cur = m
        f.write('f ' + ' '.join('%d/%d' % (k, k) for k in idx) + '\n')
print(len(V), 'verts', len(F), 'triangles')
