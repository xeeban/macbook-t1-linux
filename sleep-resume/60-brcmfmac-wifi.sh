#!/bin/bash
# Writer's-deck (MacBookPro13,2): the BCM43602 brcmfmac firmware does NOT survive
# s2idle resume — its msgbuf commonring wedges (scans fail ENOMEM) and the wedged
# device then returns -5 from .suspend, BLOCKING the next suspend. Keep the card OUT
# of the suspend path by unbinding it in pre, and re-probe fresh firmware in post.
# `modprobe -r brcmfmac` fails ("module in use", NM/wpa_supplicant), so use PCI
# driver unbind/rebind. Mirrors the apple-ibridge touchbar sleep hook.
DEV="0000:02:00.0"
DRV="/sys/bus/pci/drivers/brcmfmac"
case "$1" in
  pre)
    if [ -e "$DRV/$DEV" ]; then
      echo "$DEV" > "$DRV/unbind" 2>/dev/null \
        && logger -t brcmfmac-sleep-hook "unbound $DEV before $2" \
        || logger -t brcmfmac-sleep-hook "unbind $DEV FAILED before $2"
    fi
    ;;
  post)
    if [ ! -e "$DRV/$DEV" ] && [ -e "/sys/bus/pci/devices/$DEV" ]; then
      echo "$DEV" > "$DRV/bind" 2>/dev/null \
        && logger -t brcmfmac-sleep-hook "rebound $DEV after $2" \
        || logger -t brcmfmac-sleep-hook "rebind $DEV FAILED after $2"
    fi
    ;;
esac
exit 0
