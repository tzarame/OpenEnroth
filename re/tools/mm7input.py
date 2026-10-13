#!/usr/bin/env python3
"""Virtual keyboard via /dev/uinput (stdlib only). Keys go to the FOCUSED window.
Usage: mm7input.py KEY:SECONDS [KEY:SECONDS ...]   e.g. up:1.5 left:0.5"""
import fcntl, os, struct, sys, time

KEYS = {'up': 103, 'left': 105, 'right': 106, 'down': 108, 'space': 57, 'enter': 28,
        'esc': 1, 'f9': 67, 'f10': 68, 'f11': 87, 'f12': 88, 'pgup': 104, 'pgdn': 109, 'home': 102, 'end': 107}
UI_SET_EVBIT, UI_SET_KEYBIT, UI_DEV_CREATE, UI_DEV_DESTROY = 0x40045564, 0x40045565, 0x5501, 0x5502
EV_SYN, EV_KEY = 0, 1

def ev(fd, t, c, v):
    s = time.time()
    os.write(fd, struct.pack('llHHi', int(s), int(s % 1 * 1e6), t, c, v))

def main():
    steps = [(a.split(':')[0], float(a.split(':')[1])) for a in sys.argv[1:]]
    fd = os.open('/dev/uinput', os.O_WRONLY | os.O_NONBLOCK)
    fcntl.ioctl(fd, UI_SET_EVBIT, EV_KEY)
    for c in KEYS.values(): fcntl.ioctl(fd, UI_SET_KEYBIT, c)
    os.write(fd, struct.pack('80sHHHHi', b'mm7-virtual-kbd', 3, 1, 1, 1, 0) + bytes(4 * 64 * 4))
    fcntl.ioctl(fd, UI_DEV_CREATE)
    time.sleep(1)  # let the compositor pick the device up
    try:
        for key, dur in steps:
            c = KEYS[key]
            ev(fd, EV_KEY, c, 1); ev(fd, EV_SYN, 0, 0)
            time.sleep(dur)
            ev(fd, EV_KEY, c, 0); ev(fd, EV_SYN, 0, 0)
            time.sleep(0.15)
    finally:
        fcntl.ioctl(fd, UI_DEV_DESTROY); os.close(fd)

if __name__ == '__main__':
    main()
