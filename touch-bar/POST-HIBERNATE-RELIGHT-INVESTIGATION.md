# Post-Hibernate Touch Bar Relight — Final-Mile Investigation

**Date:** 2026-06-12 (read-only research run; user actively testing on the machine).
**Machine:** MacBookPro13,2 (T1), Arch `7.0.10-arch1-1`, apple-ib-drv r307 + local patches
(disp raw-request fix, ibridge teardown OOB fix — both deployed and verified working in this
morning's journal: live power-cycle at 07:04 and post-hibernate power-cycle at 07:09 both
completed cleanly, no GPF/D-state).

> Sibling docs: [`../sleep-resume/TOUCHBAR-RELIGHT-ANALYSIS.md`](../sleep-resume/TOUCHBAR-RELIGHT-ANALYSIS.md),
> [`IBRIDGE-TEARDOWN-UAF-ANALYSIS.md`](IBRIDGE-TEARDOWN-UAF-ANALYSIS.md).
> New artifacts from this run: [`touchbar-suspend-trace-2017/`](touchbar-suspend-trace-2017/)
> (Dunedan's 2017 macOS USB traces, downloaded and decoded — see §3).

---

## (a) VERDICT

**Mostly a firmware limitation — but not fully exhausted.** Two genuinely untried host-side
avenues remain, both grounded in new evidence found in this run:

1. **The driver puts the T1 to sleep in the wrong state on the hibernate path** (display ON),
   which the driver author's own comment says yields a "only partially responsive" device on
   resume. A pre-hibernate state-hygiene fix (no teardown, zero deadlock risk) has never been
   tried. **(§5.1 — best untried idea.)**
2. **macOS relights the bar with a `DRLC` wake command + framebuffer push over the
   config-2 bulk protocol — never via the config-1 HID path Linux uses.** Dunedan's 2017
   traces (decoded locally, §3) give the exact wire bytes; imbushuo's Windows driver gives the
   protocol grammar. Replaying this from Linux userspace is concrete and untried. **(§5.2.)**

If both fail, the conclusion is firm: the T1 firmware latches its display pipeline across S4 in a
way the config-1 ("Default iBridge Interfaces") HID surface cannot undo, and no one in 9 years of
public record (2017→2026) has relit it without a reboot. The community-documented answer for both
T1 and T2 is literally "disable hibernation" ([t2linux wiki](https://wiki.t2linux.org/state/)).

Confidence in the overall verdict: **high** (the negative space is extremely well mapped — see §2).
Confidence that §5.1 or §5.2 works: **modest** (~25–35% and ~20–30% respectively, honest guesses).

---

## (b) Where the world stands (web research, 2026-06)

The problem is *known, characterized since July 2017, and unsolved everywhere*:

- **Original characterization** — roadrunner2 (driver author),
  [macbook12-spi-driver issue #3](https://github.com/roadrunner2/macbook12-spi-driver/issues/3):
  *"After suspending and resuming, the touchbar stays off and inactive; however, at the USB level
  everything looks fine (the responses to the various commands all indicate success)."* Still open,
  "help wanted". This is exactly our symptom, 9 years ago.
- **DRLC discovery** — Dunedan,
  [cb22/macbook12-spi-driver PR #30](https://github.com/cb22/macbook12-spi-driver/pull/30) (2017):
  *"macOS sends a packet, which contains 'DRLC' two times before each suspend and once or twice
  whenever it wakes up again,"* with USB traces attached (now decoded — §3).
- **T2 mirror of the problem** — [t2linux wiki State page](https://wiki.t2linux.org/state/) and
  [basecamp/omarchy discussion #5862](https://github.com/basecamp/omarchy/discussions/5862):
  suspend-then-hibernate causes the bridge to *"drop power to the TouchBar USB device in a way it
  can't recover from"* — **"can't be recovered without a reboot"**. Their fix: `AllowHibernation=no`.
  (T2's short-S3 recovery trick — module reload + `bConfigurationValue` 0→2 cycle — addresses a
  *driver* problem, not our firmware latch; and our equivalents have all been tried.)
- **No T1 hibernate relight exists in any guide** — checked roadrunner2's gist, Dunedan/mbp-2016-linux,
  xtocdra/macbookpro13-2, almas/rob-hills T1 gists, marcosfad/mbp-ubuntu#9, mikeeq/mbp-fedora#14,
  Gentoo wiki, t1linux. Several explicitly state no post-suspend recovery script exists for T1.
- **Upstream (Linux 6.17) Touch Bar work** (AdityaGarg8,
  [HID multitouch patches](https://patchwork.kernel.org/project/linux-input/cover/20250325180138.15113-1-gargaditya08@live.com/))
  adds input support; it does **not** address resume/display recovery.

Full source list at the bottom.

## What this machine has already proven (recap + this morning's journal)

| Tried | Result |
|---|---|
| `set_tb_disp(ON)` post-S4 via synchronous EP0 SET_REPORT (patched) | **succeeds (rc=0), bar stays dark** |
| `set_tb_mode` (direct vendor `usb_control_msg`) post-S4 | succeeds, dark |
| `idle_timeout` −2/−1 re-drive post-S4 | dark |
| Logical re-enumeration `authorized` 0→1 + fresh `appletb_probe` (07:09:15 today, clean) | dark |
| Real bus reset `USBDEVFS_RESET` (07:14:36 today) | dark |
| Module reload `try-ibridge-reload.sh` (07:35 today) | (in test — expected dark) |
| Warm reboot / cold boot | **lights** |

So: the firmware re-enumerates USB perfectly post-S4 (fresh descriptors, working HID, ACKed
display commands) yet keeps the panel dark. The latch is **independent of the T1's USB stack** —
it lives in the T1's display pipeline (bridgeOS side), and only a host platform reboot clears it.
This kills the whole "deeper USB reset" family (§5.4).

---

## (c) §3. What macOS actually does — the traces, decoded

Downloaded Dunedan's `touchbar-suspend.zip`
([attachment on PR #30](https://github.com/cb22/macbook12-spi-driver/files/1165901/touchbar-suspend.zip))
and parsed the pcapng files locally (linktype 220 / usbmon-mmapped; decoder inline below the table).
Both traces show one suspend→wake (S3 sleep) cycle of the iBridge (dev 2) **in the OS X
configuration (config 2)**, which exposes bulk pipes Linux's config 1 does not have:

**Suspend (t=0):**
```
bulk OUT ep2:  02 00 12 01 | 00*8 | 10 00 00 00 | 44 52 4C 43 ("DRLC") seq 00 00 00 | 00*8 | 10 00 00 00   (x2, seq n, n+1)
ctrl  ep0:     SET_REPORT bmReqType=0x21 wValue=0x0303 (Feature, id 3) wIndex=6 wLen=15, data 03 01 F4 01 00...
bulk OUT ep2:  DRLC seq n+2
bulk IN  ep5:  16-byte ack frames (02 00 12 01 ... 10 00 00 00)
intr IN  ep7:  small status frames ~0.5 s later
```
**Wake (t=+15.7 s):**
```
bulk OUT ep2:  DRLC seq n+3, DRLC seq n+4
bulk OUT ep2:  framebuffer push: header 02 00 12 00 | 09 00 00 00 | 00*4 | 50 F6 05 00 (len 390736
               ≈ 2170x60x3 BGR) | frame-update content (FrameId, x/y/w/h, BufferSize) | pixel data,
               in 20480-byte URBs until complete
```

Cross-referencing [imbushuo's Windows T1 driver](https://github.com/imbushuo/DFRDisplayKm)
(`include/Dfr.h`): this is his "generic request" grammar exactly —
`{u32 RequestHeader; u32 rsvd; u32 rsvd; u32 RequestLength}` + `{u32 RequestKey; u8 rsvd[8];
u32 End=0x10}` — with known command keys `GINF` (get info), `REDY` (host ready), `CLDR` (clear
screen), `UDCL` (fb updated), framebuffer header `0x00120002`. **`DRLC` is simply another command
key in that same protocol — the sleep/wake display command** (sent 2x+1 at sleep, 2x at wake,
with a monotonically increasing cookie in the reserved bytes). The wake = `DRLC, DRLC, full
framebuffer redraw`.

**The decisive architectural fact:** macOS *never* relights the display through the config-1 HID
interface at all. The relight surface lives **only in the config-2 bulk protocol**. Linux's
apple-ib-tb works at cold boot because the T1, freshly booted, auto-lights its built-in
"simple-mode" rendering when it receives the HID mode/disp reports. After the host sleeps, the
firmware evidently parks the display pipeline and expects the DRLC wake — a command the config-1
surface cannot express. That is the most economical explanation of every data point we have.

(What macOS does across true S4: macOS hibernate resumes through full EFI boot, during which
Apple firmware re-initializes the T1 session — so macOS likely never faces our exact situation.
No public source documents a macOS S4 DFR wake distinct from the S3 one.)

Decoder used (for reproducibility):
```python
# parse_pcapng_usbmon.py — minimal pcapng EPB walker, linktype 220
import struct
def parse(fn):
    data, off, pkts = open(fn,'rb').read(), 0, []
    while off < len(data)-8:
        btype, blen = struct.unpack_from('<II', data, off)
        if blen < 12 or off+blen > len(data): break
        body = data[off+8:off+blen-4]
        if btype == 6:
            _, hi, lo, cap, _ = struct.unpack_from('<IIIII', body, 0)
            pkts.append(((hi<<32|lo)/1e6, body[20:20+cap]))   # 64B usbmon hdr + data
        off += blen
    return pkts
```

---

## §4. Driver-source findings (the suspend/resume path, post-patch source)

`/usr/src/apple-ib-drv-r307.4afd309/apple-touchbar.c`:

1. **`appletb_suspend()` (line ~1377) sends `MODE_OFF` + `DISP_OFF` only for
   `PM_EVENT_SUSPEND` — not for `PM_EVENT_FREEZE`, and it returns immediately for
   `PM_EVENT_HIBERNATE`** (the poweroff leg isn't even in the accepted-events list).
   The author's comment right there (lines ~1417-1425) is the smoking gun:

   > *"The touch bar device itself remembers the last state when suspended in some cases, but in
   > others (e.g. when mode != off and disp == off) it resumes with a different state; furthermore
   > **it may be only partially responsive in that state**. By turning both mode and disp off we
   > ensure it is in a good state when resuming…"*

2. **Worse: the hibernate sequence actively re-lights the bar right before power-off.** The S4
   legs are freeze → (snapshot) → **thaw** → write image → poweroff. On *thaw*,
   `appletb_reset_resume()` runs, re-activates, and force-drives mode/disp **ON** (this morning's
   journal shows `tb: Touchbar suspended.` then `usb 1-3: reset` then `tb: Touchbar resumed.`
   *inside* the hibernate entry at 07:07:12 — that's the freeze/thaw pair). The poweroff leg then
   does nothing. **Net: the T1 enters S4 with mode=ON, disp=ON — precisely the state the author
   says comes back partially responsive.** Every hibernate we have ever tested entered S4 this way.

3. **`appleib_suspend/resume` (apple-ibridge.c ~670-698) toggle ACPI `SOCW`**
   (`\_SB.PCI0.XHC1.RHUB.ASOC.SOCW`, the "Apple SOC" companion device of the iBridge under the
   xHCI root hub; verified bound on this machine: `/sys/bus/platform/drivers/apple-ibridge/APP7777:00`).
   These are *legacy* platform callbacks, which the platform core maps onto the hibernate legs too
   (freeze→`SOCW(0)`, thaw→`SOCW(1)`, poweroff→`SOCW(0)`, restore→`SOCW(1)`). No SOCW failure
   warnings in any journal ⇒ **SOCW(1) already runs, successfully, on every S4 resume — and does
   not relight.** A manual SOCW cycle can therefore only add value via *dwell time* (§5.3).
   What SOCW actually pokes (GPIO? SMC?) is not publicly documented; the DSDT can't be read
   without root from this session — worth a one-off `sudo cat /sys/firmware/acpi/tables/DSDT`
   + `iasl -d` when convenient, to see if ASOC has any *other* methods (reset, power resource)
   beyond SOCW.

4. Cold-boot probe vs. resume path: with the singleton `active`-gate behavior and the now-fixed
   transport, the *command content* of a fresh probe and a forced resume update are equivalent
   (mode write → 25 ms → disp write; same reports, same endpoints). There is **no hidden
   cold-boot-only handshake inside the driver** — no extra feature report, no
   `usb_driver_set_configuration` dance (config is already 1), no `hid_hw_power` difference that
   survives scrutiny: autopm (`PM_HINT_FULLON`) is taken around both paths. The "init magic" the
   firmware responds to at cold boot is therefore *its own boot*, not anything the driver sends.

---

## §5. Untried host-side methods, ranked

### 5.1 — BEST: pre-hibernate state hygiene (`idle_timeout=-2` before S4) — *no teardown, no risk*

**Why it might work:** §4.1/§4.2 — we have *never once* let the T1 enter S4 in the author-blessed
"good state" (mode OFF, disp OFF). The author explicitly documents partial responsiveness when
sleeping in the wrong state, and on S3 (where this driver historically worked for people) the OFF
commands *are* sent. This is the only state variable on the firmware side that we control and have
never controlled.

**Exact test (sleep hook, no driver change needed):**
```bash
# /usr/lib/systemd/system-sleep/49-touchbar-presleep.sh
#!/bin/sh
# Put the T1 touch bar into the documented-good sleep state before S4,
# restore after resume. No teardown, no unbind — only HID commands on a
# live endpoint (the proven-safe class of operation).
TB_SYSFS=$(ls -d /sys/bus/hid/drivers/apple-touchbar/*/idle_timeout 2>/dev/null | head -1)
[ -n "$TB_SYSFS" ] || exit 0
case "$1" in
  pre)
    [ "$2" = "hibernate" ] || exit 0
    echo -2 > "$TB_SYSFS"      # worker drains: sends MODE_OFF then DISP_OFF (live endpoint, fast)
    sleep 2                     # let tb_work complete both reports + 25ms gap
    ;;
  post)
    [ "$2" = "hibernate" ] || exit 0
    sleep 3                     # let reset_resume / re-enumeration settle first
    echo 300 > "$TB_SYSFS"      # back to default → worker drives MODE_SPCL + DISP_ON
    ;;
esac
exit 0
```
Caveat handled by design: with `idle_timeout=-2` latched in the singleton *before* freeze, the
**thaw-leg `reset_resume` now wants OFF too** (`appletb_update_touchbar_no_lock` computes
`want=OFF` from `idle_timeout==-2`), so the bar genuinely stays OFF through freeze→thaw→poweroff —
fixing §4.2 without touching the driver. The `post` write restores it.

**Driver-patch variant** (if the hook works, fold it in): in `appletb_suspend()` extend
`message.event == PM_EVENT_SUSPEND` to `|| message.event == PM_EVENT_FREEZE`, and add
`PM_EVENT_HIBERNATE` to the accepted-events guard so the poweroff leg also quiesces; plus skip the
forced ON in `appletb_reset_resume()` when the resume is the *thaw* of a hibernation
(`pm_transition`-style check, or simply leave the hook in place forever — it is robust and visible).

**Confidence: ~25–35%.** It directly targets a documented firmware misbehavior we provably
trigger on every hibernate, costs nothing, and is risk-free. It is *not* higher because the T2's
identical "dark until reboot after hibernate" suggests a deeper bridge-side latch that state
hygiene may not clear.

### 5.2 — The macOS-fidelity shot: replay the config-2 `DRLC` wake from userspace

**Why it might work:** it is byte-for-byte what macOS does to relight the panel (§3), and the
panel hardware demonstrably can come back from display-sleep without a T1 reboot (macOS does it
every S3 wake). Unknown: whether the firmware honors it after *S4*, and whether it demands a fuller
session handshake (`GINF`/`REDY`) first — both cheap to test since imbushuo's grammar covers them.

**Exact procedure (post-hibernate, after the bar is confirmed dark):**
```bash
# 1. release the iBridge HID interfaces (live endpoint — proven safe since the teardown fix)
echo -n '1-3:1.2' > /sys/bus/usb/drivers/usbhid/unbind   2>/dev/null
echo -n '1-3:1.3' > /sys/bus/usb/drivers/usbhid/unbind   2>/dev/null
# 2. switch the iBridge to the OS X configuration (bulk DFR pipes appear)
echo 2 > /sys/bus/usb/devices/1-3/bConfigurationValue
# 3. run the replay (below)
# 4. switch back and let udev re-bind the normal stack
echo 1 > /sys/bus/usb/devices/1-3/bConfigurationValue
```
```python
#!/usr/bin/env python3
# drlc-wake.py — replay macOS DFR wake on the T1 (config 2). pyusb required.
import struct, usb.core, usb.util
dev = usb.core.find(idVendor=0x05ac, idProduct=0x8600)
assert dev.get_active_configuration().bConfigurationValue == 2
# find the interface owning bulk-OUT 0x02 / bulk-IN 0x85 (trace: DFR pipe pair)
for intf in dev.get_active_configuration():
    eps = [ep.bEndpointAddress for ep in intf]
    if 0x02 in eps:
        if dev.is_kernel_driver_active(intf.bInterfaceNumber):
            dev.detach_kernel_driver(intf.bInterfaceNumber)
        usb.util.claim_interface(dev, intf.bInterfaceNumber)
        break
def req(key: bytes, seq: int) -> bytes:
    hdr  = struct.pack('<IIII', 0x01120002, 0, 0, 16)
    body = key + struct.pack('<I', seq) + b'\0'*4 + struct.pack('<I', 0x10)
    return hdr + body
for seq in (2, 3):                      # cookie appears to be a free-running counter; start low
    dev.write(0x02, req(b'DRLC', seq), timeout=1000)
    try: print('resp:', bytes(dev.read(0x85, 16, timeout=500)).hex())
    except usb.core.USBTimeoutError: print('no resp (may be fine)')
# escalation ladder if still dark: GINF (read info resp), REDY, CLDR, then a full
# all-0x36 framebuffer push with header 0x00120002 per imbushuo's DFR_FB structures.
```
Watch the panel between steps. If `DRLC` alone does nothing, walk the ladder
(`GINF` → `REDY` → `DRLC` → `CLDR` → framebuffer push); if *any* step lights pixels, the latch is
host-clearable and the rest is engineering. Afterwards config back to 1 and re-drive
`idle_timeout` so apple-ib-tb repaints its simple mode. (The config-1↔2 switch tears down and
re-probes all interfaces — the exact operation proven safe this morning at 07:04/07:09.)

**Confidence: ~20–30%** that some rung of the ladder lights the panel post-S4; **high** confidence
this is the *correct* protocol surface (it is the only one macOS uses). Effort: an hour of pyusb
fiddling. Worst case it teaches us the definitive answer; best case it's the basis of a proper
relight tool (or even a future T1 `tiny-dfr`-style display driver).

### 5.3 — ACPI `SOCW` manual cycle with dwell (low)

PM already does SOCW(0)→SOCW(1) around every S4 (§4.3) with no relight, so only a *long dwell*
power-down could differ (if SOCW gates a real power rail with capacitance/timing semantics):
```bash
modprobe acpi_call    # AUR
echo '\_SB.PCI0.XHC1.RHUB.ASOC.SOCW 0' > /proc/acpi/call; sleep 10
echo '\_SB.PCI0.XHC1.RHUB.ASOC.SOCW 1' > /proc/acpi/call
# then authorized 0->1 re-enumeration (proven safe) and idle_timeout re-drive
```
Do this only *after* dumping/decompiling the DSDT to see what SOCW writes — if it's an SMC/GPIO
power gate, a 10 s dwell genuinely power-cycles the T1 (= reboot-equivalent relight); if it's a
mailbox notification, it's a no-op we already do. **Confidence: ~10%** blind; revisit after DSDT
inspection. Risk: low-moderate (worst case the iBridge drops off USB until reboot — a state we
already survive routinely).

### 5.4 — Deeper USB/PCI resets (xHCI unbind/rebind, PCI FLR, port power) — effectively ruled out

The T1 is an internal always-on coprocessor: its power is **not** VBUS-derived, and we have
empirical proof the dark-latch is independent of the T1's USB stack (full `USBDEVFS_RESET` and
re-enumeration produce a perfectly fresh USB session, still dark — §2). xHCI driver unbind, PCIe
FLR of `0000:00:14.0`, or hub `PORT_POWER` clears reset the *host* side of the same bus and cannot
cut T1 power. **Confidence: ~5%; not recommended** (collateral: webcam/BT/usbmuxd churn, and the
xHCI rebind path on this platform is untested with the ASOC companion device).

### 5.5 — Not viable / checked and dismissed

- **SMC keys via `applesmc`**: no publicly documented T1/DFR power key; Apple's own "Touch Bar
  stuck" remedy is an SMC *reset* (boot-time chord, host reset). Enumerating this machine's SMC
  key list (read-only) for `DFR`/`SOC`-ish keys is a cheap curiosity, nothing more.
- **NVRAM/efivars**: no known variable gating DFR display state; nothing in any source.
- **tiny-dfr / appletbdrm**: T2/Apple-Silicon only (touch bar as USB device `05ac:8302` with its
  own display class); the T1 exposes no such device in config 1. (A config-2 DFR display driver
  for T1 is exactly what §5.2 would grow into.)
- **`fnmode`/`dim_timeout`/autopm sequencing tricks**: all funnel into the same two HID reports
  the firmware is provably ACKing-and-ignoring.

---

## (d) Honest confidence summary

| Claim | Confidence |
|---|---|
| The dark bar is a T1-firmware display-pipeline latch, not a Linux transport bug | **Very high** (transport proven good: ACKed commands, fresh enumerations, fixed teardown) |
| No published solution exists anywhere (T1, any distro, 2017–2026) | **High** (extensive multi-engine search; the unanimous community answer is "disable hibernate") |
| macOS's relight = config-2 `DRLC` + framebuffer push, unavailable in config 1 | **High** (decoded traces + imbushuo protocol corroboration) — *for S3*; macOS-S4 behavior is inferred (EFI reboot path), **medium** |
| §5.1 (pre-hibernate OFF state) relights post-S4 | **~25–35%** — strongest cost/benefit; we provably enter S4 in the author-flagged bad state today |
| §5.2 (DRLC replay) relights post-S4 | **~20–30%** — right protocol surface, unknown S4 semantics |
| §5.3 (SOCW dwell cycle) | **~10%** pending DSDT inspection |
| §5.4 (deeper bus resets) | **~5%** — architecture + empirical record argue no |

**Recommended order:** 5.1 (tonight — it's two `echo`s in a sleep hook) → 5.2 (an afternoon of
pyusb) → DSDT dump + 5.3 → if all dark: declare firmware limitation, keep the dark-bar-after-
hibernate trade-off, and treat a T1 config-2 display driver (§5.2 grown up) as the only long-term
escape hatch.

---

## Sources

- [roadrunner2/macbook12-spi-driver issue #3 — Touch Bar dead after resume (2017, open)](https://github.com/roadrunner2/macbook12-spi-driver/issues/3)
- [cb22/macbook12-spi-driver PR #30 — DRLC observation + traces (Dunedan)](https://github.com/cb22/macbook12-spi-driver/pull/30)
  — trace zip: [touchbar-suspend.zip](https://github.com/cb22/macbook12-spi-driver/files/1165901/touchbar-suspend.zip) (mirrored in `touchbar-suspend-trace-2017/`)
- [imbushuo/DFRDisplayKm — Windows T1 DFR display driver (config-2 protocol: GINF/REDY/CLDR/UDCL, FB header 0x00120002)](https://github.com/imbushuo/DFRDisplayKm)
- [imbushuo — "A deep dive into Apple Touch Bar" (BGR24 USB framebuffer protocol)](https://www.imbushuo.net/blog/archives/684/)
- [t2linux wiki — State (hibernate kills TouchBar "in a way it can't recover from")](https://wiki.t2linux.org/state/)
- [basecamp/omarchy discussion #5862 — T2 TouchBar suspend fix; long-sleep loss "can't be recovered without a reboot"](https://github.com/basecamp/omarchy/discussions/5862)
- [roadrunner2 gist — Linux on MBP Late 2016/2017](https://gist.github.com/roadrunner2/1289542a748d9a104e7baec6a92f9cd7) ·
  [Dunedan/mbp-2016-linux](https://github.com/Dunedan/mbp-2016-linux) ·
  [xtocdra/macbookpro13-2](https://github.com/xtocdra/macbookpro13-2) ·
  [marcosfad/mbp-ubuntu #9](https://github.com/marcosfad/mbp-ubuntu/issues/9)
- [LKML/Spinics — apple-ibridge patch (SOCW ACPI PM)](https://www.spinics.net/lists/linux-input/msg61889.html)
- [AdityaGarg8 — HID: multitouch Touch Bar patchset (Linux 6.17)](https://patchwork.kernel.org/project/linux-input/cover/20250325180138.15113-1-gargaditya08@live.com/)
- [AppleInsider — T1 architecture (Touch Bar runs off T1, watchOS-derived)](https://appleinsider.com/articles/16/10/28/examined-the-new-macbook-pro-touch-bar-and-apples-t1-authentication-chip) ·
  [MacRumors — T1 runs watchOS variant](https://www.macrumors.com/2016/10/28/touch-bars-t1-chip-variant-watchos/)
- [Hackaday — Touch Bar OLED panel RE (MIPI DSI command mode)](https://hackaday.com/2024/01/23/reverse-engineering-the-apple-touch-bar-screen/)
- Local evidence: `journalctl -b 0` 2026-06-12 07:04–07:35 (clean teardowns, dark-after-S4 with
  working transport); driver source `/usr/src/apple-ib-drv-r307.4afd309/` (suspend-path asymmetry §4).
