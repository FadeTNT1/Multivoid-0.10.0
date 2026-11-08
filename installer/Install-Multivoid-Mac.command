#!/bin/bash
# Double-click launcher for macOS: opens Terminal and runs install-multivoid.sh from this folder.
# First time, Gatekeeper may block it: right-click the file, choose Open, then Open again.
cd "$(dirname "$0")" || exit 1
# Files unzipped from a download carry a quarantine flag; clear it on this folder (errors are harmless).
xattr -dr com.apple.quarantine . 2>/dev/null
chmod +x ./install-multivoid.sh 2>/dev/null
MULTIVOID_LAUNCHER=1 /bin/bash ./install-multivoid.sh "$@"
status=$?
printf '\nPress Enter to close this window. '
read -r _
exit $status
