#!/usr/bin/env python3
"""Write a foxhollow input trace (FOXHOLLOW_INPUT_REPLAY) for automated runs.

    make_input_trace.py out.fhpad EVENT [EVENT...]

EVENT is BUTTONS@FRAME[+LENGTH], e.g. START@300+6 or A+B@500 (LENGTH defaults
to 6 frames). FRAME counts video frames (the 3DS port's pad.c patch reads one trace frame per retrace). A stick can be held with
STICK=x,y@FRAME+LENGTH (x, y in -128..127). The trace ends after the last event;
the game then sees a connected controller with nothing pressed.

To use it, copy the trace to the SD card and set in sdmc:/3ds/star3d/env.txt:
    FOXHOLLOW_INPUT_REPLAY=sdmc:/3ds/star3d/input.fhpad
"""
import struct
import sys

BUTTONS = {
    'LEFT': 0x0001, 'RIGHT': 0x0002, 'DOWN': 0x0004, 'UP': 0x0008,
    'Z': 0x0010, 'R': 0x0020, 'L': 0x0040,
    'A': 0x0100, 'B': 0x0200, 'X': 0x0400, 'Y': 0x0800, 'START': 0x1000,
}
STATUS = struct.Struct('<Hbbbb BBBB b x I')  # PADStatus with TARGET_PC's extButton
assert STATUS.size == 16
HEADER = b'FHPAD' + bytes([1, STATUS.size, 4])


def parse(event):
    what, _, when = event.partition('@')
    start, _, length = when.partition('+')
    start, length = int(start), int(length or 6)
    if what.upper().startswith('STICK='):
        x, y = (int(v) for v in what[6:].split(','))
        return start, length, 0, (x, y)
    mask = 0
    for name in what.upper().split('+'):
        mask |= BUTTONS[name]
    return start, length, mask, None


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    events = [parse(e) for e in sys.argv[2:]]
    frames = max(s + n for s, n, _, _ in events)
    with open(sys.argv[1], 'wb') as f:
        f.write(HEADER)
        for frame in range(frames):
            buttons, sx, sy = 0, 0, 0
            for start, length, mask, stick in events:
                if start <= frame < start + length:
                    buttons |= mask
                    if stick:
                        sx, sy = stick
            analog_a = 255 if buttons & BUTTONS['A'] else 0
            analog_b = 255 if buttons & BUTTONS['B'] else 0
            trig_l = 255 if buttons & BUTTONS['L'] else 0
            trig_r = 255 if buttons & BUTTONS['R'] else 0
            f.write(STATUS.pack(buttons, sx, sy, 0, 0, trig_l, trig_r, analog_a, analog_b, 0, 0))
            for _ in range(3):
                f.write(STATUS.pack(0, 0, 0, 0, 0, 0, 0, 0, 0, -1, 0))
    print(f'{sys.argv[1]}: {frames} frames')


if __name__ == '__main__':
    main()
