#!/system/bin/sh
# Magisk "Action" button — print probe
export PATH="/data/adb/modules/isodriveplus/system/bin:$PATH"
echo "=== ISODrive+ probe ==="
isodrive probe
echo
echo "Tap WebUI in the module page for the graphical panel."
sleep 2
