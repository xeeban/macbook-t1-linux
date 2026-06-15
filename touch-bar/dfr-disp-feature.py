#!/usr/bin/env python3
# dfr-disp-feature.py — get/set the T1 config-2 DISP Feature report (Report ID 2)
# on the iBridge HID interface 6 (/dev/hidraw1). This is the panel-power control:
#   field 1 usage 0x20 DISP_AUX1 (1B, 1-4)
#   field 2 usage 0x21 DISP      (1B, 1-4)  DISP_ON=1 DISP_DIM=2 DISP_OFF=4
#   field 3 usage 0x22           (4B LE, 0-100000 phys 0-100)  <- brightness?
#   field 4 usage 0x23           (4B LE, 0-1000   phys 0-1)
# Report id 2 payload = id(1) + 1 + 1 + 4 + 4 = 11 bytes.
#
# PLAN: while the bar is LIT (e.g. fresh after reboot):   sudo ./dfr-disp-feature.py get
#       note the bytes. Then after suspend/hibernate (bar DARK):
#                                                          sudo ./dfr-disp-feature.py set <hexbytes>
#       and watch the bar. If it relights, we've found the config-2-native relight
#       (bake those bytes into appletbdrm's .resume — no config bounce, macOS-style).
#
# Convenience: `on` sends a best-guess ON (aux1=1, disp=1, brightness=100000, f4=1000).
# Prefer capturing real values with `get` first.
import sys, fcntl, struct

DEV = "/dev/hidraw1"
RID = 2
RLEN = 11  # id + 10 data

def _iowr(nr, size):
    return (3 << 30) | (size << 16) | (ord('H') << 8) | nr

def get():
    buf = bytearray(RLEN)
    buf[0] = RID
    with open(DEV, "rb+", buffering=0) as f:
        fcntl.ioctl(f, _iowr(0x07, RLEN), buf, True)  # HIDIOCGFEATURE
    print("report id 2 =", buf.hex(" "))
    # decode
    aux1 = buf[1]; disp = buf[2]
    f22 = struct.unpack_from("<I", buf, 3)[0]
    f23 = struct.unpack_from("<I", buf, 7)[0]
    print(f"  aux1(0x20)={aux1}  disp(0x21)={disp}  f0x22={f22}  f0x23={f23}")

def setbytes(hexstr):
    data = bytes.fromhex(hexstr.replace(" ", ""))
    buf = bytearray(data)
    if not buf or buf[0] != RID:
        buf = bytearray([RID]) + buf  # prepend report id if missing
    with open(DEV, "rb+", buffering=0) as f:
        fcntl.ioctl(f, _iowr(0x06, len(buf)), buf, True)  # HIDIOCSFEATURE
    print("wrote", buf.hex(" "))

def on():
    buf = bytearray([RID, 1, 1]) + struct.pack("<I", 100000) + struct.pack("<I", 1000)
    setbytes(buf.hex())

if __name__ == "__main__":
    if len(sys.argv) < 2 or sys.argv[1] not in ("get", "set", "on"):
        print("usage: dfr-disp-feature.py get | on | set <hexbytes>", file=sys.stderr); sys.exit(2)
    cmd = sys.argv[1]
    if cmd == "get": get()
    elif cmd == "on": on()
    elif cmd == "set": setbytes(" ".join(sys.argv[2:]))  # join: hex bytes arrive as separate argv
