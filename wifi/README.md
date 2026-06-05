# Fixing Wi-Fi on a T1 MacBook Pro (Linux)

Why the built-in Broadcom card associated instantly with one router and silently refused another — and the one-line module option that fixes the common case.

> **TL;DR** — On a **MacBookPro13,2 (T1)** running Arch Linux (kernel `7.0.10`), the internal **Broadcom BCM43602** (`brcmfmac`) would scan and see networks but **fail the WPA2 4-way handshake** and never associate. The cause is a known interaction between **`wpa_supplicant` 2.11** and `brcmfmac`'s **firmware-offloaded handshake** (the `FWSUP`/`SAE` path): the firmware bungles the offloaded handshake, and the supplicant times out. The fix is one modprobe option — **`options brcmfmac feature_disable=0x82000`** — which disables the firmware offload and lets `wpa_supplicant` run the handshake in software. After that the card connects normally (DHCP, internet). **Caveat:** this fixes association with a standard AP (tested against a TELUS/Sagemcom gateway). A **mesh** router in this household (the "TMD" Sagemcom mesh) still rejects the association with `status 16` — an AP-side interop issue, not a card problem (see [Known limitation](#known-limitation)).

> **Status:** ✅ **Fixed for standard APs 2026-06-04** — native BCM43602 connects with DHCP and internet. ⚠️ Mesh-AP association still fails (router-side fix needed).

> **Want your agent to do this for you?** Point it at [`AGENT_SPEC.md`](./AGENT_SPEC.md).

This is part of [the T1 MacBook Pro on Linux journey](../).

---

## The machine

- **MacBookPro13,2** — 2016 13" MacBook Pro (T1), Arch Linux, kernel `7.0.10-arch1-1`.
- **Wi-Fi:** Broadcom **BCM43602** at PCI `0000:02:00.0`, driven by **`brcmfmac`** (the upstream Broadcom FullMAC driver). No `broadcom-wl`/`b43` needed — `brcmfmac` is correct for this chip.

## The symptom

The card was detected, the driver loaded, scans returned the SSID — but every connection attempt stalled at authentication and eventually failed. `journalctl -u wpa_supplicant` (or NetworkManager) showed the association starting and then timing out on the 4-way handshake, with no useful AP-side reason. It looked like a wrong password even when the password was right.

## The trap: blaming the driver, the firmware blob, or the password

The usual suspects are all dead ends here:

| Theory | Reality |
|---|---|
| Wrong/old firmware blob | `brcmfmac` loads the correct `brcmfmac43602-pcie.bin`; the card scans fine — firmware is present and working. |
| Need `broadcom-wl` (the proprietary `wl`) | No — `wl` is for older `b43`-era chips. BCM43602 is a FullMAC `brcmfmac` part. Installing `wl` just creates a conflict. |
| Missing regulatory domain | A real (separate) gap — install `wireless-regdb` and set your country — but it isn't what blocks association. |
| Wrong password | The handshake fails even with the correct PSK. The problem is *where* the handshake runs, not the key. |

## Root cause: a firmware-offloaded handshake that `wpa_supplicant` 2.11 can't drive

Modern `brcmfmac` firmware can perform the WPA2/WPA3 **4-way handshake in firmware** ("firmware supplicant" / `FWSUP`) and offloaded **SAE** for WPA3. `wpa_supplicant` is supposed to hand the handshake to the firmware and stay out of the way. With **`wpa_supplicant` 2.11** and this card's firmware, that offloaded path is broken — the handshake never completes and the association times out. This bites a lot of `brcmfmac` users on current distros; the reliable workaround is to **turn the offload off** so the supplicant does the handshake itself (the path it's always done well).

## The fix — one module option

`brcmfmac`'s `feature_disable` is a bitmask over its internal feature flags. The value **`0x82000`** clears exactly the two offload features behind this bug:

| Bit | Value | Feature | Effect when disabled |
|---|---|---|---|
| 13 | `0x02000` | `FWSUP` — firmware-offloaded 4-way handshake / supplicant | `wpa_supplicant` runs the WPA2 handshake in software |
| 19 | `0x80000` | `FWAUTH` — firmware-offloaded authentication | auth handled host-side |
| | **`0x82000`** | **both** | full host-side handshake — the reliable path |

Install it persistently:

```sh
echo 'options brcmfmac feature_disable=0x82000' | sudo tee /etc/modprobe.d/brcmfmac.conf

# also make sure your regulatory domain is set (separate, but worth doing):
sudo pacman -S --needed wireless-regdb
sudo sed -i 's/^#WIRELESS_REGDOM="CA"/WIRELESS_REGDOM="CA"/' /etc/conf.d/wireless-regdom 2>/dev/null || true
# (substitute your ISO country code for CA)

# reload the driver (or just reboot):
sudo modprobe -r brcmfmac && sudo modprobe brcmfmac
```

Verify the option took and the card associates:

```sh
cat /sys/module/brcmfmac/parameters/feature_disable   # 532480  (= 0x82000)
nmcli device wifi connect "<SSID>" password "<PSK>"    # or your usual flow
ip -br addr show wlan0                                 # expect an IP + UP
```

## Known limitation: mesh AP still rejects association (`status 16`)

With the fix above the card connects fine to a **standard** access point (verified against a TELUS/Sagemcom gateway — DHCP lease, internet, stable). But against a **mesh** router in the same house — a Sagemcom-based mesh marketed locally as "TMD" — association still fails, with the AP returning **`status 16`** (a generic "association denied" from the AP).

This is an **AP-side interop problem, not a card fault**:

- The card's hardware and driver are proven good (they associate with the other AP on the same fix).
- `status 16` from a mesh AP typically means the AP is steering/roaming the client in a way the client can't satisfy — most often **802.11r fast-transition** or aggressive **band-steering** that `brcmfmac` doesn't negotiate cleanly.

**Workarounds (router-side, in order of preference):**
1. On the mesh, **disable 802.11r (fast roaming)** and/or **band-steering**, or split the 2.4/5 GHz SSIDs so the client binds to one band.
2. Connect to a different AP/SSID on the network that isn't doing the steering.
3. Leave the deck on the working gateway's SSID.

There is no reliable *client-side* fix for a mesh that insists on an FT/steering mode the firmware won't complete; the lever is on the router.

## Resume note

The BCM43602 firmware also **does not survive `s2idle` resume** — after a wake its message ring wedges and `wlan0` goes down (and the wedged device then blocks the next suspend). That's handled separately by a `systemd-sleep` unbind/rebind hook documented in the [suspend/resume writeup](../sleep-resume/#wi-fi-brcmfmac-wedges-its-firmware-on-resume). It's independent of the association fix here, but you'll want both for a usable laptop.

## Lessons

- **`feature_disable` is the brcmfmac escape hatch.** When association fails despite correct firmware and PSK, the offloaded handshake is the prime suspect — disable `FWSUP` (`0x2000`), add `FWAUTH` (`0x80000`) for `0x82000`.
- **`status 16` points at the AP.** If the same card associates with one AP and not another, stop debugging the client.
- **`brcmfmac`, not `wl`.** BCM43602 is FullMAC; the proprietary `broadcom-wl` is the wrong tree and will fight the right driver.

---

*Written up by [Nori Nishigaya](https://github.com/xeeban) · Victoria, BC. More at [Emergent Insights](https://emergentinsights.substack.com/).*
