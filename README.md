# vigil

Read-only, real-time system status display for a Linux TTY. Runs **in place of getty** (e.g. on tty1): no login prompt, no input, just live load and resource info. Because there is no login, keep another TTY (or SSH) for a shell.

Shows: hostname/clock, OS/kernel/uptime, load (1/5/15 + bar), PSI stalls, CPU % and temp, memory, swap, **one line per disk/partition**, network rx/tx rate, battery, and per-core CPU bars. Adapts to terminal size. Sections can be reordered or hidden.

## Install

Packages are built by CI (see Actions → latest run → Artifacts):

| Distro | File |
|---|---|
| Ubuntu | `linux-ubuntu.deb` |
| Arch | `linux-arch.pkg.tar.zst` |
| Red Hat | `linux-redhat.rpm` |
| openSUSE | `linux-opensuse.rpm` |

Then:

```sh
sudo systemctl disable --now getty@tty1.service
sudo systemctl enable --now vigil@tty1.service     # any tty works: vigil@tty2, ...
```

Revert: disable `vigil@tty1`, enable `getty@tty1`.

From source (no distro assumptions; paths are overridable):

```sh
make
sudo make install            # PREFIX, BINDIR, SYSCONFDIR, UNITDIR, DESTDIR
```

## Configuration: `/etc/vigil.conf`

`key = value`, `#` at line start is a comment. See the shipped file for every key. Restart to apply: `systemctl restart vigil@tty1`. Command-line flags override it. Bad lines are reported to the journal and skipped.

- `sections` - order/subset of `load psi cpu mem swap disk net bat cores`
- `disks` - `auto` (every real block-device mount, deduplicated per device; loop/squashfs/erofs excluded) or a list like `/,/boot,/home`; `disk_max` caps lines
- `style` - preset `classic` (coloured `#`), `mono` (no colour), `levels` (no colour, bar character changes with level)
- `fill`, `fill_low`, `fill_mid`, `fill_high`, `empty`, `left`, `right` - any single printable ASCII character, e.g. `- = _ * ~ #`
- `color`, `color_low/mid/high/empty/title/dim` - 16-colour names
- `warn`, `crit` - level thresholds (%); `bar_width`; `interval`; `uid`; `disable_blanking`

Example, a colourless bar that changes character with load:

```
style = mono
fill_low  = -
fill_mid  = =
fill_high = *
empty     = _
```

## Trust model ("almost zero trust")

- Never execs, never opens sockets, never writes anywhere but the tty
- Started as root, it drops to an unprivileged uid (default 65534) after setup, verifies root can't be regained, and sets `no_new_privs`
- All keyboard input is discarded (no echo, no signals, no line editing)
- Values from `/proc`, `/sys`, `/etc` are size-bounded, range-checked, and stripped of non-printable bytes; config values are ASCII-only and validated, so no escape sequences can be injected
- Started as root, the config file must be root-owned and not group/world-writable, or it is ignored
- Trusted: the kernel's `/proc` and `/sys`, and the tty it was handed
- The systemd unit adds `ProtectSystem=strict`, `IPAddressDeny=any`, `CapabilityBoundingSet=CAP_SETUID CAP_SETGID`, etc.

## Usage

```
vigil [-c file] [-i seconds] [-u uid] [-B] [-1]
  -1   print one plain frame to stdout and exit (works without a tty; used by CI)
```

## License

None chosen yet.
