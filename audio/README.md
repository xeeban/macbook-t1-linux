# Fixing sound on a T1 MacBook Pro (Linux)

Why the built-in speakers (and headphone jack) stay completely silent on a fresh Arch install — and the out-of-tree driver that brings them to life.

> **TL;DR** — On a **MacBookPro13,2 (T1)** running Arch Linux (kernel `7.0.10`), the internal audio is a **Cirrus Logic CS8409** HDA codec wired to **Maxim amplifiers** over I²C. The mainline kernel's in-tree `snd_hda_codec_cs8409` loads, but it does **not** do the vendor-specific amplifier/TDM programming this model needs, so the speakers never get a signal — **dead silence**, even though a sink shows up in PipeWire and nothing is muted. The fix is **davidjo's [`snd_hda_macbookpro`](https://github.com/davidjo/snd_hda_macbookpro)** driver, which ships a patched CS8409 module that does the full Apple-style codec + amp init. Install it via its DKMS installer and reboot:
> ```sh
> git clone https://github.com/davidjo/snd_hda_macbookpro.git
> cd snd_hda_macbookpro
> sudo ./install.cirrus.driver.sh -i      # -i = DKMS install (auto-rebuilds on kernel updates)
> sudo reboot
> ```
> After the reboot a test sound plays out the speakers.

> **Status:** ✅ **Fixed 2026-06-05** — internal speakers and headphone jack work. DKMS keeps the patched module rebuilt across kernel updates.

> **Want your agent to do this for you?** Point it at [`AGENT_SPEC.md`](./AGENT_SPEC.md).

This is part of [the T1 MacBook Pro on Linux journey](../).

---

## The machine

- **MacBookPro13,2** — 2016 13" MacBook Pro (T1), Arch Linux, kernel `7.0.10-arch1-1`, PipeWire (`pipewire-pulse` + `wireplumber`).
- **Audio:** **Cirrus Logic CS8409** HDA codec on the PCH controller (`alsa_output.pci-0000_00_1f.3.analog-stereo`), driving **Maxim (MAX98706-class) amplifiers** over I²C. Output is fixed by the codec at **44.1 kHz / 24-bit / 4-channel**.

## The symptom

No sound from the speakers and no sound from the headphone jack — but nothing *looks* broken:

- PipeWire is running and a sink exists (`pactl list sinks short` shows the analog-stereo sink).
- The sink is **not muted** and volume is up (`pactl get-sink-mute @DEFAULT_SINK@` → `no`).
- `alsamixer` shows channels present and unmuted.
- `paplay /usr/share/sounds/alsa/Front_Left.wav` "succeeds" — returns cleanly, no error — and yet **silence**.

That last point is the tell: the playback path accepts audio, but nothing reaches the speakers because the **amplifiers were never programmed**.

## The trap: muting, PipeWire, firmware, and `model=` quirks

The usual suspects are all dead ends here:

| Theory | Reality |
|---|---|
| Muted / wrong default sink | The sink is present, default, and unmuted; volume is up. Not it. |
| PipeWire/WirePlumber misconfigured | Reinstalling `pipewire`/`pipewire-pulse`/`wireplumber` changes nothing — the plumbing is fine, the codec just isn't producing output. |
| Missing `sof-firmware` | Worth having, but this codec is **HDA**, not SOF/DSP — `sof-firmware` doesn't drive the CS8409 speaker path. Installing it doesn't fix the silence. |
| `snd-hda-intel` model quirk — `options snd-hda-intel model=mbp143` | A genuine red herring. `model=` quirks steer the **generic Realtek-style** `snd-hda-intel` path; the CS8409 has its own codec driver and ignores them. We tried this; it did nothing. Remove it afterward. |
| Just use the in-tree `cs8409` driver | The in-tree module **loads** but doesn't perform the vendor amp/TDM init for this MacBook — codec up, speakers silent. |

## Root cause: the in-tree CS8409 driver doesn't program the amps

The CS8409 on this machine is, per davidjo's reverse-engineering, acting as a **digital front-end**: it converts the incoming stream to a **TDM** bit-stream and ships it to external **Maxim amplifiers**, which do the actual D/A conversion. All of the magic — the I²C programming of the amps, the TDM setup, the output-pin routing — happens through **undocumented CS8409 vendor-node (0x47) coef writes**, exactly the way macOS does it. The mainline `snd_hda_codec_cs8409` doesn't carry that Apple-specific init for the T1, so the codec powers up but the amps are never told to play. Result: a working-looking pipeline that emits nothing.

## The fix — davidjo's patched CS8409 driver via DKMS

[`davidjo/snd_hda_macbookpro`](https://github.com/davidjo/snd_hda_macbookpro) ships a **patched `snd_hda_codec_cs8409`** that performs the full Apple-style codec + amplifier bring-up. Install it through its own DKMS installer so it survives kernel updates:

```sh
git clone https://github.com/davidjo/snd_hda_macbookpro.git
cd snd_hda_macbookpro
sudo ./install.cirrus.driver.sh -i      # -i installs via DKMS
sudo reboot
```

The `-i` flag is the important part: it registers the patched module with **DKMS**, so it **auto-rebuilds against each new kernel** instead of silently reverting to the in-tree module on the next `pacman -Syu`.

### Verify it took

```sh
# DKMS is tracking the patched driver (shadows the in-tree module):
dkms status | grep snd_hda_macbookpro     # snd_hda_macbookpro/0.1, <kernel>: installed (Original modules exist)

# the patched CS8409 codec is the loaded, active codec:
lsmod | grep cs8409                        # snd_hda_codec_cs8409 ... in use

# a sink is live, unmuted, and you actually hear this:
pactl list sinks short
paplay /usr/share/sounds/alsa/Front_Left.wav
```

## Gotcha: the headphone-jack plug/unplug race

From the driver's own `NOTES.md`: headphone plug/unplug events use **unsolicited responses** that, under Linux, run concurrently with other codec commands. The driver serializes these verb blocks, but they take **multiple seconds**. Practical consequence:

- After **plugging in headphones, wait a couple of seconds before starting playback** — kick off audio too fast and the play-setup verbs collide with the still-running plug-in block.
- Plug-while-playing then unplug-while-playing is known to work; it's the *plug → immediate play* sequence that races.

Also per upstream: **SPDIF isn't implemented**, inputs (internal mic / line-in) are set up but not wired to capture streams, and **sleep/power-down behavior for the codec is untested** by the author — worth knowing if you chase a resume-audio issue later.

## Cleanup — drop the `model=` red herring

If you tried the `snd-hda-intel` model quirk while debugging, remove it; it does nothing for the CS8409 and only muddies the next person's diagnosis:

```sh
sudo rm /etc/modprobe.d/audio-macbook.conf      # the 'options snd-hda-intel model=mbp143' file
```

(The whole quirk lived in that one file — nothing else to undo, no initramfs rebuild needed. Takes effect next reboot.)

## Lessons

- **A clean `paplay` with no sound means the path works but the output stage is dead** — look downstream (amps/codec init), not at mute/volume/PipeWire.
- **CS8409 ≠ generic HDA.** `snd-hda-intel model=` quirks and `sof-firmware` are for other codecs; they don't touch the CS8409 speaker path. The model-specific work lives in the codec driver itself.
- **Install out-of-tree drivers with `-i` (DKMS), not a one-off build** — otherwise the next kernel update silently restores the broken in-tree module and your sound "randomly" dies.
- **`T1 ≠ T2` here too** — this is the CS8409 path; T2 Macs use a different audio bring-up. Don't follow T2 audio guides on this hardware.

---

*Written up by [Nori Nishigaya](https://github.com/xeeban) · Victoria, BC. More at [Emergent Insights](https://emergentinsights.substack.com/).*
