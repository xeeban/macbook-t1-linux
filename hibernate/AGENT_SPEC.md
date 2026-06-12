# Agent Spec — Set up & test hibernate (S4) on a T1 MacBook Pro (Linux)

**Purpose:** Hand this file to a coding agent (Claude Code, etc.) and say:

> *"Follow `AGENT_SPEC.md` to set up and test hibernate on my MacBook. Stop at every GO/NO-GO gate, run the staged `pm_test` dry-run before any real hibernate, and never run a hibernate test while I have unsaved work — a failed resume cold-boots and loses the session."*

This is a complete, self-contained spec + plan with verification gates and guardrails that encode mistakes already paid for. **Read the whole file before running anything.** It assumes the [`sleep-resume/`](../sleep-resume/) fix is already in place — hibernate reuses its Wi-Fi + Touch Bar hooks.

---

## 0. Mission & definition of done

**Goal:** `sudo systemctl hibernate` writes a memory image, powers the machine fully off, and on power-on **restores the exact session** — filesystem intact, Wi-Fi reconnected, Touch Bar lit.

**Done when ALL are true (after a real `systemctl hibernate` + power-on):**
- [ ] The pre-hibernate session is **restored** (windows/editor as left), not a fresh login.
- [ ] `journalctl -k -b | grep -c 'hibernation exit'` ≥ 1.
- [ ] **No** `nvme[0-9].*(timeout|fail|reset|Identify)` in `dmesg` around resume.
- [ ] `mount | grep ' / '` still `rw`.
- [ ] Wi-Fi reconnects; **human confirms** Touch Bar lit + keyboard/trackpad respond.
- [ ] Survives 2 hibernate/resume cycles.

**The agent cannot self-certify the physical checks or that "the session was restored."** Pause and ask.

---

## 1. Preconditions — verify BEFORE touching anything

```sh
cat /sys/devices/virtual/dmi/id/product_name   # expect MacBookPro13,2 (or T1-class)
free -h                                         # note RAM → swap must be ≥ this
swapon --show                                   # existing swap? size?
findmnt -no FSTYPE,UUID -T /                     # root fs type + UUID (swapfile host)
grep '^HOOKS' /etc/mkinitcpio.conf              # systemd hook? (decides resume wiring)
cat /sys/power/state                            # must include 'disk' (S4 available)
grep -o 'resume=[^ ]*' /proc/cmdline            # any pre-existing resume= ?
# bootloader: detect which one
ls /boot/refind_linux.conf /etc/default/grub /boot/loader/entries/*.conf 2>/dev/null
```

**Hard gate P-1 (S4 available):** `/sys/power/state` must contain `disk`. If not, hibernation is disabled in the kernel/lockdown — stop and report.

**Hard gate P-2 (suspend fix present):** Confirm the [`sleep-resume/`](../sleep-resume/) Wi-Fi + Touch Bar `systemd-sleep` hooks exist (`ls /usr/lib/systemd/system-sleep/`). If absent on a T1 with brcmfmac + Touch Bar, **do that spec first** — hibernate resume will otherwise wedge Wi-Fi/Touch Bar.

**Hard gate P-3 (filesystem):** This spec covers **ext4** swapfiles. If root is **btrfs**, the swapfile needs `chattr +C` + a different offset method (`btrfs inspect-internal map-swapfile`) — capture `findmnt -no FSTYPE -T /` and report before proceeding.

**Hard gate P-4 (safety):** Confirm with the human that **all work is saved** and a hard power-down (10-second power-button hold) is acceptable for each test. Do not run any hibernate test otherwise.

---

## 2. Background the agent must hold in context

- **Why hibernate, not suspend:** on this T1, `s2idle` resume is fixed for short cycles but a multi-hour real-world idle can still wedge the Apple NVMe controller (it can't recover from a held deep-idle state). **Hibernate powers fully off → cold NVMe init on resume → the bug has no surface.** Do not "fix" a hibernate problem by reverting to plain suspend.
- **Swapfile resume target:** for a **swapfile**, `resume=` is the **UUID of the filesystem containing the file** and `resume_offset=` is the file's **first physical extent** (`filefrag -v` extent `0:`, in fs blocks). **Not** the swapfile's own swap-signature UUID — that is only for swap *partitions*. Getting this wrong is the #1 failure.
- **Initramfs wiring depends on the hook:** **`systemd`** hook → reads `resume=` off cmdline automatically, **no `resume` hook**. **busybox/`base`** → must add `resume` to `HOOKS` after `udev` and rebuild. Check `mkinitcpio.conf` and do the right one; never add `resume` alongside `systemd`.
- **`pm_test=devices` is the free dry-run:** it cycles device suspend/resume and **auto-wakes in ~5 s without writing an image or powering off.** Always run it before a real hibernate, and **always reset `pm_test` to `[none]`** afterward — left set, a "real" hibernate silently no-ops.
- **Benign resume noise** (do not flag as failure): `usb … error -62`, `hid-generic … driver_sysfs_add failed`, `brcmfmac … Apple…bin failed -2` followed by a successful generic firmware load.

---

## 3. Plan overview (phases with gates)

```
A. Swap + resume= + initramfs  → B. Reboot & verify kernel sees target  🚦
C. Staged pm_test=devices dry-run (no data risk)                         🚦
D. Real systemctl hibernate (human present, work saved)                  🚦
E. Reboot-stress + human check                                          🚦
F. Hygiene / follow-up
```

Each 🚦 is a hard GO/NO-GO.

---

## 4. The phases

### Phase A — Swap, resume target, initramfs

**A.1 — Ensure swap ≥ RAM.** If `swapon --show` is empty or too small:
```sh
sudo fallocate -l 8G /swapfile && sudo chmod 600 /swapfile
sudo mkswap /swapfile && sudo swapon /swapfile
grep -q '/swapfile' /etc/fstab || echo '/swapfile none swap defaults 0 0' | sudo tee -a /etc/fstab
# back up fstab first if you edit it:  sudo cp /etc/fstab /etc/fstab.bak-prehibernate-$(date +%Y%m%d-%H%M%S)
```

**A.2 — Compute resume target + add to cmdline.**
```sh
ROOT_UUID=$(findmnt -no UUID -T /swapfile)
OFFSET=$(sudo filefrag -v /swapfile | awk '$1=="0:"{print $4}' | tr -d '.')
echo "resume=UUID=$ROOT_UUID resume_offset=$OFFSET"
```
Add `resume=UUID=$ROOT_UUID resume_offset=$OFFSET` to the kernel cmdline, **backing up the bootloader config first**:
- **rEFInd:** `sudo cp /boot/refind_linux.conf{,.bak-prehibernate-$(date +%Y%m%d-%H%M%S)}`, then append to the active entry's args (the boot line you actually use).
- **GRUB:** add to `GRUB_CMDLINE_LINUX_DEFAULT` in `/etc/default/grub`, then `sudo grub-mkconfig -o /boot/grub/grub.cfg`.
- **systemd-boot:** add to `options` in the active `/boot/loader/entries/*.conf`.

**A.3 — Initramfs.** If `HOOKS` contains `systemd`: **nothing to do.** Else add `resume` after `udev` in `HOOKS=(…)` and `sudo mkinitcpio -P`.

### Phase B — Reboot & verify the kernel sees the target 🚦
```sh
sudo reboot
# after reboot:
grep -o 'resume=[^ ]*\|resume_offset=[^ ]*' /proc/cmdline   # both present
cat /sys/power/resume_offset                                # == $OFFSET
cat /sys/power/state                                        # includes 'disk'
swapon --show                                               # swapfile active
```
**GATE B-1:** `/sys/power/resume_offset` must equal the computed offset and `resume=` must be on the cmdline. If `resume_offset` is `0` or mismatched, the kernel will not find the image — recompute (A.2) and fix the cmdline before going further. **Do not hibernate until this matches.**

### Phase C — Staged `pm_test=devices` dry-run (no data risk) 🚦
```sh
echo devices | sudo tee /sys/power/pm_test
sudo systemctl hibernate            # exercises device suspend/resume, auto-wakes ~5s, NO image/poweroff
sudo journalctl -k -b | grep -iE "fail|error|brcmf|nvme|timed out" | tail -40
echo none | sudo tee /sys/power/pm_test
cat /sys/power/pm_test              # MUST read [none]
```
**GATE C-1:** Machine auto-wakes cleanly and the journal shows no driver wake failures in the test window (ignore the §2 benign-noise lines). `pm_test` confirmed back to `[none]`. If a device fails to come back here, it will fail in a real hibernate too — fix it (usually a missing/incorrect `systemd-sleep` hook) before Phase D.

### Phase D — Real `systemctl hibernate` 🚦
**Human present, all work saved.**
```sh
cat /sys/power/pm_test       # must be [none]
sync; sync
sudo systemctl hibernate     # writes image → powers OFF; human presses power to resume
```
After power-on, verify §0 + the journal:
```sh
journalctl -k -b | grep -iE 'hibernation entry|hibernation exit|Restarting tasks'
sudo dmesg | grep -icE 'nvme[0-9].*(timeout|fail|reset|Identify)'   # 0
mount | grep ' / '                                                  # rw
cat /sys/class/net/*/operstate                                       # wifi up
```
**GATE D-1:** `hibernation exit` present, zero nvme errors, fs `rw`, **and the human confirms the session was restored + Touch Bar/Wi-Fi work** → GO. If it **cold-booted** (fresh login, no restore): the kernel rejected the image — re-verify GATE B-1 (offset mismatch is the usual cause) and that swap was active *before* hibernate. If it **hung** (no resume after ~90 s): human force-powers-off; at the rEFInd menu edit the boot entry (F2/Insert) and append ` noresume` to get a clean session; then discard the stale image: `sudo swapoff /swapfile && sudo mkswap /swapfile && sudo swapon /swapfile`. One real hibernate test per session.

### Phase E — Reboot-stress + human check 🚦
- Run a **second** hibernate/resume cycle to prove it's repeatable, not a one-off.
- **GATE E-1 (human):** Session restored both times; Touch Bar lit, keyboard/trackpad and Wi-Fi fine after each.

### Phase F — Hygiene
- Confirm `swapon --show` and the `resume=` cmdline persist across a clean reboot (proves fstab + bootloader edits stuck).
- Remove any scratch files; keep the `.bak-prehibernate-*` backups until the human is satisfied.

### Phase G — (Optional, recommended) suspend-then-hibernate 🚦
Only after plain hibernate is validated. Gives instant resume for short breaks and automatic, wedge-proof S4 for long absences — the s2idle phase is *capped* at `HibernateDelaySec`, so the machine never idles long enough to hit the multi-hour wedge.

**G.1 — Delay** (`/etc/systemd/sleep.conf.d/10-suspend-then-hibernate.conf`):
```ini
[Sleep]
HibernateDelaySec=30min
HibernateOnACPower=yes
```
**G.2 — Validate the STH mechanism with a SHORT delay first.** Temporarily set `HibernateDelaySec=2min`, then (work saved, human present):
```sh
cat /sys/power/pm_test            # [none]
sudo systemctl suspend-then-hibernate
# expect: s2idle → auto-wake at ~2min → hibernate (powers off) → power-on resumes
journalctl -b | grep -iE "Performing sleep operation|suspend entry|suspend exit|hibernation entry|hibernation exit" | tail
```
**GATE G-1:** The journal must show the **two-phase handoff** — a `suspend` op (`PM: suspend entry (s2idle)` → `suspend exit`) *followed by* a `hibernate` op (`hibernation entry` → `hibernation exit`) — and the human confirms the session restored. If it only suspended (no hibernate phase), `HibernateOnACPower`/delay is wrong; if it wedged, treat as a hibernate failure (Phase D recovery). Then raise the delay to the production value (e.g. `30min`).

**G.3 — Triggers (lid + idle).** `/etc/systemd/logind.conf.d/10-suspend-then-hibernate.conf`:
```ini
[Login]
HandleLidSwitch=suspend-then-hibernate
HandleLidSwitchExternalPower=suspend-then-hibernate
HandleLidSwitchDocked=ignore
IdleAction=suspend-then-hibernate
IdleActionSec=15min
```
Apply: `sudo systemctl restart systemd-logind` (preserves the session on systemd ≥256; reboot if unsure).

**G.4 — On GNOME, disable GNOME's competing idle-suspend** (it only offers plain suspend/hibernate, not STH, and races logind):
```sh
gsettings set org.gnome.settings-daemon.plugins.power sleep-inactive-ac-type 'nothing'
gsettings set org.gnome.settings-daemon.plugins.power sleep-inactive-battery-type 'nothing'
```
**GATE G-2 (verify triggers):** `HandleLidSwitch` is logind-owned and dependable — confirm with a real lid-close → reopen. **`IdleAction` under GNOME/Wayland is the uncertain one:** test with `IdleActionSec=2min`, leave the machine untouched, confirm it sleeps (`journalctl -b | grep -i 'Performing sleep operation'`), then restore `15min`. If idle won't fire, fall back to GNOME idle → `'hibernate'` and report the limitation rather than leaving idle unprotected.

---

## 5. Guardrails (the expensive lessons, collected)

1. **Never hibernate-test with unsaved work or unattended.** A failed resume cold-boots; the session is the only thing lost — keep it worthless to lose.
2. **Always run `pm_test=devices` first, and always reset it to `[none]`.** A left-set `pm_test` makes "real" hibernate silently no-op.
3. **Swapfile resume = host-fs UUID + `filefrag` offset.** Not the swap signature UUID. Verify `/sys/power/resume_offset` matches after reboot.
4. **`systemd` initramfs hook → no `resume` hook.** Don't add both.
5. **Don't revert to plain suspend to "fix" hibernate.** Long `s2idle` is the fragile path on this hardware; hibernate is the robust one.
6. **Ignore the benign resume noise** (`usb -62`, `hid-generic driver_sysfs_add`, `brcmfmac Apple .bin -2` + generic fallback). Don't chase them.
7. **Swap ≥ RAM.** Image is compressed but size for the worst case.

## 6. Rollback
```sh
# remove resume= from the cmdline (restore bootloader backup or delete the args), then:
sudo swapoff /swapfile && sudo rm /swapfile
sudo cp /etc/fstab.bak-prehibernate-<STAMP> /etc/fstab    # or just delete the /swapfile line
sudo reboot
```

## 7. What to report back to the human
A short status with: RAM/swap sizes, root fs type + UUID, the computed `resume_offset` and confirmation `/sys/power/resume_offset` matched after reboot, which initramfs path applied (systemd vs. resume hook), the staged `pm_test` result, the real-hibernate result (hibernation-exit present, nvme error count, fs state), and an explicit request for the human's "session restored + Touch Bar + Wi-Fi" confirmation. Surface anything that contradicts this spec (btrfs root, offset not sticking, a device that won't resume in the staged test) instead of forcing the steps.
