# Agent Spec — Make suspend/resume work on a T1 MacBook Pro (Linux)

**Purpose:** Hand this file to a coding agent (Claude Code, etc.) and say:

> *"Follow `AGENT_SPEC.md` to make my MacBook resume from sleep. Stop at every GO/NO-GO gate, and never run a suspend test unattended — wake failures here force a hard power-down."*

This is a complete, self-contained spec + plan, written so an autonomous agent can execute it deterministically with verification gates and guardrails that encode mistakes already paid for. **Read the whole file before running anything.**

---

## 0. Mission & definition of done

**Goal:** The machine enters `s2idle` and **resumes cleanly** — from `rtcwake`, from `systemctl suspend`, and from a real lid-close — with the filesystem intact, Wi-Fi reconnected, and the Touch Bar lit.

**Done when ALL are true (after a real `systemctl suspend`):**
- [ ] `/sys/power/suspend_stats/success` incremented; `fail` did **not**.
- [ ] `journalctl -k -b 0 | grep -c 'PM: suspend exit'` ≥ 1.
- [ ] `mount | grep ' / '` still shows `rw`.
- [ ] `sudo dmesg | grep -icE 'nvme[0-9].*(timeout|fail|reset|Identify)'` → `0`.
- [ ] `wlan0` (or your iface) is `up` and reconnects.
- [ ] **Human confirms**: keyboard + trackpad respond, Touch Bar is lit.
- [ ] Survives 2–3 suspend/resume cycles.

**The agent cannot self-certify the physical checks or safely run an unattended suspend.** Pause and ask.

---

## 1. Preconditions — verify BEFORE touching anything

```sh
cat /sys/devices/virtual/dmi/id/product_name      # expect MacBookPro13,2 (or T1-class 14,x)
cat /sys/power/mem_sleep                           # expect [s2idle] selected; if [deep], see §2
lspci | grep -i nvme                               # note the NVMe address + vendor:device
lspci -nn | grep -i nvme                           # confirm vendor:device — this spec targets 106b:2003 (Apple S3X)
cat /sys/power/suspend_stats/success /sys/power/suspend_stats/fail
journalctl -k -b -1 | grep -c 'PM: suspend exit'  # is there ANY prior successful resume?
```

**Hard gate P-1 (hardware):** This spec targets the **Apple S3X NVMe controller `106b:2003`** on a T1 MacBook. If your NVMe is a *different* vendor/device, the root cause below may not apply — capture `lspci -nn -vv` for the NVMe and report before proceeding.

**Hard gate P-2 (safety):** Confirm with the human that **they are physically present** and that a hard power-down (forced 10-second power-button hold) is acceptable for each test. Do not run any suspend test otherwise.

---

## 2. Background the agent must hold in context

- **Sleep state:** Intel Macs have broken **S3 ("deep")** resume in firmware. **`s2idle` is correct.** If `mem_sleep` shows `[deep]`, force s2idle with `mem_sleep_default=s2idle` on the kernel cmdline. Do **not** "fix" resume by switching to deep.
- **The real bug:** `nvme_suspend()` (`drivers/nvme/host/pci.c`) keeps the controller powered across `s2idle` unless one of `{pm_suspend_via_firmware, !npss, !pcie_aspm_enabled, NVME_QUIRK_SIMPLE_SUSPEND}` holds. `106b:2003` has none → keep-alive branch → the Apple controller can't recover on wake → `I/O timeout → Identify -4 → reset -5` → root fs goes read-only → hard hang.
- **Two famous dead ends:**
  - `pcie_aspm=off` is a **silent no-op** on Macs (firmware withholds the ASPM `_OSC`; kernel sets `aspm_disabled`). Verify with `lspci -vv` if tempted — it won't change ASPM state.
  - The **Touch Bar driver UAF is a red herring** for resume. It's a real bug, but unloading the Touch Bar stack before suspend and watching it *still* wedge proves it is not the resume blocker. Don't rabbit-hole there.
- **`rtcwake -m mem` bypasses `systemd-sleep` hooks.** It validates the NVMe fix but **not** the Wi-Fi/Touch-Bar resume hooks. Use `systemctl suspend` for those.
- **Evidence dies with the disk.** When the root fs goes read-only on a failed resume, `journald` can't record it. Use `no_console_suspend` and have the human photograph the panel.

---

## 3. Plan overview (phases with gates)

```
A. Apply NVMe fix (cmdline + udev)  → B. Reboot & verify knobs took  🚦
C. Gated rtcwake test (NVMe only)   → D. Add Wi-Fi + Touch Bar hooks  🚦
E. Real systemctl suspend test      → F. Reboot-stress + human check  🚦
G. Hygiene / cleanup
```

Each 🚦 is a hard GO/NO-GO. A failed suspend wedges the machine — never cross a gate on assumption.

---

## 4. The phases

### Phase A — Apply the NVMe fix

**A.1 — Kernel command line.** Add `intel_idle.max_cstate=1 nvme_core.default_ps_max_latency_us=0` (and ensure `mem_sleep_default=s2idle`). Edit the bootloader config; **back it up first**. For rEFInd:
```sh
sudo cp /boot/refind_linux.conf /boot/refind_linux.conf.bak
# Edit line 1's kernel args to include:
#   mem_sleep_default=s2idle intel_idle.max_cstate=1 nvme_core.default_ps_max_latency_us=0
```
For GRUB: edit `GRUB_CMDLINE_LINUX_DEFAULT` in `/etc/default/grub`, then `sudo grub-mkconfig -o /boot/grub/grub.cfg`. For systemd-boot: the relevant entry in `/boot/loader/entries/*.conf`.

**A.2 — D3cold udev rule** (substitute your NVMe vendor:device if not Apple's):
```sh
sudo tee /etc/udev/rules.d/99-nvme-d3cold-resume-fix.rules >/dev/null <<'EOF'
ACTION=="add", SUBSYSTEM=="pci", ATTR{vendor}=="0x106b", ATTR{device}=="0x2003", ATTR{d3cold_allowed}="0"
EOF
```

### Phase B — Reboot & verify the knobs took 🚦
```sh
sudo reboot
# after reboot:
grep -o 'intel_idle.max_cstate=1' /proc/cmdline                 # present
cat /sys/module/nvme_core/parameters/default_ps_max_latency_us  # 0
cat /sys/bus/pci/devices/<NVME_ADDR>/d3cold_allowed             # 0  ← udev rule fired
```
**GATE B-1:** All three must read as above. If `d3cold_allowed` is `1`, the udev rule didn't match — debug with `udevadm test /sys/bus/pci/devices/<NVME_ADDR>` and check the vendor/device strings. **Do not suspend until d3cold reads 0.**

### Phase C — Gated `rtcwake` test (proves the NVMe fix) 🚦
Quiesce first so a wedge can't corrupt much, and so there's nothing to lose if the fs goes read-only:
```sh
# Close editors/indexers/git. Then:
sync; sync; sync
sudo journalctl --flush
echo 3 | sudo tee /proc/sys/vm/drop_caches
sudo rtcwake -m mem -s 60          # self-wakes after 60s; resume can take ~1 min
```
After it returns, verify:
```sh
cat /sys/power/suspend_stats/success                                # incremented
mount | grep ' / '                                                 # rw
sudo dmesg | grep -icE 'nvme[0-9].*(timeout|fail|reset|Identify)'  # 0
journalctl -k -b 0 | grep -c 'PM: suspend exit'                    # ≥1
```
**GATE C-1:** If it wakes clean → **GO to Phase D.** If it **wedges** (no wake in ~90s): the human hard-powers-off; cold boot recovers the fs. The NVMe combo didn't take — re-verify Phase B, then try the alternative `nvme.quirks=106b:2003:simple_suspend` on the cmdline (reboot, re-verify with `dmesg | grep -i quirk`), and retest. Only **one** suspend test per session; each wedge costs a power cycle.

### Phase D — Add the resume hooks (Wi-Fi + Touch Bar)
These fix devices that don't survive resume. Install both into `/usr/lib/systemd/system-sleep/` (mode `0755`):

**D.1 — Wi-Fi (`brcmfmac`) unbind/rebind** — substitute your Wi-Fi PCI address for `DEV`:
```sh
sudo tee /usr/lib/systemd/system-sleep/60-brcmfmac-wifi.sh >/dev/null <<'EOF'
#!/bin/bash
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
> Why unbind, not `modprobe -r`: NetworkManager/wpa_supplicant hold the module, so `modprobe -r brcmfmac` fails. PCI unbind/rebind works and re-probes fresh firmware.

**D.2 — Touch Bar USB-reset + reload.** Copy [`50-apple-ibridge-touchbar.sh`](./50-apple-ibridge-touchbar.sh) from this repo to `/usr/lib/systemd/system-sleep/` (mode `0755`). It unloads `apple_touchbar`+`apple_ibridge` in `pre`, and in `post` **deauthorizes→reauthorizes** the iBridge USB device (`05ac:8600`) before reloading — a plain reload leaves the firmware endpoint stuck (`tb: hw open failed -19`). Skip this if the machine has no Touch Bar.

**GATE D-2:** Both hooks present, executable (`0755`), and `bash -n` clean.

### Phase E — Real `systemctl suspend` test (runs the hooks) 🚦
```sh
sync; sync; sync; sudo journalctl --flush
sudo rtcwake -m no -s 120          # arm RTC wake WITHOUT suspending
sudo systemctl suspend             # real suspend → fires both systemd-sleep hooks
```
After the RTC wakes it (~2 min), verify everything from §0, plus that the hooks ran:
```sh
journalctl -t brcmfmac-sleep-hook -b 0          # unbound … / rebound …
journalctl -b 0 | grep apple-ibridge            # touchbar hook logs under the systemd-sleep identifier
```
> ⚠️ The Touch Bar hook logs via stdout (captured as `systemd-sleep[...]`), **not** a `logger` tag — `journalctl -t apple-ibridge-sleep-hook` will find nothing. Grep `apple-ibridge` instead.

**GATE E-1:** All §0 checks green and both hooks logged → **GO.** If suspend **fails to enter** (`rtcwake`/suspend errors, `fail` increments): read `/sys/power/suspend_stats/last_failed_dev` — a Wi-Fi address means the hook didn't run (check exec bit + logs); a Touch Bar device means its reload path is suspect. If it **wedges**: hard power-off; the NVMe fix still stands (the regression is hook-side).

### Phase F — Reboot-stress + human check 🚦
- Reboot once; confirm `d3cold_allowed` is still `0` on a clean boot (proves the udev rule, not a one-off).
- Run 2–3 suspend/resume cycles.
- **GATE F-1 (human):** Confirm Touch Bar lit, keyboard/trackpad work, Wi-Fi reconnects after each. Then optionally restore auto-suspend: `gsettings set org.gnome.settings-daemon.plugins.power sleep-inactive-ac-type 'suspend'` and ensure `sleep.target` etc. are **not** masked (`systemctl status sleep.target`).

### Phase G — Hygiene / cleanup
- If you masked sleep targets while debugging, unmask them: `sudo systemctl unmask sleep.target suspend.target hibernate.target hybrid-sleep.target suspend-then-hibernate.target`.
- Remove any temporary passwordless-sudo grant created for unattended steps; validate with `visudo -c`; never leave `NOPASSWD: ALL` behind.
- Drop debug-only kernel args (`no_console_suspend`, `ignore_loglevel`) and restore `quiet` once resume is reliable. **Keep** the three fix knobs.
- Remove scratch files and config backups you created (after confirming the fix sticks across a reboot).

---

## 5. Guardrails (the expensive lessons, collected)

1. **Never suspend unattended.** A failed resume = forced power-down. Human present, every test.
2. **One suspend test per session.** Quiesce (`sync` + `journalctl --flush` + `drop_caches`) before each.
3. **`rtcwake` ≠ real suspend.** It bypasses `systemd-sleep` hooks — only `systemctl suspend`/lid-close exercises the Wi-Fi/Touch-Bar hooks.
4. **`pcie_aspm=off` is a no-op on Macs.** Don't count it as a variable; `lspci -vv` proves it.
5. **The Touch Bar driver is a red herring for resume.** Prove it innocent once (unload-before-suspend) and move on.
6. **Deep sleep is the wrong direction.** Keep `s2idle`.
7. **Capture the panel.** `no_console_suspend` + a camera; the fs death erases the journal.
8. **Verify `d3cold_allowed=0` on a clean boot before trusting the fix** — the runtime attr resets each boot; only the udev rule makes it persist.

## 6. Rollback
```sh
sudo cp /boot/refind_linux.conf.bak /boot/refind_linux.conf       # restore cmdline (adapt per bootloader)
sudo rm /etc/udev/rules.d/99-nvme-d3cold-resume-fix.rules
sudo rm /usr/lib/systemd/system-sleep/60-brcmfmac-wifi.sh \
        /usr/lib/systemd/system-sleep/50-apple-ibridge-touchbar.sh
sudo reboot
```
If resume is unreliable and you must ship the machine, mask the sleep targets and configure "lock + screen-blank, never suspend" so a stray lid-close can't wedge it.

## 7. What to report back to the human
A short status with: NVMe vendor:device confirmation, which fix path you used (APST/D3cold combo vs. `simple_suspend` quirk), the post-reboot knob verification, the result of each gated suspend test (success/fail counters, nvme error count, fs state), whether both hooks logged, and an explicit request for the physical Touch-Bar/Wi-Fi confirmation. Surface anything that contradicts this spec (different NVMe, `d3cold` not sticking, a hook device in `last_failed_dev`) instead of forcing the steps.
