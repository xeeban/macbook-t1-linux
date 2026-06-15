#!/usr/bin/env python3
# dfr-feat.py — general get/set of the T1 config-2 vendor (0xff12) Feature
# reports on /dev/hidraw1 (iBridge interface 6). Decoded from the report
# descriptor 2026-06-15:
#
#   ID 2 (11B): [id][0x20 aux1:1][0x21 disp:1][0x22 bright?:4LE][0x23:4LE]
#   ID 3 (15B): [id][0x31 mode:1][0x32 bright%:4LE 0..100000][0x50:1][0x51:8]
#       ^ macOS's 2017 wake trace used SET_REPORT id 3 (15B). 0x32 = 0..100000 phys 0..100.
#   ID 4 (14B): [id][0x10 mode:1][0x11 lux:4LE 0..2e6][0x12:4LE 0..1000][0x13 bright%:4LE 0..100000]
#       ^ ALS / auto-brightness. 0x13 = resulting brightness %.
#   ID 6 (10B): [id][0x31:1][0x51:8]
#
# Usage:
#   sudo ./dfr-feat.py get 3              # read + decode report id 3
#   sudo ./dfr-feat.py getall            # read 2,3,4,6
#   sudo ./dfr-feat.py bright3 <0-100>   # set report 3 to manual brightness pct (0x31=1, 0x32=pct*1000)
#   sudo ./dfr-feat.py bright4 <0-100>   # set report 4 brightness 0x13=pct*1000 (0x10=1)
#   sudo ./dfr-feat.py set 3 "03 01 .."  # raw write
import sys, fcntl, struct

DEV = "/dev/hidraw1"
LEN = {2: 11, 3: 15, 4: 14, 6: 10}
# Report ID 1 = HID Sensors (Usage Page 0x20) ambient-light sensor FEATURE report.
# Layout (best-guess from descriptor): [id][0x0316 ReportingState:1][0x030e Interval:2LE]
#   [0x0309 SensorStatus?:1][0x0319 PowerState:1][0x0318:20][0x0201 SensorState:1]
#   [0x14d1 illum?:4][0x0304:2]  -> 33 bytes. We probe the length to be safe.
def _get1():
    for n in (33, 32, 31, 30, 14, 13, 12, 10, 8, 6):
        buf = bytearray(n); buf[0] = 1
        try:
            with open(DEV, "rb+", buffering=0) as f:
                fcntl.ioctl(f, _iowr(0x07, n), buf, True)
            print(f"id 1 (len {n}) = {bytes(buf).hex(' ')}")
            return buf, n
        except OSError:
            continue
    print("id 1: no length worked"); return None, 0

def als_on():
    buf, n = _get1()
    if not buf: return
    # ReportingState -> All Events (2); Interval -> 200ms; PowerState -> D0 Full (2)
    buf[1] = 2
    struct.pack_into('<H', buf, 2, 200)
    buf[5] = 2
    _set(buf)
    print("re-read:"); _get1()

def _iowr(nr, size): return (3 << 30) | (size << 16) | (ord('H') << 8) | nr

def _get(rid):
    buf = bytearray(LEN[rid]); buf[0] = rid
    with open(DEV, "rb+", buffering=0) as f:
        fcntl.ioctl(f, _iowr(0x07, LEN[rid]), buf, True)  # HIDIOCGFEATURE
    return buf

def _set(buf):
    with open(DEV, "rb+", buffering=0) as f:
        fcntl.ioctl(f, _iowr(0x06, len(buf)), buf, True)  # HIDIOCSFEATURE
    print("wrote", bytes(buf).hex(" "))

def decode(rid, b):
    print(f"id {rid} = {bytes(b).hex(' ')}")
    if rid == 2:
        print(f"  aux1=0x{b[1]:x} disp=0x{b[2]:x} 0x22={struct.unpack_from('<I',b,3)[0]} 0x23={struct.unpack_from('<I',b,7)[0]}")
    elif rid == 3:
        print(f"  0x31(mode)={b[1]} 0x32(bright)={struct.unpack_from('<I',b,2)[0]} 0x50={b[6]} 0x51={struct.unpack_from('<Q',b,7)[0]}")
    elif rid == 4:
        print(f"  0x10(mode)={b[1]} 0x11(lux)={struct.unpack_from('<I',b,2)[0]} 0x12={struct.unpack_from('<I',b,6)[0]} 0x13(bright)={struct.unpack_from('<I',b,10)[0]}")
    elif rid == 6:
        print(f"  0x31={b[1]} 0x51={struct.unpack_from('<Q',b,2)[0]}")

def getall():
    for rid in (2, 3, 4, 6):
        try: decode(rid, _get(rid))
        except OSError as e: print(f"id {rid}: {e}")

if __name__ == "__main__":
    a = sys.argv
    if len(a) < 2: print("usage: get N | getall | bright3 PCT | bright4 PCT | set N <hex>", file=sys.stderr); sys.exit(2)
    cmd = a[1]
    if cmd == "get1": _get1()
    elif cmd == "als-on": als_on()
    elif cmd == "get": decode(int(a[2]), _get(int(a[2])))
    elif cmd == "getall": getall()
    elif cmd == "bright3":
        pct = int(a[2]); cur = _get(3)
        struct.pack_into('<I', cur, 2, pct * 1000); cur[1] = 1   # 0x31 mode=1, 0x32=pct*1000
        _set(cur); decode(3, _get(3))
    elif cmd == "bright4":
        pct = int(a[2]); cur = _get(4)
        struct.pack_into('<I', cur, 10, pct * 1000); cur[1] = 1  # 0x10 mode=1, 0x13=pct*1000
        _set(cur); decode(4, _get(4))
    elif cmd == "set":
        rid = int(a[2]); data = bytes.fromhex(a[3].replace(" ", ""))
        buf = bytearray(data) if data and data[0] == rid else bytearray([rid]) + bytearray(data)
        _set(buf)
