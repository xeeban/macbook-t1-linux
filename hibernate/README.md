# Hibernate (S4) on a T1 MacBook Pro (Linux)

Why **hibernate** — not just suspend — is the move that finally made "step away and close the lid" safe on this machine, and the exact config that makes it resume reliably.

> **TL;DR** — On a **MacBookPro13,2 (T1)** running Arch (kernel `7.0.10`), `s2idle` suspend/resume is *fixed for short cycles* (see [`sleep-resume/`](../sleep-resume/)) — but a **multi-hour real-world idle still wedged the machine**, forcing a hard reboot. **Hibernate (S4) is the dependable answer for stepping away.** It writes RAM to a swapfile and **powers the machine fully off**, so on wake the NVMe controller gets a **cold init from power-on** instead of being asked to recover from the deep idle state that breaks `s2idle` — the very bug from the sleep-resume saga is *sidestepped, not inherited*. Setup is four pieces: an **8 GiB swapfile**, a **`resume=UUID=… resume_offset=…`** pair on the kernel command line, a **systemd initramfs** (which needs no separate `resume` hook), and the **same Wi-Fi + Touch Bar `systemd-sleep` hooks** the suspend fix already installed. First real `systemctl hibernate` round-trip: image written, powered off, **session fully restored in ~42 s on power-on, Touch Bar and Wi-Fi alive.**

> **Status:** ✅ **SOLVED & validated 2026-06-11.** Real `systemctl hibernate` → cold power-off → resume with the editor session, Wi-Fi, and Touch Bar all intact. Journal clean (`Restarting tasks: Done` + `PM: hibernation: hibernation exit`, no errors). Staged `pm_test=devices` dry-run passed first.

> **Want your agent to do this for you?** Point it at [`AGENT_SPEC.md`](./AGENT_SPEC.md) — a self-contained, gated plan.

This is part of [the T1 MacBook Pro on Linux journey](../) and a direct sequel to [the suspend/resume fix](../sleep-resume/).

---

## The machine

- **MacBookPro13,2** — 2016 13" MacBook Pro with the **T1** Touch Bar, repurposed as a distraction-free "writer's deck."
- **Arch Linux**, GNOME on Wayland, kernel **`7.0.10-arch1-1`**, **8 GB RAM**.
- **Bootloader:** rEFInd (`/boot/refind_linux.conf`).
- **Initramfs:** `mkinitcpio` with the **`systemd`** hook (matters — see [§ Initramfs](#initramfs-systemd-needs-no-resume-hook)).
- **Root fs:** ext4 on `nvme0n1p3`, UUID `c384f74c-…`. NVMe is the **Apple S3X `106b:2003`** controller from the suspend saga.
- Already on the kernel cmdline from the suspend fix: `mem_sleep_default=s2idle intel_idle.max_cstate=1 nvme_core.default_ps_max_latency_us=0`.

## Why hibernate when suspend is "fixed"?

The [suspend/resume writeup](../sleep-resume/) earns its ✅: after the NVMe APST/D3cold fix, `s2idle` resumes cleanly across `rtcwake` cycles and a real lid-close. **But "resumes from a 60-second `rtcwake`" and "survives being closed on the couch for three hours" are different claims.** On 2026-06-11 the machine was closed and walked away from for a few hours, and it **never woke** — black screen, no resume, hard power-down required (and the read-only-fs journal replay that comes with it). `s2idle` keeps the box in a shallow powered state the whole time; the longer and deeper the platform idles, the more chances for the Apple NVMe/PCIe power interaction to land somewhere it can't climb out of.

**Hibernate removes the variable entirely.** S4 snapshots RAM to disk and then does a *real ACPI power-off* — zero watts, nothing held across the gap. Resume is a normal cold boot of the hardware (firmware POST → NVMe cold-init → kernel) that then reads the image back and restores your session. The NVMe controller is never asked to "wake from deep idle"; it's initialized from power-on like any boot. That's the whole reason it's reliable where long `s2idle` is fragile.

> **Open question going in, now answered:** does hibernate's full hardware re-init *sidestep* the NVMe wake bug, or *inherit* it? **It sidesteps it.** Cold power-on init is exactly the path the controller is happy with.

## The setup — four pieces

### 1. Swap big enough for the image

Hibernate needs somewhere to write the memory image. A swapfile is simplest (no repartitioning); it just has to live on a filesystem the kernel can reach early and be large enough for the *used* pages (the image is compressed — this machine's 8 GB RAM produced a ~2.7 GB image — but size it to RAM to be safe).

```sh
sudo swapoff -a 2>/dev/null
sudo fallocate -l 8G /swapfile      # 8 GiB ≥ 8 GB RAM
sudo chmod 600 /swapfile
sudo mkswap /swapfile
sudo swapon /swapfile
# make it permanent:
echo '/swapfile none swap defaults 0 0' | sudo tee -a /etc/fstab
```
> On **btrfs**, a swapfile needs `chattr +C` (nodatacow) on an empty file and the offset read differently — out of scope here; this machine is ext4.

### 2. Tell the kernel where to resume from — `resume=` + `resume_offset=`

For a **swapfile** (not a swap *partition*) you point the kernel at the **filesystem that holds the file** (its UUID) plus the **physical block offset** of the file within that filesystem:

```sh
# UUID of the fs CONTAINING the swapfile (here, root):
findmnt -no UUID -T /swapfile                  # → c384f74c-71d2-4024-a6a7-9ad154e5df89
# physical offset of the swapfile's first extent:
sudo filefrag -v /swapfile | awk '$1=="0:"{print $4}' | tr -d '.'   # → 28698624
```

Add both to the kernel command line. On rEFInd, edit the active entry's args in `/boot/refind_linux.conf` (here the **"Boot writers-deck"** line) — **back it up first**:

```sh
sudo cp /boot/refind_linux.conf /boot/refind_linux.conf.bak-prehibernate-$(date +%Y%m%d-%H%M%S)
```
```
"Boot writers-deck"  "… mem_sleep_default=s2idle intel_idle.max_cstate=1 nvme_core.default_ps_max_latency_us=0 resume=UUID=c384f74c-71d2-4024-a6a7-9ad154e5df89 resume_offset=28698624"
```
> The `resume_offset` is in **filesystem blocks** (the value `filefrag` reports for extent `0:`). Don't confuse it with the swapfile's own swap-signature UUID (`d795cfc3-…` on this box) — that UUID is **not** used here; swapfiles resume by host-fs UUID + offset, only swap *partitions* resume by their own UUID.

### Initramfs: `systemd` needs no `resume` hook

How resume is wired up depends on which init your initramfs uses:

- **`systemd` hook** (this machine): systemd's built-in resume generator reads `resume=`/`resume_offset=` straight off the kernel cmdline and restores the image before mounting root. **No separate hook needed.** Confirm with:
  ```sh
  grep '^HOOKS' /etc/mkinitcpio.conf      # contains: systemd
  ```
- **busybox/`base`+`udev` hook:** you must add the **`resume`** hook to `HOOKS=(… udev resume …)` (after `udev`, before `filesystems`) and `sudo mkinitcpio -P`.

If your `HOOKS` has `systemd`, you're done here — do **not** also add `resume` (it's a no-op at best with systemd).

### 3. Reuse the suspend hooks — they fire on hibernate too

The Wi-Fi unbind/rebind and Touch Bar USB-reset `systemd-sleep` hooks from the [suspend fix](../sleep-resume/) run on **every** sleep transition, hibernate included (`$1 = pre|post`, `$2 = hibernate`). They were already installed here, which is why Wi-Fi and the Touch Bar came back on the first hibernate resume. **If you haven't done the suspend writeup yet, install those two hooks first** — hibernate resume will otherwise hit the same Wi-Fi/Touch-Bar-don't-survive-resume quirks. Do **not** install the AUR `brcmfmac-suspend` package on top of them; it duplicates the Wi-Fi hook.

## Testing — staged first, then real

Reboot so `resume=` actually loads, then **verify the kernel sees the target** before trusting anything:

```sh
grep -o 'resume=[^ ]*\|resume_offset=[^ ]*' /proc/cmdline   # both present
cat /sys/power/resume_offset                                # 28698624 (matches)
cat /sys/power/state                                        # includes 'disk'
swapon --show                                               # /swapfile active
```

### Staged dry-run (no data risk) — `pm_test=devices`

`pm_test` exercises the **full suspend/resume device cycle and auto-wakes in ~5 s without writing an image or powering off.** It's the cheap way to flush out a driver that won't come back — *before* you bet a real session on it:

```sh
echo devices | sudo tee /sys/power/pm_test
sudo systemctl hibernate            # cycles devices, auto-wakes in ~5s
sudo journalctl -k -b | grep -iE "fail|error|brcmf|nvme|timed out" | tail -40
echo none  | sudo tee /sys/power/pm_test   # MUST reset to [none] before the real test
cat /sys/power/pm_test                      # → [none]
```
On this machine the staged run was clean — no fail/error in the test window. **Leaving `pm_test` set to `devices` makes a "real" hibernate silently no-op back to your session**, so always reset to `[none]`.

### Real hibernate (writes the image, powers off)

**Save all work first** — if resume ever fails, the machine cold-boots and unsaved state is lost.

```sh
cat /sys/power/pm_test       # must be [none]
sudo systemctl hibernate     # writes image → powers off; press power to resume
```

## Validation

What a clean round-trip looks like in the journal — note the ~42 s gap that spans image-write, power-off, your power-button press, and image-read:

```
PM: hibernation: hibernation entry
PM: hibernation: Allocated 2783996 kbytes in 0.42 seconds (6628.56 MB/s)   ← snapshot
Restarting tasks: Done
PM: hibernation: hibernation exit                                          ← resumed, no errors
```

Checklist after power-on:
- [ ] Your **session is restored** (open windows/editor exactly as left), not a fresh login.
- [ ] `journalctl -k -b | grep -c 'hibernation exit'` ≥ 1, with **no** `nvme … timeout/reset/Identify` around it.
- [ ] **Touch Bar lit** and responsive (the one genuinely untested unknown going in — it survived).
- [ ] **Wi-Fi reconnected** on its own.
- [ ] Time-to-desktop ≈ under a minute.

### Benign noise you can ignore

On resume the journal shows a few re-enumeration complaints that are **cosmetic** — everything still works:

| Line | What it is |
|---|---|
| `usb 4-1: device not accepting address 2, error -62` | A USB device re-probe hiccup on the iBridge bus during resume; transient, self-recovers. |
| `hid-generic … driver_sysfs_add failed` | A HID re-probe warning as devices re-enumerate; harmless. |
| `brcmfmac … Direct firmware load for …Apple…bin failed -2` then a working `Firmware: BCM43602/2 … version 7.35.177.61` | The driver just can't find the *Apple-specific* firmware variant and falls back to the generic one, which loads fine. Same benign line you see at every boot. |

## If resume ever fails — recovery

A failed resume normally just **cold-boots** (the kernel rejects a stale/invalid image and boots fresh — you only lose the unsaved session). A true **hang** requiring a force-off is the worst realistic case.

1. **Force power off:** hold the power button ~10 s.
2. At the **rEFInd menu**, highlight **"Boot writers-deck"**, press **F2** (or **Insert**) to edit the boot options, append ` noresume`, and boot — this skips the stale image and gives you a clean session. (Fallback: boot an entry that has no `resume=`; it cold-boots, though that session won't restore the image.)
3. Discard the stale image so the next hibernate starts clean:
   ```sh
   sudo swapoff /swapfile && sudo mkswap /swapfile && sudo swapon /swapfile
   ```

**Full revert** (undo hibernate entirely): remove `resume=…/resume_offset=…` from the rEFInd line (or restore the `.bak-prehibernate-*` backup), `sudo swapoff /swapfile && sudo rm /swapfile`, drop the fstab line, reboot.

## New step-away workflow

Replace "close the lid and hope" with an explicit, reliable action:

```sh
sudo systemctl hibernate     # when stepping away for more than a few minutes
```
It keeps the whole session and resumes in well under a minute, instead of risking the long-`s2idle` wedge.

## `suspend-then-hibernate` — the best of both, and the real fix for "walked away and it wedged"

Plain hibernate is reliable but slow to resume (~1 min, and you have to press power). Plain `s2idle` resumes instantly but, on this machine, **can't be trusted for long idles** — the original sin this whole repo keeps circling. **`suspend-then-hibernate` (STH) resolves the tension:** it enters `s2idle` (instant resume for short breaks) and, after `HibernateDelaySec`, **automatically wakes and drops to S4** (zero-drain, wedge-proof for long absences). Because the `s2idle` phase is *capped* at the delay, the machine never sits in the fragile state long enough to wedge.

> **Status:** ✅ **STH cycle validated 2026-06-11.** A manual `systemctl suspend-then-hibernate` ran the complete two-phase handoff cleanly — `s2idle` → RTC-woke at the delay → hibernated → resumed with the session restored — and the [suspend/resume hooks](../sleep-resume/) fired correctly at **every** one of the four transitions. The lid-close / idle *triggers* below are the standard systemd wiring for it; verify the idle path on your own setup (see the note).

### Why STH sidesteps the wedge by design

The 2026-06-11 morning failure was lid-close → `s2idle` → *hours* → wedge. STH makes that sequence impossible: the longest the machine can stay in `s2idle` is `HibernateDelaySec`. Set that comfortably below the multi-hour danger zone (30 min here) and every "step away" lands in one of two safe states — instant-resume `s2idle` if you're back soon, full hibernate if you're not. Nothing ever idles toward the wedge.

### Config — three pieces

**1. The delay** (`/etc/systemd/sleep.conf.d/10-suspend-then-hibernate.conf`):
```ini
[Sleep]
# Sit in instant-resume s2idle this long, then auto-hibernate (S4).
# Kept well under the multi-hour s2idle wedge threshold.
HibernateDelaySec=30min
HibernateOnACPower=yes      # also hibernate when plugged in (systemd ≥256 default, set explicitly)
```

**2. The triggers — hand lid + idle to logind** (`/etc/systemd/logind.conf.d/10-suspend-then-hibernate.conf`):
```ini
[Login]
HandleLidSwitch=suspend-then-hibernate
HandleLidSwitchExternalPower=suspend-then-hibernate
HandleLidSwitchDocked=ignore
IdleAction=suspend-then-hibernate
IdleActionSec=15min
```
Apply with `sudo systemctl restart systemd-logind` (or reboot). On systemd 260 the restart preserves the running session.

**3. Stop GNOME from racing logind for the idle action.** GNOME's own idle-suspend (`gsd-power`) only offers plain `suspend`/`hibernate` — **not** STH — and it fires on its own timer, so leave it on and it'll plain-`s2idle` the machine before logind's STH ever runs. Disable just the suspend action (screen-blank/lock stay):
```sh
gsettings set org.gnome.settings-daemon.plugins.power sleep-inactive-ac-type 'nothing'
gsettings set org.gnome.settings-daemon.plugins.power sleep-inactive-battery-type 'nothing'
```

### The validated handoff (real journal, `HibernateDelaySec` was 2 min for this capture)

```
systemd-sleep: Performing sleep operation 'suspend'...
PM: suspend entry (s2idle)                                  ← Phase 1: instant-resume sleep
   …RTC alarm set for HibernateDelaySec…
PM: suspend exit                                            ← woke at the delay
systemd-sleep: Performing sleep operation 'hibernate'...
PM: hibernation: hibernation entry                          ← Phase 2: auto-drops to S4
   …image written, powered off, power button pressed…
PM: hibernation: hibernation exit                          ← resumed, session restored
systemd-logind: Operation 'suspend-then-hibernate' finished.
```
The Wi-Fi unbind/rebind and Touch Bar USB-reset hooks logged at all four boundaries — the same hooks the suspend fix installed, no extra work.

> **⚠️ Verify the idle trigger on your setup.** `HandleLidSwitch` is handled entirely by logind and is dependable. **`IdleAction`**, though, depends on the session reporting idle to logind — under GNOME/Wayland this generally works once GNOME's own idle-suspend is disabled (above), but confirm it on yours: drop `IdleActionSec` to `2min`, stop touching the machine, and check it sleeps (`journalctl -b | grep -i 'Performing sleep operation'`), then raise it back to `15min`. If logind idle won't fire on your config, fall back to GNOME idle → straight `hibernate` (`sleep-inactive-ac-type 'hibernate'`) — safe, just no instant-resume window on idle.

### Test it like any other sleep change

Same staged-then-real discipline: a `pm_test=devices` dry-run first if you've changed hooks, then a manual `sudo systemctl suspend-then-hibernate` with work saved, watching for the two-phase handoff in the journal. Only wire the lid/idle triggers once the manual cycle is clean.

## Lessons

- **"Resumes from `rtcwake`" ≠ "survives a real walk-away."** Short self-waking cycles validated the NVMe fix but didn't model hours of platform idle. The failure mode that matters is the one that happens when you're *not* watching.
- **Pick the power state that removes the fragile variable.** The NVMe bug is fundamentally "can't recover from a held deep-idle state." Hibernate doesn't hold anything — full power-off, cold init — so the bug has no surface to land on. Sometimes the fix is a different layer, not a better workaround in the same layer.
- **Dry-run before you bet a session.** `pm_test=devices` auto-wakes in 5 s and would have caught a non-resuming driver for free. Always reset it to `[none]` after.
- **Swapfiles resume by host-fs UUID + offset, not their own UUID.** The single most common hibernate-config mistake on a swapfile setup.

## Upstream / references

- Kernel docs: [`Documentation/power/swsusp.rst`](https://www.kernel.org/doc/html/latest/power/swsusp.html), `basic-pm-debugging.rst` (`pm_test`).
- Arch Wiki: *Power management/Suspend and hibernate* (swapfile `resume_offset`, systemd vs. `resume` hook).
- The NVMe root cause this setup sidesteps: [`../sleep-resume/`](../sleep-resume/).

---

*Written up by [Nori Nishigaya](https://github.com/xeeban) · Victoria, BC. More at [Emergent Insights](https://emergentinsights.substack.com/).*
