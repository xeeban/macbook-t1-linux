# Fixing suspend/resume on a T1 MacBook Pro (Linux)

Why this 2016 MacBook Pro never came back from sleep under Linux — and why the obvious suspect (the Touch Bar driver) was a **red herring** that cost real time.

> **TL;DR** — On a **MacBookPro13,2 (T1)** running Arch Linux (kernel `7.0.10`, `s2idle`), the machine **never resumed** from sleep: it would enter suspend and then wedge, requiring a hard power-down (which dropped the filesystem to read-only). The real cause is **not** the sleep state and **not** the Touch Bar driver. It's the **Apple S3X NVMe controller** (`pci 106b:2003`): on `s2idle` the kernel leaves it powered in a deep state it cannot recover from, so on wake the controller falls off the PCIe bus (`nvme … I/O timeout → Identify -4 → reset failure -5`), the root filesystem dies, and the box hangs. The fix is three knobs — **`intel_idle.max_cstate=1`** + **`nvme_core.default_ps_max_latency_us=0`** on the kernel command line, plus a udev rule pinning **`d3cold_allowed=0`** on the NVMe device. Two small `systemd-sleep` hooks then handle a Wi-Fi and a Touch Bar quirk that only become visible *once resume works at all*.

> **Status:** ✅ **SOLVED & validated 2026-06-05.** First clean `s2idle` suspend/resume in this machine's Linux life, confirmed across three `rtcwake` cycles and a real `systemctl suspend` round-trip (lid-close path), with the Touch Bar and Wi-Fi both alive on wake.

> **Want your agent to do this for you?** Point it at [`AGENT_SPEC.md`](./AGENT_SPEC.md) — a self-contained, gated plan.

This is part of [the T1 MacBook Pro on Linux journey](../) and a sequel to [the Touch Bar fix](../touch-bar/).

---

## The machine

- **MacBookPro13,2** — 2016 13" MacBook Pro with the **T1** Touch Bar, repurposed as a distraction-free "writer's deck."
- **Arch Linux**, GNOME on Wayland, kernel **`7.0.10-arch1-1`**.
- Sleep mode: **`s2idle`** (forced with `mem_sleep_default=s2idle`). On Intel Macs `s2idle` is correct — S3 ("deep") resume is broken in Apple's firmware.
- **NVMe:** Apple S3X controller at PCI `0000:01:00.0`, ID **`106b:2003`**.
- **Wi-Fi:** Broadcom BCM43602 (`brcmfmac`) at PCI `0000:02:00.0`.

## The symptom

Every real suspend ended the same way: screen off, then nothing. No wake on key, lid, or power button — only a forced power-down brought it back, and because the filesystem had gone read-only mid-crash, the next boot replayed the journal. In the kernel log, **every** suspend boot ended at `PM: suspend entry` with **no matching `PM: suspend exit`** — resume had *never once succeeded* on this machine, in either `s2idle` or `deep`.

## ⚠️ The red herring: don't chase the Touch Bar driver

The out-of-tree `apple_ibridge` Touch Bar driver has a genuine **use-after-free** in its HID suspend callback (`appleib_hid_suspend()` walks a freed sub-device — see the decode in [the Touch Bar writeup](../touch-bar/)). It Oopses a PM worker, and it *looks* exactly like a resume bug. We patched it. **The machine still wouldn't resume.**

The decisive test: unload the Touch Bar stack **before** suspending, so the buggy callback can't run —

```sh
sudo modprobe -r apple_touchbar apple_ibridge   # confirm both gone
sudo rtcwake -m mem -s 30
```

— and it **still wedged**. That rules the Touch Bar out as the resume blocker. It was a real but *parallel* bug. **If you're here for resume, do not spend time on the iBridge driver.** (You'll still want a Touch-Bar-on-resume hook later — see "Two more quirks" — but that's polish, not the fix.)

**Lesson #1: prove the suspect is guilty before you sentence it.** One unload-before-suspend test would have saved days.

## Reading the real crash

The breakthrough came from one kernel command-line change — `intel_idle.max_cstate=1` (limit CPU idle depth) — plus `no_console_suspend` so the panel would show a failure even after the disk died. With those, resume got **further than ever before**: cleanly through device-resume (Wi-Fi firmware reload, efivarfs resync), through a **full task thaw** (`Restarting tasks: Done`, `OOM killer enabled`) — and *then* died:

```
nvme nvme0: I/O tag 28 (1014) QID 0 timeout, disable controller
nvme nvme0: Identify Controller failed (-4)
nvme nvme0: Disabling device after reset failure: -5
EXT4-fs error … : Detected aborted journal
EXT4-fs (nvme0n1p2): Remounting filesystem read-only
```

The fault is in the **NVMe controller's resume re-init**, *after* every device and task callback has already succeeded. (This evidence existed only as a **phone photo** of the panel — once the root fs went read-only, `journald` lost its own disk and nothing persisted. If you debug this, keep a camera handy and use `no_console_suspend`.)

## Root cause: `nvme_suspend()` keeps the Apple controller alive across `s2idle`

The kernel decides how to put an NVMe controller to sleep in `nvme_suspend()` (`drivers/nvme/host/pci.c`):

```c
if (pm_suspend_via_firmware() || !ctrl->npss || !pcie_aspm_enabled(pdev) ||
    (ndev->ctrl.quirks & NVME_QUIRK_SIMPLE_SUSPEND))
    return nvme_disable_prepare_reset(ndev, true);  // ← SAFE: full shutdown + cold re-init on resume
// otherwise: keep the controller powered and rely on APST/ASPM low-power states
```

On this machine **none** of those conditions hold: it's `s2idle` (so not firmware-suspend), the controller advertises non-zero power states (`npss`), PCIe **ASPM is enabled**, and `106b:2003` carries **no `NVME_QUIRK_SIMPLE_SUSPEND`**. So the kernel takes the *keep-alive* path — and the Apple S3X cannot recover from the deep idle state it's left in. On wake it never answers, times out, and drops off the bus.

That's why it dies on resume and not on suspend, and why **duration didn't matter** — any real `s2idle` triggers the bad branch.

## The fix — three knobs

This is the [mbp-2016-linux](https://github.com/Dunedan/mbp-2016-linux) documented combo for the MacBookPro13,2, plus the idle-depth limit that first got resume off the ground:

**1 & 2 — kernel command line** (`intel_idle.max_cstate=1 nvme_core.default_ps_max_latency_us=0`):

- `intel_idle.max_cstate=1` — caps CPU C-states; load-bearing here (it's what first got resume past suspend-entry).
- `nvme_core.default_ps_max_latency_us=0` — **disables APST** (Autonomous Power State Transitions), keeping the controller in a shallower state it *can* wake from.

Add them to your bootloader's kernel line. On rEFInd (`/boot/refind_linux.conf`):

```
"Boot"  "root=LABEL=arch_root rw quiet mem_sleep_default=s2idle intel_idle.max_cstate=1 nvme_core.default_ps_max_latency_us=0"
```

**3 — disable D3cold for the NVMe device, persistently.** `d3cold_allowed` is a runtime sysfs attribute that resets to `1` every boot, so pin it with a udev rule matched by PCI vendor/device (not bus path):

```sh
sudo tee /etc/udev/rules.d/99-nvme-d3cold-resume-fix.rules >/dev/null <<'EOF'
# Apple S3X NVMe (106b:2003) wedges on s2idle resume unless D3cold is disabled.
# d3cold_allowed resets to 1 each boot, so pin it to 0 here.
ACTION=="add", SUBSYSTEM=="pci", ATTR{vendor}=="0x106b", ATTR{device}=="0x2003", ATTR{d3cold_allowed}="0"
EOF
```

Reboot, then **verify before you trust it**:

```sh
grep -o 'intel_idle.max_cstate=1' /proc/cmdline                   # present
cat /sys/module/nvme_core/parameters/default_ps_max_latency_us    # 0
cat /sys/bus/pci/devices/0000:01:00.0/d3cold_allowed              # 0  ← the udev rule fired
```

> Adjust `0000:01:00.0` to your NVMe's address (`lspci | grep -i nvme`). The udev rule keys on `106b:2003`, so it follows the device regardless of slot.

### Things that do *not* work (so you don't try them)

| Tempting knob | Reality |
|---|---|
| `pcie_aspm=off` | **No-op on Macs.** Apple firmware doesn't grant the OS ASPM control via ACPI `_OSC`, so the kernel sets `aspm_disabled` and **silently ignores** `pcie_aspm=off`. `lspci -vv` still shows `ASPM L1.2 Enabled`. To actually force it you'd need `pcie_aspm=force pcie_aspm.policy=performance` — but you don't need to: the APST + D3cold combo fixes it without touching ASPM. |
| `mem_sleep_default=deep` (S3) | **Backwards.** Intel Macs have broken S3 resume in firmware; `s2idle` is correct. |
| Swapping the NVMe SSD / cabling | It's a controller power-state interaction, not a hardware fault. |

### Alternative one-liner (if you'd rather force the safe branch directly)

This kernel's `nvme.ko` supports a named-quirk override. You can apply the missing quirk to *only* the Apple controller from the command line:

```
nvme.quirks=106b:2003:simple_suspend
```

That flips `nvme_suspend()` to the full-shutdown branch for that device. It's a clean alternative to the APST/D3cold combo (verify with `dmesg | grep -i quirk` showing no "unrecognized quirk"). We shipped the documented APST + D3cold combo because it's the tested-on-this-model recipe, but either should work.

## Two more quirks that only appear *after* resume works

Once the machine actually came back, two devices turned out not to survive the round-trip. Both are handled with `systemd-sleep` hooks (run automatically on real `systemctl suspend` / lid-close). **Note: `rtcwake` bypasses these hooks** — test them with a real suspend.

### Wi-Fi (`brcmfmac`) wedges its firmware on resume

After the first resume the BCM43602's msgbuf ring is stuck (`brcmf_msgbuf_tx_ioctl: Failed to reserve space in commonring`, scans fail with `-12`/ENOMEM), `wlan0` goes down, and — worse — the wedged device then returns `-5` from `.suspend` and **blocks the next suspend entirely**. `modprobe -r brcmfmac` fails (NetworkManager/wpa_supplicant hold it), so the working reset is a **PCI driver unbind/rebind**. Keep the card out of the suspend path (unbind in `pre`) and re-probe fresh firmware in `post`:

```sh
sudo tee /usr/lib/systemd/system-sleep/60-brcmfmac-wifi.sh >/dev/null <<'EOF'
#!/bin/bash
# BCM43602 brcmfmac firmware does not survive s2idle resume (commonring wedge,
# then -5 on .suspend blocks the next sleep). modprobe -r fails (module in use),
# so unbind the PCI device before sleep and rebind it after.
DEV="0000:02:00.0"
DRV="/sys/bus/pci/drivers/brcmfmac"
case "$1" in
  pre)  [ -e "$DRV/$DEV" ] && echo "$DEV" > "$DRV/unbind" 2>/dev/null
        logger -t brcmfmac-sleep-hook "unbound $DEV before $2" ;;
  post) [ ! -e "$DRV/$DEV" ] && [ -e "/sys/bus/pci/devices/$DEV" ] && echo "$DEV" > "$DRV/bind" 2>/dev/null
        logger -t brcmfmac-sleep-hook "rebound $DEV after $2" ;;
esac
exit 0
EOF
sudo chmod 0755 /usr/lib/systemd/system-sleep/60-brcmfmac-wifi.sh
```

(Set `DEV` to your Wi-Fi PCI address — `lspci | grep -i network`.) For the underlying *association* fix on this card, see the [Wi-Fi writeup](../wifi/).

### Touch Bar needs a USB power-cycle on resume

A plain `modprobe -r` / `modprobe` of the Touch Bar stack after resume leaves the iBridge firmware endpoint stuck (`apple-touchbar … tb: hw open failed (-19)` / ENODEV) and the bar dark. The fix is to **deauthorize → reauthorize** the iBridge USB device (`05ac:8600`) to force a clean re-enumeration before reloading. The full hook (find-by-VID/PID, unload in `pre`, USB-reset + reload in `post`) is in [`50-apple-ibridge-touchbar.sh`](./50-apple-ibridge-touchbar.sh):

```sh
sudo install -m0755 50-apple-ibridge-touchbar.sh /usr/lib/systemd/system-sleep/
```

## Validation

Validate **in person**, never unattended — a failed resume on this machine means a hard power-down.

```sh
# Self-waking suspend (bypasses the sleep hooks, but proves the NVMe fix):
sudo rtcwake -m mem -s 60
# Real suspend that ALSO runs the hooks (lid-close path):
sudo rtcwake -m no -s 120 && sudo systemctl suspend

# After wake, expect ALL of:
cat /sys/power/suspend_stats/success     # incremented
cat /sys/power/suspend_stats/fail        # NOT incremented
journalctl -k -b 0 | grep -c 'PM: suspend exit'                     # ≥1
mount | grep ' / '                                                  # still rw
sudo dmesg | grep -icE 'nvme[0-9].*(timeout|fail|reset|Identify)'   # 0
cat /sys/class/net/wlan0/operstate                                  # up
# and a human: Touch Bar lit, keyboard + trackpad respond.
```

On this machine: three clean `rtcwake` cycles (60s and 180s) plus a real `systemctl suspend` round-trip (suspended → RTC-woke 2 min later → both hooks fired → Touch Bar live, Wi-Fi reconnected, `success=1 fail=0`, zero nvme errors, root still `rw`).

### Benign noise you can ignore

During resume the panel shows Thunderbolt/PCIe complaints — `pcieport … Unable to change power state from D3cold to D0, device inaccessible`, `thunderbolt … not ready N ms after resume; giving up`, and even a `WARNING … tb_cfg_read` stack trace. These are **cosmetic** (Alpine Ridge controllers with nothing attached). A `WARNING` prints a trace but does not halt the kernel — if you also see `Restarting tasks` and the machine comes back, you're fine.

## Lessons

- **The symptom lied; the call trace didn't — twice.** "Bad resume" first pointed at deep-sleep, then at the Touch Bar driver. Reading the actual crash (NVMe controller off-bus) was the only thing that mattered.
- **Prove the suspect.** Unloading the Touch Bar before suspend — and watching it *still* fail — is what cleared the red herring. Cheap test, days saved.
- **Know which power layer you're in.** APST (intra-controller), ASPM (PCIe link), and D3cold (PCI device power) are different knobs. `pcie_aspm=off` being a silent no-op on Macs is a classic time-sink.
- **`rtcwake` ≠ a real suspend.** It bypasses `systemd-sleep` hooks, so it can't validate the Wi-Fi/Touch-Bar resume hooks — use `systemctl suspend` for those.
- **Capture the panel.** When the root fs dies on resume, the journal can't record the death. `no_console_suspend` + a phone camera was the only evidence.

## Upstream

The clean fix would be a `NVME_QUIRK_SIMPLE_SUSPEND` entry for `106b:2003` in the kernel's `nvme_id_table` (the same quirk other Apple controllers already carry). Until then, the command-line `nvme.quirks=106b:2003:simple_suspend` override or the APST/D3cold combo above are the user-space-only paths.

---

*Written up by [Nori Nishigaya](https://github.com/xeeban) · Victoria, BC. More at [Emergent Insights](https://emergentinsights.substack.com/).*
