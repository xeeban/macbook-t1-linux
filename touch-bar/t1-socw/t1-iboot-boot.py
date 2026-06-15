#!/usr/bin/env python3
# t1-iboot-boot.py — tell the T1's iBoot (recovery mode, USB 05ac:1281) to boot
# bridgeOS. Equivalent to `irecovery -n`. After a bare ASOC.FRST reset the T1
# lands in iBoot recovery (dark Touch Bar); this sends the recovery-mode USB
# command protocol to set auto-boot and reboot into the OS, which should bring
# the T1 back as 05ac:8600 with a FRESHLY COLD-BOOTED (full-brightness) bar.
#
# iBoot recovery command protocol (from libirecovery): each command is a
# control transfer  bmRequestType=0x40, bRequest=0, wValue=0, wIndex=0,
# data = "<command>\0".  No firmware restore, no erase — only env + reboot.
#
# Needs python-pyusb (sudo pacman -S python-pyusb). Run as root.
# Safety net: a host reboot always re-inits the T1 to 05ac:8600 regardless.
import sys, time
import usb.core, usb.util

VID, PID = 0x05AC, 0x1281

d = usb.core.find(idVendor=VID, idProduct=PID)
if d is None:
    sys.exit("No T1 in recovery mode (05ac:1281). Check: "
             "cat /sys/bus/usb/devices/1-3/idProduct")

print(f"found T1 in recovery: {d.idVendor:04x}:{d.idProduct:04x}")
try:
    if d.is_kernel_driver_active(0):
        d.detach_kernel_driver(0)
        print("detached kernel driver on intf 0")
except Exception as e:
    print("(kernel-driver check skipped:", e, ")")
try:
    d.set_configuration()
except Exception as e:
    print("set_configuration warning:", e)

def cmd(s):
    data = s.encode() + b"\x00"
    n = d.ctrl_transfer(0x40, 0, 0, 0, data, 5000)
    print(f"  sent {s!r} -> {n} bytes")
    time.sleep(0.4)

mode = sys.argv[1] if len(sys.argv) > 1 else "normal"
print(f"sending iBoot boot sequence (mode={mode})...")
if mode == "reboot":
    # least-invasive: just reboot (boots OS only if auto-boot already true)
    cmd("reboot")
else:
    # irecovery -n equivalent: ensure auto-boot, persist, reboot into bridgeOS
    cmd("setenv auto-boot true")
    cmd("saveenv")
    cmd("reboot")
print("done; watch for the T1 to re-enumerate as 05ac:8600 and the bar to light.")
