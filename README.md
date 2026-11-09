# vigil

Read-only, real-time system status display for a Linux TTY. Runs **in place of getty** (e.g. on tty1): no login prompt, no input, just live load and resource info.

Shows: hostname/clock, OS/kernel/uptime, load (1/5/15 + bar), PSI stalls, CPU % and temp, memory, swap, disk (`/`), network rx/tx rate, battery, and per-core CPU bars. Adapts to terminal size.

## Trust model ("almost zero trust")

- Never execs, never opens sockets, never writes anywhere but the tty
- Started as root, it drops to an unprivileged uid (default 65534) immediately after setup, verifies root can't be regained, and sets `no_new_privs`
- All keyboard input is discarded (no echo, no signals, no line editing)
- Everything read from `/proc`, `/sys`, `/etc` is size-bounded, range-checked, and stripped of non-printable bytes before display
- Trusted: the kernel's `/proc` and `/sys`, and the tty it was handed
- systemd unit adds `ProtectSystem=strict`, `IPAddressDeny=any`, `CapabilityBoundingSet=CAP_SETUID CAP_SETGID`, etc.

Because there is no login, use another TTY (or SSH) to get a shell.

## Build / install

```sh
make
sudo make install                    # /usr/local/bin/vigil + systemd unit
sudo systemctl disable --now getty@tty1.service
sudo systemctl enable --now vigil@tty1.service
```

Any TTY works: `vigil@tty2.service`, etc. To revert: disable `vigil@tty1`, enable `getty@tty1`.

## Usage

```
vigil [-i seconds] [-u uid] [-B]
  -i N    refresh interval 1-60 (default 1)
  -u UID  uid to drop to when started as root (default 65534)
  -B      disable console blanking/powerdown
```

Can also be run in any terminal for testing.

## License

None chosen yet.
