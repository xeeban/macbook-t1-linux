#!/bin/bash
# Step 2: the real test -- ONE manual hibernate.
# SAVE YOUR WORK FIRST. The machine will power off. Press the power button to resume.
# With the hook fix in place, the Touch Bar unload no longer runs on hibernate,
# so the kernel deadlock that forced your hard reboot cannot occur on this path.
set -u

echo "About to hibernate this machine."
echo "  - Save any open work now."
echo "  - When the screen powers off, press the POWER button to resume."
echo "  - On resume, LOOK AT THE TOUCH BAR: is it lit and responsive?"
echo
read -r -p "Type 'go' then Enter to hibernate (anything else aborts): " ans
if [ "$ans" != "go" ]; then
    echo "aborted."
    exit 1
fi

echo "hibernating..."
sudo systemctl hibernate

# Execution resumes HERE after you power back on.
echo
echo "=== resumed ==="
echo "Now run ./test-03-check-resume.sh and note whether the Touch Bar is lit."
