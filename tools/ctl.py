#!/usr/bin/env python3
"""Drive the chess clock from the Mac over USB serial: simulate taps, read state, grab screenshots.

    tools/ctl.py "state"
    tools/ctl.py "touch 184 90; wait 2.5; state; shot /tmp/after_start.png"

Steps are separated by ';':
    touch X Y   a tap at screen position X,Y (368 x 448; y<196 top player, y>=252 bottom player)
    left P S    set player P's (0 = bottom, 1 = top) time to S seconds (test aid)
    log S       print everything the board prints for S seconds (real taps show as "TAP x= y= phase=")
    waitfile F  block until file F exists (a hand-off: create it when the human is ready)
    cal         show 5 numbered targets; combine with "log S" to see where real taps land
    boot        print what the board printed while booting (touch chip found? PSRAM?)
    wait S      sleep S seconds
    state       print the clock state the board reports, plus the Mac-side wall clock for comparison
    shot FILE   save exactly what the panel shows as a PNG

Opening the port restarts the board (macOS asserts DTR/RTS), so one invocation = one fresh session.
"""
import struct
import sys
import time
import zlib

import serial
from serial.tools import list_ports


def find_port():
    c = sorted(p.device for p in list_ports.comports() if p.vid == 0x303A)
    return next((d for d in c if d.startswith("/dev/cu.")), c[0] if c else None)


def png(path, w, h, rgb):
    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


class Board:
    def __init__(self, port=None):
        port = port or find_port()
        if not port:
            sys.exit("no board found (tools/board.sh status)")
        self.ser = serial.Serial()
        self.ser.port, self.ser.baudrate, self.ser.timeout = port, 115200, 0.2
        self.ser.dtr = self.ser.rts = False
        self.ser.open()
        time.sleep(2.5)  # opening the port reboots the board; let it finish
        self.boot = self.ser.read_all().decode(errors="replace")  # what it printed while booting
        self.ser.reset_input_buffer()

    def send(self, line):
        self.ser.write((line + "\n").encode())

    def read_until(self, prefix, timeout=5.0):
        end = time.time() + timeout
        while time.time() < end:
            line = self.ser.readline().decode(errors="replace").strip()
            if line.startswith(prefix):
                return line
        raise TimeoutError(f"no '{prefix}' line from the board within {timeout}s")

    def state(self):
        t0 = time.time()
        self.send("STATE")
        line = self.read_until("STATE")
        t1 = time.time()
        d = dict(kv.split("=", 1) for kv in line.split()[1:])
        d["mac_s"] = (t0 + t1) / 2
        return d

    def shot(self, path):
        self.ser.reset_input_buffer()
        self.send("DUMP")
        head = self.read_until("FB ", 5).split()
        w, h = int(head[1]), int(head[2])
        rgb = bytearray()
        for _ in range(h):
            row = self.ser.readline().strip()
            while len(row) != w * 4:  # a stray log line, or a short read: take the next line
                row = self.ser.readline().strip()
            for i in range(0, w * 4, 4):
                v = int(row[i:i + 4], 16)
                r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
                rgb += bytes((r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2))
        png(path, w, h, bytes(rgb))
        return path


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    b = Board()
    for step in sys.argv[1].split(";"):
        t = step.split()
        if not t:
            continue
        if t[0] == "touch":
            b.send(f"TOUCH {t[1]} {t[2]}")
        elif t[0] == "waitfile":  # block until FILE exists (lets a human say "go" before a timed test)
            import os
            while not os.path.exists(t[1]):
                time.sleep(0.2)
        elif t[0] == "cal":
            b.send("CAL")
        elif t[0] == "boot":
            print(b.boot.strip())
        elif t[0] == "left":
            b.send(f"LEFT {t[1]} {t[2]}")
        elif t[0] == "log":  # print everything the board says for S seconds (e.g. real taps: "TAP x=.. y=..")
            end = time.time() + float(t[1])
            while time.time() < end:
                line = b.ser.readline().decode(errors="replace").strip()
                if line:
                    print(f"{time.time() % 1000:8.2f} {line}", flush=True)
        elif t[0] == "wait":
            time.sleep(float(t[1]))
        elif t[0] == "state":
            print(b.state())
        elif t[0] == "shot":
            print("saved", b.shot(t[1]))
        else:
            sys.exit(f"unknown step: {step}")


if __name__ == "__main__":
    main()
