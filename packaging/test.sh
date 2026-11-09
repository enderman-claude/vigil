#!/bin/sh
# usage: packaging/test.sh <distro>  - installs dist/linux-<distro>.<format> and smoke-tests it
set -eu
cd "$(dirname "$0")/.."
D=${1:?usage: test.sh ubuntu|arch|redhat|opensuse}
case $D in
ubuntu)   dpkg -i dist/linux-ubuntu.deb ;;
arch)     pacman -U --noconfirm dist/linux-arch.pkg.tar.zst ;;
redhat|opensuse) rpm -i dist/linux-$D.rpm ;;
*) echo "unknown distro: $D" >&2; exit 2 ;;
esac
fail() { echo "::error::$1"; exit 1; }
BIN=$(command -v vigil) || fail "vigil not on PATH after install"
test -f /etc/vigil.conf || fail "/etc/vigil.conf missing"
test -f /usr/lib/systemd/system/vigil@.service || fail "unit file missing"
grep -q "^ExecStart=$BIN\$" /usr/lib/systemd/system/vigil@.service || fail "unit ExecStart does not match $BIN"
"$BIN" -1 > /tmp/vigil.out 2>/tmp/vigil.err || fail "vigil -1 failed: $(cat /tmp/vigil.err)"
cat /tmp/vigil.out
grep -q ' load ' /tmp/vigil.out || fail "no load line in output"
# containers may have no block-device mounts, so test disks with an explicit mount point
printf 'disks = /\nstyle = levels\n' > /tmp/vigil-test.conf
"$BIN" -1 -c /tmp/vigil-test.conf > /tmp/vigil.out 2>/tmp/vigil.err || fail "vigil -1 -c failed: $(cat /tmp/vigil.err)"
grep -q ' disk ' /tmp/vigil.out || fail "no disk line with disks = /"
echo "OK: $D"
