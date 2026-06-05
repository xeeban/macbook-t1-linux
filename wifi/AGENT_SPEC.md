# Agent Spec — Make Wi-Fi work on a T1 MacBook Pro (Linux)

**Purpose:** Hand this file to a coding agent (Claude Code, etc.) and say:

> *"Follow `AGENT_SPEC.md` to get my MacBook's built-in Wi-Fi associating. Stop at the GO/NO-GO gate and tell me if the limitation about mesh routers applies to me."*

Self-contained spec + plan with verification gates and the guardrails that encode mistakes already paid for. **Read the whole file before running anything.**

---

## 0. Mission & definition of done

**Goal:** The internal **Broadcom BCM43602** (`brcmfmac`) associates with a WPA2 AP and gets working internet, persistently across reboots.

**Done when ALL are true:**
- [ ] `cat /sys/module/brcmfmac/parameters/feature_disable` → `532480` (`0x82000`).
- [ ] The card associates with a standard AP and gets an IP (`ip -br addr show <iface>` shows `UP` + address).
- [ ] Internet works (`ping -c1 1.1.1.1`).
- [ ] Survives a reboot (config is in `/etc/modprobe.d/`, not a runtime poke).

**Known non-goal:** associating with a **mesh** router that enforces 802.11r/band-steering may be impossible client-side — see Gate C-1.

---

## 1. Preconditions — verify BEFORE touching anything

```sh
cat /sys/devices/virtual/dmi/id/product_name        # MacBookPro13,2 / T1-class
lspci -nn | grep -i network                         # expect Broadcom BCM43602 [14e4:43ba]
lsmod | grep -E 'brcmfmac|wl|b43'                    # brcmfmac should be the one loaded
dmesg | grep -i brcmfmac | grep -i firmware          # firmware should load (brcmfmac43602-pcie.bin)
nmcli -t -f WIFI g 2>/dev/null; rfkill list          # wifi not blocked
```

**Hard gate P-1:** If the chip is **not** BCM43602 (`14e4:43ba`), this exact `feature_disable` value may not be right for your part — confirm the symptom (scans work, association/handshake fails) before applying. If `broadcom-wl`/`wl` or `b43` is loaded, that's the wrong driver for this chip; plan to remove it so `brcmfmac` binds.

---

## 2. Background the agent must hold in context

- BCM43602 is a **FullMAC** part → **`brcmfmac`** is correct. **Do not** install `broadcom-wl`/`wl` (older `b43`-era; it conflicts).
- The bug: `wpa_supplicant` 2.11 + this firmware's **offloaded 4-way handshake** (`FWSUP`) / offloaded auth (`FWAUTH`) fails to complete → association times out even with the correct PSK.
- The fix: `brcmfmac feature_disable` bitmask. `0x2000` = `FWSUP`, `0x80000` = `FWAUTH`; **`0x82000`** disables both → host-side handshake via `wpa_supplicant` (the reliable path).
- `feature_disable` is read at module load → it must go in **`/etc/modprobe.d/`** and the module reloaded/rebooted; setting it at runtime alone won't persist.
- **`status 16`** during association points at the **AP** (mesh/steering), not the card — see §4 Gate C-1.

---

## 3. Plan overview

```
A. Apply feature_disable + regdom  → B. Reload driver & verify flag  🚦
C. Associate & test internet       → D. Reboot-persist check          🚦
E. Hygiene
```

---

## 4. The phases

### Phase A — Apply the fix
```sh
echo 'options brcmfmac feature_disable=0x82000' | sudo tee /etc/modprobe.d/brcmfmac.conf
sudo pacman -S --needed wireless-regdb          # regulatory domain (separate but recommended)
# set country (substitute your ISO code), e.g. CA:
sudo sed -i 's/^#\?WIRELESS_REGDOM=.*/WIRELESS_REGDOM="CA"/' /etc/conf.d/wireless-regdom 2>/dev/null || true
# if broadcom-wl/b43 is present, remove it so brcmfmac binds:
#   sudo pacman -R broadcom-wl  (or broadcom-wl-dkms)
```

### Phase B — Reload & verify the flag took 🚦
```sh
sudo modprobe -r brcmfmac 2>/dev/null && sudo modprobe brcmfmac    # or reboot
cat /sys/module/brcmfmac/parameters/feature_disable                # 532480 (=0x82000)
```
**GATE B-1:** `feature_disable` must read `532480`. If it reads `0`, the modprobe.d file wasn't picked up (check filename ends in `.conf`, no typo) or the module didn't actually reload (something held it — reboot instead).

### Phase C — Associate & test 🚦
```sh
nmcli device wifi connect "<SSID>" password "<PSK>"
ip -br addr show                                  # the wifi iface should be UP with an IP
ping -c1 1.1.1.1
```
**GATE C-1:** If it associates and pings → **GO to Phase D.**
If association fails with **`status 16`** (check `journalctl -u wpa_supplicant -b` / `journalctl -u NetworkManager -b | grep -i assoc`):
- This is almost certainly a **mesh/steering AP** rejecting the client (802.11r fast-transition or band-steering), **not** the card.
- **Confirm the card is fine** by associating with a *different*, non-mesh AP/SSID (phone hotspot works as a test).
- **The lever is router-side:** disable 802.11r / band-steering on the mesh, or split 2.4/5 GHz SSIDs, or use a non-steering AP. **Report this to the human — do not keep editing client config.** There is no reliable client-side fix.

### Phase D — Reboot-persist check 🚦
```sh
sudo reboot
# after reboot:
cat /sys/module/brcmfmac/parameters/feature_disable   # still 532480
nmcli -t -f STATE g                                    # connected
```
**GATE D-1:** Flag persists and the card auto-connects.

### Phase E — Hygiene
- Remove any runtime-only experiments; the only persistent change should be `/etc/modprobe.d/brcmfmac.conf` (+ regdom).
- If you removed `broadcom-wl`, confirm nothing in `/etc/modprobe.d/` still blacklists `brcmfmac`.
- Remove any temporary passwordless-sudo grant you created; validate with `visudo -c`.
- **Resume:** if this machine also suspends, note that `brcmfmac` doesn't survive `s2idle` resume — install the unbind/rebind sleep hook from the [`sleep-resume`](../sleep-resume/) writeup.

---

## 5. Guardrails (the expensive lessons)

1. **`brcmfmac`, not `wl`.** BCM43602 is FullMAC; the proprietary driver is the wrong tree.
2. **`feature_disable` must be persistent** — `/etc/modprobe.d/*.conf`, then reload/reboot.
3. **`status 16` = blame the AP.** If one AP works and another doesn't, it's mesh/steering, not the client. Stop editing client config and tell the human to adjust the router.
4. **Firmware/password are usually innocent** — the card scans and the PSK is right; it's the offloaded handshake.
5. **Sudoers safety:** validate with `visudo -c`; never leave a `NOPASSWD: ALL` grant behind.

## 6. Rollback
```sh
sudo rm /etc/modprobe.d/brcmfmac.conf
sudo modprobe -r brcmfmac && sudo modprobe brcmfmac   # or reboot
```

## 7. What to report back to the human
Chip ID confirmation, that `feature_disable=0x82000` took (`532480`), the association result, and — if you hit `status 16` — an explicit statement that it's a mesh/AP-side limitation with the router-side options, plus confirmation that the card associates with a non-mesh AP.
