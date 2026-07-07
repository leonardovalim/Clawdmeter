"""Hand-bumped short version tag for the Windows daemon/tray.

Mirrors firmware/src/version.h's FIRMWARE_VERSION: short and manually
controlled rather than derived from git, so it stays simple to read at a
glance. Bump whenever you ship something worth telling apart in the tray
menu: v1.0, v1.0a, v1.0b, ... v1.1, ...
"""

DAEMON_VERSION = "v1.1"
