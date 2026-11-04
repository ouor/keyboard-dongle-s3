"""BLE device inspector for checking remotes from the PC (bleak).

  python ble_inspect.py scan [-t 8] [-n NAME]
  python ble_inspect.py inspect ADDR [--pair] [--listen SEC]
  python ble_inspect.py selftest
"""
import argparse
import asyncio
import json
import sys
import time

from bleak import BleakClient, BleakScanner

REPORT_MAP = "00002a4b-0000-1000-8000-00805f9b34fb"
PNP_ID = "00002a50-0000-1000-8000-00805f9b34fb"
REPORT_REF = "00002908-0000-1000-8000-00805f9b34fb"


def hexs(b):
    return " ".join(f"{x:02X}" for x in b)


def text(b):
    return "".join(chr(x) if 32 <= x < 127 else "." for x in b)


# --- HID report descriptor (short items only; long items are skipped) ---
MAIN = {0x8: "Input", 0x9: "Output", 0xB: "Feature", 0xA: "Collection", 0xC: "End Collection"}
GLOBAL = {0x0: "Usage Page", 0x1: "Logical Min", 0x2: "Logical Max", 0x3: "Physical Min", 0x4: "Physical Max",
          0x5: "Unit Exponent", 0x6: "Unit", 0x7: "Report Size", 0x8: "Report ID", 0x9: "Report Count",
          0xA: "Push", 0xB: "Pop"}
LOCAL = {0x0: "Usage", 0x1: "Usage Min", 0x2: "Usage Max"}
PAGES = {0x01: "Generic Desktop", 0x07: "Keyboard", 0x08: "LED", 0x09: "Button", 0x0C: "Consumer",
         0x0D: "Digitizer", 0xFF00: "Vendor"}
COLLECTIONS = {0: "Physical", 1: "Application", 2: "Logical", 3: "Report"}


def parse_report_map(data):
    """Return list of (indent, name, value) tuples."""
    out, i, depth = [], 0, 0
    while i < len(data):
        b = data[i]
        if b == 0xFE:  # long item
            i += 3 + (data[i + 1] if i + 1 < len(data) else 0)
            continue
        size = (0, 1, 2, 4)[b & 3]
        typ, tag = (b >> 2) & 3, b >> 4
        raw = data[i + 1:i + 1 + size]
        val = int.from_bytes(raw, "little") if raw else 0
        i += 1 + size
        name = {0: MAIN, 1: GLOBAL, 2: LOCAL}.get(typ, {}).get(tag, f"item 0x{b:02X}")
        if name in ("Logical Min", "Physical Min") and size and val >= 1 << (8 * size - 1):
            val -= 1 << (8 * size)  # signed
        if name == "End Collection":
            depth = max(depth - 1, 0)
        shown = val
        if name == "Usage Page":
            shown = f"0x{val:02X} ({PAGES.get(val, '?')})"
        elif name == "Collection":
            shown = COLLECTIONS.get(val, val)
        elif name in ("Usage", "Usage Min", "Usage Max"):
            shown = f"0x{val:02X}"
        elif name in ("Input", "Output", "Feature"):
            shown = ("Const" if val & 1 else "Data") + "," + ("Var" if val & 2 else "Array") + "," + ("Rel" if val & 4 else "Abs")
        out.append((depth, name, shown if name != "End Collection" else ""))
        if name == "Collection":
            depth += 1
    return out


def print_report_map(data):
    print("      --- Report Map decoded ---")
    for depth, name, val in parse_report_map(data):
        print("      " + "  " * depth + f"{name} {val}".rstrip())


# --- commands ---
async def scan(timeout, name):
    found = await BleakScanner.discover(timeout=timeout, return_adv=True)
    rows = sorted(found.values(), key=lambda da: da[1].rssi, reverse=True)
    for dev, adv in rows:
        n = adv.local_name or dev.name or ""
        if name and name.lower() not in n.lower():
            continue
        print(f"{dev.address}  RSSI {adv.rssi:4}  {n or '(no name)'}")
        for u in adv.service_uuids:
            print(f"    service  {u}{'  <- HID' if u.startswith('00001812') else ''}")
        for cid, md in adv.manufacturer_data.items():
            print(f"    mfr 0x{cid:04X}  {hexs(md)}")
        if adv.tx_power is not None:
            print(f"    tx_power {adv.tx_power}")
    print(f"{len(rows)} device(s)")


async def inspect(addr, pair, listen, record):
    report_id = {}  # char handle -> HID report id (from Report Reference), used for --record
    async with BleakClient(addr, timeout=20, pair=pair) as c:  # pair before discovery: HID chars often need encryption
        print(f"connected {addr}  mtu {c.mtu_size}  paired={pair}")
        for svc in c.services:
            print(f"\n[service] {svc.uuid}  {svc.description}  (handle {svc.handle})")
            for ch in svc.characteristics:
                print(f"  [char] {ch.uuid}  {ch.description}  h={ch.handle}  {','.join(ch.properties)}")
                if "read" in ch.properties:
                    try:
                        v = bytes(await c.read_gatt_char(ch))
                        print(f"      value ({len(v)}) {hexs(v)}  |{text(v)}|")
                        if ch.uuid == REPORT_MAP:
                            print_report_map(v)
                        elif ch.uuid == PNP_ID and len(v) >= 7:
                            print(f"      PnP: src={v[0]} VID=0x{int.from_bytes(v[1:3], 'little'):04X} "
                                  f"PID=0x{int.from_bytes(v[3:5], 'little'):04X} ver=0x{int.from_bytes(v[5:7], 'little'):04X}")
                    except Exception as e:
                        print(f"      read failed: {e}")
                for d in ch.descriptors:
                    try:
                        v = bytes(await c.read_gatt_descriptor(d.handle))
                        extra = ""
                        if d.uuid == REPORT_REF and len(v) >= 2:
                            report_id[ch.handle] = v[0]
                            extra = f"  -> report id {v[0]}, {({1: 'Input', 2: 'Output', 3: 'Feature'}).get(v[1], v[1])}"
                        print(f"    [desc] {d.uuid}  {d.description}  h={d.handle}  {hexs(v)}{extra}")
                    except Exception as e:
                        print(f"    [desc] {d.uuid}  {d.description}  h={d.handle}  read failed: {e}")

        if listen:
            t0 = time.monotonic()
            out = open(record, "w", encoding="utf-8") if record else None

            def on_data(ch, data):
                t = time.monotonic() - t0
                print(f"{t:8.3f}s  h={ch.handle} {ch.uuid[4:8]}  {hexs(data)}")
                if out and ch.handle in report_id:  # same format as replay.py
                    out.write(json.dumps({"t": round(t, 4), "rid": report_id[ch.handle], "data": bytes(data).hex()}) + "\n")

            subs = [ch for s in c.services for ch in s.characteristics if {"notify", "indicate"} & set(ch.properties)]
            for ch in subs:
                try:
                    await c.start_notify(ch, lambda _, data, ch=ch: on_data(ch, data))
                except Exception as e:
                    print(f"notify h={ch.handle} failed: {e}")
            print(f"\nlistening {listen}s on {len(subs)} char(s) - press remote buttons...")
            try:
                await asyncio.sleep(listen)
            finally:
                if out:
                    out.close()
                    print(f"recorded to {record} (replay: python replay.py play {record})")


def selftest():
    # Boot keyboard descriptor head: Usage Page(Generic Desktop), Usage(Keyboard), Collection(Application),
    # Report ID 1, Logical Min -1 (signed), Input(Data,Var,Abs), End Collection
    d = bytes([0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01, 0x15, 0xFF, 0x81, 0x02, 0xC0])
    items = parse_report_map(d)
    assert [n for _, n, _ in items] == ["Usage Page", "Usage", "Collection", "Report ID", "Logical Min", "Input", "End Collection"], items
    assert items[2][2] == "Application" and items[3] == (1, "Report ID", 1)
    assert items[4][2] == -1 and items[5][2] == "Data,Var,Abs" and items[6][0] == 0
    print("selftest ok")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("scan")
    s.add_argument("-t", "--timeout", type=float, default=8)
    s.add_argument("-n", "--name", help="filter by name substring")
    i = sub.add_parser("inspect")
    i.add_argument("address")
    i.add_argument("--pair", action="store_true", help="pair first (needed if HID chars need encryption)")
    i.add_argument("--listen", type=float, default=0, metavar="SEC", help="subscribe to notifications and print reports")
    i.add_argument("--record", metavar="FILE", help="with --listen: save HID input reports as jsonl for replay.py")
    sub.add_parser("selftest")
    a = p.parse_args()
    if a.cmd == "selftest":
        return selftest()
    asyncio.run(scan(a.timeout, a.name) if a.cmd == "scan" else inspect(a.address, a.pair, a.listen, a.record))


if __name__ == "__main__":
    sys.exit(main())
