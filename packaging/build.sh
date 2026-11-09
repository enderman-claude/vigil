#!/bin/sh
# usage: packaging/build.sh <ubuntu|arch|redhat|opensuse>
# Produces dist/linux-<distro>.<format>:
#   ubuntu -> .deb   arch -> .pkg.tar.zst   redhat, opensuse -> .rpm
# Needs only a C compiler, make, and the distro's own packaging tools.
set -eu
cd "$(dirname "$0")/.."
D=${1:?usage: build.sh ubuntu|arch|redhat|opensuse}
V=$(cat VERSION)
OUT=$PWD/dist
mkdir -p "$OUT"
UNITDIR=/usr/lib/systemd/system

tarball() { # source tarball with a vigil-<version>/ prefix
	tar -czf "$1" --exclude=./.git --exclude=./dist --exclude=./vigil \
		--transform "s,^\.,vigil-$V," .
}

case $D in
ubuntu)
	make clean
	R=$(mktemp -d); chmod 755 "$R"
	make install DESTDIR="$R" PREFIX=/usr SYSCONFDIR=/etc UNITDIR=$UNITDIR
	mkdir -m 755 "$R/DEBIAN"
	cat > "$R/DEBIAN/control" <<EOF
Package: vigil
Version: $V
Section: admin
Priority: optional
Architecture: $(dpkg --print-architecture)
Maintainer: enderman-claude <enderman-claude@users.noreply.github.com>
Depends: libc6
Description: read-only real-time system status display that replaces getty
 Shows load, CPU, memory, disks, network and battery on a Linux TTY.
 Takes no input and drops privileges after start-up.
EOF
	echo /etc/vigil.conf > "$R/DEBIAN/conffiles"
	dpkg-deb --root-owner-group --build "$R" "$OUT/linux-ubuntu.deb"
	;;
arch)
	id builder >/dev/null 2>&1 || useradd -m builder
	W=$(mktemp -d); chmod 755 "$W"
	tarball "$W/vigil-$V.tar.gz"
	sed "s/@VERSION@/$V/" packaging/arch/PKGBUILD.in > "$W/PKGBUILD"
	chown -R builder "$W"
	su builder -s /bin/sh -c "cd $W && makepkg -f"
	cp "$W"/vigil-*.pkg.tar.zst "$OUT/linux-arch.pkg.tar.zst"
	;;
redhat|opensuse)
	T=$(mktemp -d)
	mkdir -p "$T/SOURCES" "$T/SPECS"
	tarball "$T/SOURCES/vigil-$V.tar.gz"
	sed "s/@VERSION@/$V/" packaging/rpm/vigil.spec.in > "$T/SPECS/vigil.spec"
	rpmbuild --define "_topdir $T" -bb "$T/SPECS/vigil.spec"
	cp "$T"/RPMS/*/vigil-*.rpm "$OUT/linux-$D.rpm"
	;;
*)
	echo "unknown distro: $D" >&2; exit 2
	;;
esac
ls -l "$OUT"
