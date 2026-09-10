#!/bin/sh
set -eu

# Locate the installer files beside this script, from any working directory.
installer_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

set --
if [ "$(id -u)" -ne 0 ]; then
    set -- sudo
fi
"$@" install -Dm755 "$installer_dir/lintaskmanager" /usr/bin/lintaskmanager
"$@" install -Dm644 "$installer_dir/lintaskmanager.desktop" /usr/share/applications/lintaskmanager.desktop
"$@" install -Dm644 "$installer_dir/lintaskmanager.png" /usr/share/icons/hicolor/256x256/apps/lintaskmanager.png

printf '\nInstalled. Open Linux Task Manager from your applications menu.\n'
