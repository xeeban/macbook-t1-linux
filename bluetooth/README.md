# Fixing Bluetooth on a T1 MacBook Pro (Linux)

Why Bluetooth appears "broken" on a fresh Arch install when the hardware was working the whole time — and the two-command fix that's pure userspace, no driver or firmware surgery.

> **TL;DR** — On a **MacBookPro13,2 (T1)** running Arch Linux (kernel `7.0.10`), the internal **Broadcom Bluetooth** (UART-attached via the Apple `URT0` serial port, driven by `btbcm` + `hci_uart`) comes up **fully** at the kernel level — `hci0` enumerates, isn't rfkill-blocked, and the patchram firmware (`BCM-0a5c-6410.hcd`, shipped in `linux-firmware`) loads. The reason "Bluetooth doesn't work" is entirely userspace: Arch **doesn't install `bluez-utils`** (so there's no `bluetoothctl`) and **doesn't enable `bluetooth.service`** by default. Fix:
> ```sh
> sudo pacman -S --needed bluez-utils
> sudo systemctl enable --now bluetooth
> ```
> After that the controller is `Powered: yes` and pairs normally — including A2DP audio to Bluetooth headphones.

> **Status:** ✅ **Fixed 2026-06-05** — controller live, paired with Bose headphones, A2DP audio confirmed working over Bluetooth.

> **Want your agent to do this for you?** Point it at [`AGENT_SPEC.md`](./AGENT_SPEC.md).

This is part of [the T1 MacBook Pro on Linux journey](../).

---

## The machine

- **MacBookPro13,2** — 2016 13" MacBook Pro (T1), Arch Linux, kernel `7.0.10-arch1-1`, GNOME, PipeWire.
- **Bluetooth:** Broadcom controller attached over **UART**, not USB (`lsusb` shows no BT device). ACPI path `\_SB_.PCI0.URT0.BLTH`; sysfs chain `dw-apb-uart.1 → serial1-0 → bluetooth/hci0`. Driven by **`hci_uart` + `btbcm`**, with patchram firmware **`BCM-0a5c-6410.hcd`** (a symlink to `BCM-0bb4-0306.hcd`, both shipped by `linux-firmware`).

## The symptom

No Bluetooth in the desktop, no obvious way to manage it — and, tellingly, **no `bluetoothctl` command at all**. It *looks* like missing hardware support, which sends you hunting for the wrong fix (drivers, firmware extraction from macOS, kernel patches).

## The trap: assuming it's a driver or firmware problem

On these Macs the *other* radios genuinely needed out-of-tree work (see [Wi-Fi](../wifi/) and [audio](../audio/)), so the instinct is to assume Bluetooth is the same. It isn't. The evidence that the **kernel/hardware side is already healthy**:

| Check | Result | Meaning |
|---|---|---|
| `rfkill list bluetooth` | `hci0` present, soft/hard block **no** | The controller exists and isn't blocked. |
| `lsmod \| grep -E 'btbcm\|hci_uart'` | both loaded | The UART Bluetooth driver stack bound. |
| `ls /sys/class/bluetooth/` | `hci0 → …/URT0…/bluetooth/hci0` | The serdev device attached via the Apple UART. |
| `ls /lib/firmware/brcm/ \| grep hcd` | `BCM-0a5c-6410.hcd` present | The patchram firmware is installed. |
| Controller `BD_ADDR` after fix | `F4:0F:24:…` (Apple OUI) | A **real Apple address**, not a default/zero — i.e. the patchram firmware **loaded correctly**. |

If firmware had failed to load, the controller would typically not enumerate at all, or come up with a bogus/default address. It came up with its real Apple BD_ADDR. The radio was fine.

## Root cause: Arch ships Bluetooth inert (no CLI, daemon disabled)

Two distribution defaults, nothing hardware-specific:

1. **`bluez` is installed but `bluez-utils` is not.** `bluez` provides the `bluetoothd` daemon; **`bluez-utils` provides `bluetoothctl`** (and `btmgmt`, `bluetoothd`'s companions). Without it you have no command-line way to power on, scan, or pair.
2. **`bluetooth.service` is `disabled` + `inactive`.** Arch does not enable the Bluetooth daemon by default. With the daemon down, even a healthy `hci0` does nothing useful — no agent to manage pairing/connection, and most desktop BT panels stay empty.

That's the whole story. The hardest part of this fix was *not* over-engineering it.

## The fix — two commands

```sh
sudo pacman -S --needed bluez-utils
sudo systemctl enable --now bluetooth
```

`--now` both enables the unit (so it starts at boot) and starts it immediately.

### Verify

```sh
bluetoothctl show          # expect: Powered: yes, PowerState: on, a Controller XX:XX:… line
```

Then the real acceptance test — actually pair a device (here, Bose headphones for A2DP):

```sh
bluetoothctl --timeout 15 scan on    # let devices appear
bluetoothctl devices                 # find your device's MAC
bluetoothctl pair  <MAC>
bluetoothctl trust <MAC>             # auto-reconnect on future boots
bluetoothctl connect <MAC>
```

For audio devices, confirm the sink shows up and routes:

```sh
pactl list sinks short | grep -i bluez    # a bluez_output.* sink appears once connected
```

A `Powered: yes` controller is *not* the finish line — **pairing and using a real device is.** Here: paired Bose headphones, A2DP sink appeared, music played over Bluetooth.

## Lessons

- **"No `bluetoothctl`" usually means missing `bluez-utils`, not missing hardware.** `bluez` ≠ `bluez-utils`; you need both.
- **Check rfkill + `/sys/class/bluetooth/` + firmware presence before assuming a driver problem.** If `hci0` exists, isn't blocked, and the controller reports a real (non-default) BD_ADDR, the radio is fine — look at the daemon and tooling.
- **Enable the daemon explicitly** — Arch leaves `bluetooth.service` disabled; `systemctl enable --now bluetooth` is the step people skip.
- **Not every Apple radio needs out-of-tree heroics.** Touch Bar, Wi-Fi, and audio on this model did; Bluetooth did not. Diagnose each on its own evidence instead of assuming the pattern repeats — the cheapest fix is the one you almost over-engineered past.

---

*Written up by [Nori Nishigaya](https://github.com/xeeban) · Victoria, BC. More at [Emergent Insights](https://emergentinsights.substack.com/).*
