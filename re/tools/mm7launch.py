#!/usr/bin/env python3
"""Restart MM7 through Heroic and load a save file by pushing UI messages into the game's queue.
Usage: mm7launch.py save002.mm7   (needs kernel.yama.ptrace_scope=0)"""
import os, struct, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mm7peek

QUEUE = 0x50CA50            # i4 count, then 40 x {i4 msg, i4 param, i4 extra}
SAVE_FILES = 0x69CE74       # char[][280] load-window file list (autosave, quicksaves, save000...), found live
UIMSG_ShowLoadWindow, UIMSG_SelectLoadSlot, UIMSG_LoadGame = 55, 165, 82
APP = '1207658916'

def pid_or_none():
    try: return mm7peek.find_pid()
    except SystemExit: return None

def push(m, msg, param=0):
    m.seek(QUEUE); n, = struct.unpack('<i', m.read(4))
    m.seek(QUEUE + 4 + n * 12); m.write(struct.pack('<iii', msg, param, 0))
    m.seek(QUEUE); m.write(struct.pack('<i', n + 1))

def wait(cond, timeout, what):
    t = time.time()
    while not cond():
        if time.time() - t > timeout: sys.exit('timeout waiting for ' + what)
        time.sleep(0.5)

NO_PAUSE_SITE, ACTIVATE_BRANCH = 0x463BAA, 0x463A63  # see NoPauseOnFocusLoss.lua

def no_pause(p):
    """Patch WM_ACTIVATEAPP so the game never pauses on focus loss; clear the flag if already paused."""
    code = b'\xE9' + struct.pack('<i', ACTIVATE_BRANCH - (NO_PAUSE_SITE + 5))
    t = time.time()
    while time.time() - t < 20:
        try:
            with open(f'/proc/{p}/mem', 'r+b', buffering=0) as m:
                m.seek(NO_PAUSE_SITE); cur = m.read(5)
                if cur == code: return
                if cur == bytes.fromhex('be00010000'):
                    m.seek(NO_PAUSE_SITE); m.write(code); print('no-pause patch applied'); return
        except OSError:
            pass
        time.sleep(0.05)
    print('warning: could not apply no-pause patch')

def main():
    target = sys.argv[1].lower()
    if '--no-restart' in sys.argv:
        return load_only(target)
    p = pid_or_none()
    if p:
        subprocess.run(['pkill', '-x', 'mm7.exe'])
        wait(lambda: pid_or_none() is None, 30, 'old game to exit')
        time.sleep(3)
    subprocess.Popen(['xdg-open', f'heroic://launch?appName={APP}&runner=gog'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    wait(lambda: pid_or_none(), 120, 'game process')
    p = pid_or_none()
    no_pause(p)
    load_only(target)

def load_only(target):
    """From the main menu: open the load window, select `target` and load it."""
    p = pid_or_none()
    with open(f'/proc/{p}/mem', 'r+b', buffering=0) as m:
        def read_names():
            names = []
            for i in range(48):
                m.seek(SAVE_FILES + i * 280); name = m.read(280).split(b'\0')[0].decode('latin-1').lower()
                if not name: break
                names.append(name)
            return names
        def queue_len():
            m.seek(QUEUE); return struct.unpack('<i', m.read(4))[0]
        # main menu is ready when it consumes our "show load window" message and fills the file list
        t = time.time()
        while not read_names():
            if time.time() - t > 60: sys.exit('timeout waiting for load window')
            if queue_len() == 0: push(m, UIMSG_ShowLoadWindow)
            time.sleep(0.3)
        names = read_names()
        print('save list:', names)
        if target not in names: sys.exit(f'{target} not in list')
        push(m, UIMSG_SelectLoadSlot, names.index(target))
        wait(lambda: queue_len() == 0, 10, 'slot selection')
        push(m, UIMSG_LoadGame)
        # loaded when the party has a position on the map
        def in_game():
            m.seek(0xACD4EC); x, y = struct.unpack('<ii', m.read(8)); return x or y
        wait(in_game, 60, 'save to load')
    print('loaded', target)

if __name__ == '__main__':
    main()
