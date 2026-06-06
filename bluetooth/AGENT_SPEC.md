# Agent Spec — Make Bluetooth work on a T1 MacBook Pro (Linux)

**Purpose:** Hand this file to a coding agent (Claude Code, etc.) and say:

> *"Follow `AGENT_SPEC.md` to get my MacBook's Bluetooth working and pair a device. Stop at the GO/NO-GO gates."*

Self-contained spec + plan with verification gates and the guardrails that encode mistakes already paid for. **Read the whole file before running anything.** The headline guardrail: **this is almost certainly a userspace fix — do NOT start by chasing drivers or firmware.**

---

## 0. Mission & definition of done

**Goal:** A usable Bluetooth stack — controller powered, and a real device paired/connected — persistent across reboots.

**Done when ALL are true:**
- [ ] `bluetoothctl show` reports a `Controller …` with `Powered: yes`.
- [ ] `systemctl is-enabled bluetooth` → `enabled` and `is-active` → `active`.
- [ ] A real device pairs **and connects** (`bluetoothctl info <MAC>` → `Connected: yes`); for audio, a `bluez_output.*` sink appears in `pactl list sinks short`.
- [ ] Survives a reboot (device auto-reconnects because it was `trust`ed).

---

## 1. Preconditions — verify BEFORE touching anything (these prove it's userspace, not hardware)

```sh
cat /sys/devices/virtual/dmi/id/product_name        # MacBookPro13,2 / T1-class
rfkill list bluetooth                                # hci0 present, soft/hard block = no
ls -l /sys/class/bluetooth/                          # hci0 symlink exists (UART: …/URT0…/bluetooth/hci0)
lsmod | grep -E 'btbcm|hci_uart'                     # UART BT driver stack loaded
ls /lib/firmware/brcm/ | grep -i hcd                 # BCM-*.hcd patchram firmware present
pacman -Q bluez bluez-utils 2>&1                     # is bluez-utils actually installed?
systemctl is-enabled bluetooth; systemctl is-active bluetooth
```

**Hard gate P-1 (the diagnostic fork):**
- If `hci0` exists, is **not** rfkill-blocked, the modules are loaded, and a `BCM-*.hcd` is present → **the radio is fine; this is a userspace fix. Go to Phase A.**
- If `hci0` does **not** exist, or rfkill shows it **hard-blocked**, or no `BCM-*.hcd` firmware is present → that's a genuine kernel/firmware problem (out of scope for this spec); **stop and report** with the kernel log (`sudo dmesg | grep -iE 'bluetooth|btbcm|hci_uart|firmware'`) rather than guessing.

---

## 2. Background the agent must hold in context

- BT on this model is **Broadcom over UART** (`hci_uart` + `btbcm`), attached via the Apple `URT0` serial port — **not USB**, so `lsusb` shows nothing; that's expected, not a fault.
- Patchram firmware **`BCM-0a5c-6410.hcd`** (→ `BCM-0bb4-0306.hcd`) ships in `linux-firmware`. A controller that enumerates with a **real Apple BD_ADDR (`F4:0F:24:…`)** is proof the firmware loaded — do not go extracting `.hcd` blobs from macOS.
- The actual gaps are distro defaults: **`bluez-utils` not installed** (no `bluetoothctl`) and **`bluetooth.service` disabled**.
- **Anti-goal:** do NOT install out-of-tree BT drivers, patch the kernel, or extract firmware. Touch Bar / Wi-Fi / audio needed that; **Bluetooth does not.**

---

## 3. Plan overview

```
A. Install bluez-utils + enable daemon → B. Verify controller powered 🚦
C. Pair & connect a real device        → D. Reboot-persist check       🚦
```

---

## 4. The phases

### Phase A — Install tooling + enable the daemon
```sh
sudo pacman -S --needed bluez-utils
sudo systemctl enable --now bluetooth
```

### Phase B — Verify the controller is up 🚦
```sh
systemctl is-active bluetooth        # active
bluetoothctl show                    # Controller …, Powered: yes, PowerState: on
```
**GATE B-1:** `bluetoothctl show` lists a controller with `Powered: yes`. If `bluetoothctl` reports **no controller** even though `hci0` exists in rfkill, the daemon may not have attached it — `sudo systemctl restart bluetooth`, then re-check. If it powers on but `Powered` flips back to `no`, check `rfkill` for a soft block (`rfkill unblock bluetooth`).

### Phase C — Pair & connect a real device 🚦
```sh
bluetoothctl --timeout 15 scan on    # put the target device in pairing mode first
bluetoothctl devices                 # capture the target MAC
bluetoothctl pair  <MAC>
bluetoothctl trust <MAC>             # trust = auto-reconnect on future boots
bluetoothctl connect <MAC>
bluetoothctl info <MAC>              # Connected: yes
# audio devices only:
pactl list sinks short | grep -i bluez   # bluez_output.* sink present
```
**GATE C-1:** A real device shows `Connected: yes`. For audio, the `bluez_output.*` sink exists and can be selected. **`Powered: yes` alone is NOT done** — the acceptance test is a working paired device (e.g. audio actually playing through BT headphones).

### Phase D — Reboot-persist check 🚦
```sh
sudo reboot
# after reboot, no manual steps:
systemctl is-active bluetooth        # active (enabled at boot)
bluetoothctl info <MAC>              # trusted device auto-reconnects (power the device on)
```
**GATE D-1:** Daemon auto-starts and the trusted device reconnects.

---

## 5. Guardrails (the expensive lessons)

1. **Diagnose before driver-hunting.** If `hci0` is present + unblocked + firmware present + real BD_ADDR, it's userspace. Do not install out-of-tree BT drivers or extract macOS firmware — you'll waste hours fixing what isn't broken.
2. **`bluez` ≠ `bluez-utils`.** No `bluetoothctl` almost always means `bluez-utils` is missing.
3. **Enable the daemon explicitly.** Arch leaves `bluetooth.service` disabled by default.
4. **`trust` the device**, or it won't auto-reconnect after reboot/power-cycle.
5. **Powered ≠ done.** Acceptance = a real device paired, connected, and (for audio) actually playing.
6. **Sudoers safety:** validate with `visudo -c`; never leave a `NOPASSWD: ALL` grant behind.

## 6. Rollback
```sh
sudo systemctl disable --now bluetooth
# (leaving bluez-utils installed is harmless; remove only if you must:)
# sudo pacman -R bluez-utils
```

## 7. What to report back to the human
Product confirmation, the preconditions result (proving radio health), that `bluetooth.service` is enabled+active, the controller `Powered: yes` line, and the **connected** device (with, for audio, confirmation that sound played through it).
