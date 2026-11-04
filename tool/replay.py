"""Record remote input once, replay it into the dongle as often as you like (no remote, no pairing).

  python replay.py record out.jsonl [--port COMx]      saves `rpt` lines the dongle prints for live BLE input
  python replay.py play in.jsonl [--port COMx] [--speed 2]   sends them back as `inject` (goes through the keymap)
  python replay.py send "map 3 2302 AppHome" [--port COMx]   one-off command, prints the reply

File format (one JSON per line, also written by `ble_inspect.py inspect ADDR --listen 30 --record f.jsonl`):
  {"t": 1.234, "rid": 3, "data": "2302"}
"""
import argparse
import json
import sys
import time

import serial
from serial.tools import list_ports

DONGLE = (0x303A, 0x4007)  # firmware/main/usb.c USB_VID/USB_PID


def open_port(port):
    if not port:
        found = [p.device for p in list_ports.comports() if (p.vid, p.pid) == DONGLE]
        if not found:
            sys.exit("dongle CDC (303A:4007) not found; pass --port")
        port = found[0]
    s = serial.serial_for_url(port, 115200, timeout=0.2)  # also accepts loop:// for testing
    s.dtr = True  # firmware only prints when DTR is set
    time.sleep(0.1)
    return s


def read_until_ok(s, timeout=2.0):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        line = s.readline().decode(errors="replace").strip()
        if line:
            print(line)
            if line.startswith(("ok", "err")):
                return line.startswith("ok")
    print("(no reply)")
    return False


def record(path, port):
    s = open_port(port)
    print(f"recording from {s.port} -> {path}, press remote buttons, Ctrl+C to stop")
    t0, n = None, 0
    with open(path, "w", encoding="utf-8") as f:
        try:
            while True:
                parts = s.readline().decode(errors="replace").split()
                if len(parts) == 3 and parts[0] == "rpt":
                    now = time.monotonic()
                    t0 = t0 or now
                    f.write(json.dumps({"t": round(now - t0, 4), "rid": int(parts[1]), "data": parts[2]}) + "\n")
                    f.flush()
                    n += 1
                    print(f"{now - t0:8.3f}s  rid {parts[1]}  {parts[2]}")
        except KeyboardInterrupt:
            pass
    print(f"{n} report(s) saved")


def play(path, port, speed):
    events = [json.loads(line) for line in open(path, encoding="utf-8") if line.strip()]
    s = open_port(port)
    t0 = time.monotonic()
    for e in events:
        assert 0 <= e["rid"] <= 255 and all(c in "0123456789abcdefABCDEF" for c in e["data"]), e
        delay = e["t"] / speed - (time.monotonic() - t0)
        if delay > 0:
            time.sleep(delay)
        s.write(f"inject {e['rid']} {e['data']}\n".encode())
        print(f"{e['t']:8.3f}s  inject {e['rid']} {e['data']}")
        s.reset_input_buffer()  # don't let replies pile up
    print(f"{len(events)} report(s) replayed")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--port", help="COMx (default: auto-detect 303A:4007)")
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("record", parents=[common]).add_argument("file")
    pl = sub.add_parser("play", parents=[common])
    pl.add_argument("file")
    pl.add_argument("--speed", type=float, default=1.0)
    sub.add_parser("send", parents=[common]).add_argument("line")
    a = p.parse_args()
    if a.cmd == "record":
        record(a.file, a.port)
    elif a.cmd == "play":
        play(a.file, a.port, a.speed)
    else:
        s = open_port(a.port)
        s.write((a.line + "\n").encode())
        sys.exit(0 if read_until_ok(s) else 1)


if __name__ == "__main__":
    main()
