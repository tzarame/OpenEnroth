#!/usr/bin/env python3
"""Read live MM7 state from a running mm7.exe (Wine/Proton) via /proc/<pid>/mem.
Addresses from GrayFace's MMExtension (Scripts/Structs/00 structs.lua), MM7 column.
Needs kernel.yama.ptrace_scope=0 (or root). Usage: mm7peek.py [interval_sec]"""
import os, struct, sys, time

PARTY = 0xACD4EC   # i4 X, Y, Z, Direction(0-2047, 0=E, 512=N), LookAngle
HOUR, MINUTE = 0xACD554, 0xACD558
TIME = 0xACCE64    # i8 game ticks

def find_pid():
    for p in os.listdir('/proc'):
        if not p.isdigit(): continue
        try: cmd = open(f'/proc/{p}/cmdline', 'rb').read().lower()
        except OSError: continue
        if cmd.split(b'\0')[0].endswith(b'mm7.exe'): return int(p)  # Wine sets argv[0] to the exe path
    sys.exit('mm7.exe not running')

def main():
    pid = find_pid()
    interval = float(sys.argv[1]) if len(sys.argv) > 1 else 0
    with open(f'/proc/{pid}/mem', 'rb', buffering=0) as m:
        def rd(addr, fmt):
            m.seek(addr); return struct.unpack(fmt, m.read(struct.calcsize(fmt)))
        while True:
            x, y, z, d, look = rd(PARTY, '<5i')
            h, mi = rd(HOUR, '<2i'); t, = rd(TIME, '<q')
            print(f'pid {pid}  pos=({x},{y},{z}) dir={d} look={look}  {h:02d}:{mi:02d} ticks={t}', flush=True)
            if not interval: break
            time.sleep(interval)

if __name__ == '__main__':
    main()
