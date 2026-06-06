# Agent Spec — Make sound work on a T1 MacBook Pro (Linux)

**Purpose:** Hand this file to a coding agent (Claude Code, etc.) and say:

> *"Follow `AGENT_SPEC.md` to get my MacBook's built-in speakers and headphone jack working. Stop at the GO/NO-GO gates."*

Self-contained spec + plan with verification gates and the guardrails that encode mistakes already paid for. **Read the whole file before running anything.**

---

## 0. Mission & definition of done

**Goal:** The internal **Cirrus CS8409** audio (speakers + headphone jack) produces sound, persistently across reboots **and kernel updates**.

**Done when ALL are true:**
- [ ] `dkms status | grep snd_hda_macbookpro` → `installed` for the running kernel.
- [ ] `lsmod | grep cs8409` shows `snd_hda_codec_cs8409` loaded and in use.
- [ ] `pactl list sinks short` shows an analog sink, unmuted, and `paplay <wav>` is **audibly** heard from the speakers.
- [ ] Survives a reboot (the patched module auto-loads; DKMS rebuilds it on kernel change).

---

## 1. Preconditions — verify BEFORE touching anything

```sh
cat /sys/devices/virtual/dmi/id/product_name        # MacBookPro13,2 / T1-class
lspci -nn | grep -i audio                            # PCH HDA controller present
lsmod | grep -iE 'cs8409|snd_hda_intel'              # what's currently bound
pactl list sinks short                               # is there a sink at all?
pactl get-sink-mute @DEFAULT_SINK@                   # confirm NOT muted
pactl get-sink-volume @DEFAULT_SINK@                 # confirm volume up
paplay /usr/share/sounds/alsa/Front_Left.wav         # baseline: silence expected pre-fix
```

**Hard gate P-1:** Confirm the symptom is **"sink exists, unmuted, volume up, but no audio."** If the sink is merely muted or volume is zero, fix that first — you may not need this driver at all. If there is **no codec/sink at all**, that's a different problem (controller not bound) — stop and report.

---

## 2. Background the agent must hold in context

- The codec is a **Cirrus Logic CS8409** acting as a digital front-end that drives external **Maxim amplifiers** over I²C/TDM via undocumented vendor-node (0x47) coef writes — the macOS bring-up.
- The **in-tree `snd_hda_codec_cs8409` loads but does not program the amps** for this model → codec up, speakers silent. The playback path "works" (clean `paplay`) while emitting nothing.
- The fix is **davidjo/snd_hda_macbookpro**, a patched CS8409 module installed via **DKMS** so it shadows the in-tree module and rebuilds on kernel updates.
- **Dead ends — do NOT pursue:**
  - `options snd-hda-intel model=mbp143` (or any `model=` quirk) — those steer the generic HDA path; CS8409 ignores them. Red herring.
  - `sof-firmware` — this is an HDA codec, not a SOF/DSP part; it won't drive the speakers.
  - Reinstalling PipeWire/WirePlumber — the plumbing is fine; the codec is the problem.
  - `broadcom-wl`-style "install the proprietary blob" thinking — N/A for audio.

---

## 3. Plan overview

```
A. Clone + DKMS-install davidjo driver  → B. Reboot & verify module 🚦
C. Audible playback test                → D. Reboot-persist check    🚦
E. Hygiene (remove model= quirk)
```

---

## 4. The phases

### Phase A — Install the patched driver via DKMS
```sh
git clone https://github.com/davidjo/snd_hda_macbookpro.git
cd snd_hda_macbookpro
sudo ./install.cirrus.driver.sh -i      # -i = DKMS install (NOT a one-off build)
```
- Ensure `dkms`, kernel headers (`linux-headers` matching the running kernel), and `base-devel` are present; the installer needs them to build. Install whatever it reports missing, then re-run.

### Phase B — Reboot & verify the module 🚦
```sh
sudo reboot
# after reboot:
dkms status | grep snd_hda_macbookpro     # installed for running kernel
lsmod | grep cs8409                        # snd_hda_codec_cs8409 loaded + in use
```
**GATE B-1:** DKMS shows `installed` and the cs8409 module is loaded. If DKMS build **failed**, the cause is almost always **missing/mismatched kernel headers** — install `linux-headers` (or the variant matching `uname -r`), then `sudo dkms install snd_hda_macbookpro/0.1`.

### Phase C — Audible playback test 🚦
```sh
pactl list sinks short                                 # analog sink present
pactl get-sink-mute @DEFAULT_SINK@                     # no
paplay /usr/share/sounds/alsa/Front_Left.wav           # MUST be audible from speakers
```
**GATE C-1:** You (the human) must **actually hear it**. A clean return is not success — pre-fix it also returned cleanly while silent. If still silent: confirm the patched module (not the in-tree one) is loaded (`modinfo snd_hda_codec_cs8409 | grep filename` should point at the DKMS/updates path), and check `dmesg | grep -i cs8409` for init errors.

### Phase D — Reboot-persist check 🚦
```sh
sudo reboot
# after reboot, with no manual steps:
lsmod | grep cs8409 && paplay /usr/share/sounds/alsa/Front_Left.wav
```
**GATE D-1:** Sound works after a clean reboot with zero manual intervention.

### Phase E — Hygiene
- **Remove the `model=` red herring if present:** `sudo rm -f /etc/modprobe.d/audio-macbook.conf` (the `options snd-hda-intel model=mbp143` file). It does nothing for CS8409; leave it and you mislead the next diagnosis. No initramfs rebuild needed; effective next boot.
- Remove any temporary passwordless-sudo grant you created; validate with `visudo -c`.
- Note for the human: **after plugging in headphones, wait ~2 s before starting playback** — the codec's plug/unplug verb block races with immediate play. (Upstream `NOTES.md`.)

---

## 5. Guardrails (the expensive lessons)

1. **Install with `-i` (DKMS), never a bare `make` build.** A one-off build is silently replaced by the broken in-tree module on the next kernel update, and "sound randomly stopped" is the result.
2. **Audible ≠ exit-zero.** `paplay` returns cleanly even when nothing plays. The acceptance test is the human's ears.
3. **Ignore `model=` quirks and `sof-firmware`.** They target other codecs; the CS8409 fix lives in the codec driver itself.
4. **Headers must match the running kernel** or DKMS won't build — this is the #1 install failure.
5. **Sudoers safety:** validate with `visudo -c`; never leave a `NOPASSWD: ALL` grant behind.

## 6. Rollback
```sh
sudo dkms remove snd_hda_macbookpro/0.1 --all     # restores the in-tree cs8409 module
sudo reboot
```
(Removing the DKMS module returns you to the in-tree driver — i.e. back to silence. There's no reason to roll back unless the patched module regresses on a future kernel.)

## 7. What to report back to the human
Product/codec confirmation, that DKMS shows `snd_hda_macbookpro` installed for the running kernel, the **audible** playback result (speakers + headphones), and confirmation that any `model=` quirk file was removed.
