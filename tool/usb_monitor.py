"""Shows what the dongle actually sends to Windows (USB side check). Windows-only, no extra packages.

  python usb_monitor.py list [--all]           HID collections of the dongle (VID 303A) as Windows sees them
  python usb_monitor.py watch [--all] [-t SEC] key / consumer events from the dongle only (--all: every device)

Uses the Raw Input API: works even for keyboards (which hidapi can't open on Windows) and tells devices apart.
"""
import argparse
import ctypes as C
import ctypes.wintypes as W
import json
import pathlib
import sys
import time

u32 = C.WinDLL("user32", use_last_error=True)

RIM_TYPEMOUSE, RIM_TYPEKEYBOARD, RIM_TYPEHID = 0, 1, 2
RIDI_DEVICENAME, RIDI_DEVICEINFO = 0x20000007, 0x2000000B
RID_INPUT, RIDEV_INPUTSINK, WM_INPUT, PM_REMOVE = 0x10000003, 0x100, 0x00FF, 1
HWND_MESSAGE = W.HWND(-3)
DONGLE_VID = "VID_303A"


class RAWINPUTDEVICE(C.Structure):
    _fields_ = [("usUsagePage", W.USHORT), ("usUsage", W.USHORT), ("dwFlags", W.DWORD), ("hwndTarget", W.HWND)]


class RAWINPUTDEVICELIST(C.Structure):
    _fields_ = [("hDevice", W.HANDLE), ("dwType", W.DWORD)]


class RAWINPUTHEADER(C.Structure):
    _fields_ = [("dwType", W.DWORD), ("dwSize", W.DWORD), ("hDevice", W.HANDLE), ("wParam", W.WPARAM)]


class RAWKEYBOARD(C.Structure):
    _fields_ = [("MakeCode", W.USHORT), ("Flags", W.USHORT), ("Reserved", W.USHORT), ("VKey", W.USHORT),
                ("Message", W.UINT), ("ExtraInformation", W.ULONG)]


class RID_DEVICE_INFO_HID(C.Structure):
    _fields_ = [("dwVendorId", W.DWORD), ("dwProductId", W.DWORD), ("dwVersionNumber", W.DWORD),
                ("usUsagePage", W.USHORT), ("usUsage", W.USHORT)]


class RID_DEVICE_INFO_KEYBOARD(C.Structure):
    _fields_ = [(n, W.DWORD) for n in ("dwType", "dwSubType", "dwKeyboardMode", "dwNumberOfFunctionKeys",
                                       "dwNumberOfIndicators", "dwNumberOfKeysTotal")]


class RID_DEVICE_INFO_MOUSE(C.Structure):
    _fields_ = [("dwId", W.DWORD), ("dwNumberOfButtons", W.DWORD), ("dwSampleRate", W.DWORD), ("fHasHorizontalWheel", W.BOOL)]


class RID_DEVICE_INFO(C.Structure):
    class _U(C.Union):
        _fields_ = [("mouse", RID_DEVICE_INFO_MOUSE), ("keyboard", RID_DEVICE_INFO_KEYBOARD), ("hid", RID_DEVICE_INFO_HID)]
    _fields_ = [("cbSize", W.DWORD), ("dwType", W.DWORD), ("u", _U)]


u32.GetRawInputDeviceList.argtypes = [C.POINTER(RAWINPUTDEVICELIST), C.POINTER(W.UINT), W.UINT]
u32.GetRawInputDeviceInfoW.argtypes = [W.HANDLE, W.UINT, C.c_void_p, C.POINTER(W.UINT)]
u32.GetRawInputData.argtypes = [W.HANDLE, W.UINT, C.c_void_p, C.POINTER(W.UINT), W.UINT]
u32.RegisterRawInputDevices.argtypes = [C.POINTER(RAWINPUTDEVICE), W.UINT, W.UINT]
u32.CreateWindowExW.restype = W.HWND
u32.CreateWindowExW.argtypes = [W.DWORD, W.LPCWSTR, W.LPCWSTR, W.DWORD, C.c_int, C.c_int, C.c_int, C.c_int,
                                W.HWND, W.HMENU, W.HINSTANCE, W.LPVOID]
u32.PeekMessageW.argtypes = [C.POINTER(W.MSG), W.HWND, W.UINT, W.UINT, W.UINT]
u32.GetKeyNameTextW.argtypes = [W.LONG, W.LPWSTR, C.c_int]

# consumer usage -> name, from the same table the firmware uses
_codes = json.loads((pathlib.Path(__file__).with_name("keycodes.json")).read_text(encoding="utf-8"))
CONSUMER = {v: k for k, v in _codes["consumer"].items()}
KEYBOARD = {v: k for k, v in _codes["keyboard"].items()}

_names = {}


def device_name(h):
    if not h:
        return "(injected/no device)"
    if h not in _names:
        n = W.UINT(0)
        u32.GetRawInputDeviceInfoW(h, RIDI_DEVICENAME, None, C.byref(n))
        buf = C.create_unicode_buffer(n.value + 1)
        u32.GetRawInputDeviceInfoW(h, RIDI_DEVICENAME, buf, C.byref(n))
        _names[h] = buf.value
    return _names[h]


def device_info(h):
    info = RID_DEVICE_INFO(cbSize=C.sizeof(RID_DEVICE_INFO))
    n = W.UINT(C.sizeof(info))
    u32.GetRawInputDeviceInfoW(h, RIDI_DEVICEINFO, C.byref(info), C.byref(n))
    return info


def cmd_list(show_all):
    n = W.UINT(0)
    u32.GetRawInputDeviceList(None, C.byref(n), C.sizeof(RAWINPUTDEVICELIST))
    arr = (RAWINPUTDEVICELIST * n.value)()
    u32.GetRawInputDeviceList(arr, C.byref(n), C.sizeof(RAWINPUTDEVICELIST))
    shown = 0
    for d in arr[:n.value]:
        name = device_name(d.hDevice)
        if not show_all and DONGLE_VID not in name.upper():
            continue
        info = device_info(d.hDevice)
        kind = {RIM_TYPEMOUSE: "mouse   ", RIM_TYPEKEYBOARD: "keyboard", RIM_TYPEHID: "hid     "}[d.dwType]
        extra = ""
        if d.dwType == RIM_TYPEHID:
            h = info.u.hid
            extra = f"page 0x{h.usUsagePage:02X} usage 0x{h.usUsage:02X} ver 0x{h.dwVersionNumber:04X}"
        print(f"{kind} {extra:34} {name}")
        shown += 1
    print(f"{shown} collection(s)" + ("" if show_all else " from the dongle (VID 303A); --all for every device"))


def key_name(kb):
    lparam = (kb.MakeCode << 16) | ((1 << 24) if kb.Flags & 2 else 0)  # RI_KEY_E0
    buf = C.create_unicode_buffer(64)
    u32.GetKeyNameTextW(lparam, buf, 64)
    return buf.value or f"vk 0x{kb.VKey:02X}"


def handle(lparam, show_all, t0):
    n = W.UINT(0)
    hsz = C.sizeof(RAWINPUTHEADER)
    u32.GetRawInputData(W.HANDLE(lparam), RID_INPUT, None, C.byref(n), hsz)
    buf = (C.c_ubyte * n.value)()
    if u32.GetRawInputData(W.HANDLE(lparam), RID_INPUT, buf, C.byref(n), hsz) != n.value:
        return
    hdr = RAWINPUTHEADER.from_buffer(buf)
    name = device_name(hdr.hDevice)
    if not show_all and DONGLE_VID not in name.upper():
        return
    t = f"{time.monotonic() - t0:8.3f}s"
    src = "" if not show_all else f"  [{name[-60:]}]"
    if hdr.dwType == RIM_TYPEKEYBOARD:
        kb = RAWKEYBOARD.from_buffer(buf, hsz)
        print(f"{t}  key {'up  ' if kb.Flags & 1 else 'down'} {key_name(kb):14} scan 0x{kb.MakeCode:02X} vk 0x{kb.VKey:02X}{src}")
    elif hdr.dwType == RIM_TYPEHID:
        size, count = W.DWORD.from_buffer(buf, hsz).value, W.DWORD.from_buffer(buf, hsz + 4).value
        raw = bytes(buf[hsz + 8: hsz + 8 + size * count])
        for i in range(count):
            r = raw[i * size:(i + 1) * size]
            desc = ""
            if len(r) >= 3:  # [report id][usage lo][usage hi] = consumer report from usb.c
                u = r[1] | r[2] << 8
                desc = f"consumer {CONSUMER.get(u, hex(u)) if u else 'release'}"
            print(f"{t}  hid  {r.hex(' ')}  {desc}{src}")


def cmd_watch(show_all, seconds):
    hwnd = u32.CreateWindowExW(0, "STATIC", "usb_monitor", 0, 0, 0, 0, 0, HWND_MESSAGE, None, None, None)
    if not hwnd:
        sys.exit(f"CreateWindowEx failed: {C.get_last_error()}")
    devs = (RAWINPUTDEVICE * 2)(RAWINPUTDEVICE(0x01, 0x06, RIDEV_INPUTSINK, hwnd),   # keyboard
                                RAWINPUTDEVICE(0x0C, 0x01, RIDEV_INPUTSINK, hwnd))   # consumer control
    if not u32.RegisterRawInputDevices(devs, 2, C.sizeof(RAWINPUTDEVICE)):
        sys.exit(f"RegisterRawInputDevices failed: {C.get_last_error()}")
    print(f"watching {'all devices' if show_all else 'dongle (VID 303A)'}" + (f" for {seconds}s" if seconds else ", Ctrl+C to stop"))
    t0 = time.monotonic()
    msg = W.MSG()
    try:
        while not seconds or time.monotonic() - t0 < seconds:
            while u32.PeekMessageW(C.byref(msg), hwnd, 0, 0, PM_REMOVE):
                if msg.message == WM_INPUT:
                    handle(msg.lParam, show_all, t0)
            time.sleep(0.005)
    except KeyboardInterrupt:
        pass


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    for name in ("list", "watch"):
        s = sub.add_parser(name)
        s.add_argument("--all", action="store_true")
        if name == "watch":
            s.add_argument("-t", "--seconds", type=float, default=0)
    a = p.parse_args()
    cmd_list(a.all) if a.cmd == "list" else cmd_watch(a.all, a.seconds)


if __name__ == "__main__":
    main()
