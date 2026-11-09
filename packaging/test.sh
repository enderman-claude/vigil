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
BIN=$(command -v vigil)
test -f /etc/vigil.conf
test -f /usr/lib/systemd/system/vigil@.service
grep -q "^ExecStart=$BIN\$" /usr/lib/systemd/system/vigil@.service
"$BIN" -1 | tee /tmp/vigil.out
grep -q ' load ' /tmp/vigil.out
grep -q ' disk ' /tmp/vigil.out
"$BIN" -1 -c /etc/vigil.conf >/dev/null
echo "OK: $D"
