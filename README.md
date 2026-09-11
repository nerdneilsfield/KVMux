# KVMux

USB KVM desktop application with a headless LAN relay. The relay host connects
to the capture card and CH9329 serial control cable; the desktop client displays
video and forwards captured input.

- [Build and platform presets](docs/building.md)
- [LAN relay: automatic selection, connection and troubleshooting](docs/lan-relay.md)
- [Hardware identification and validation](docs/hardware-validation.md)
- [Acceptance evidence and remaining checks](docs/acceptance.md)

The LAN relay currently requires native MJPEG and uses unauthenticated,
unencrypted TCP for a controlled network. Hardware validation is incomplete;
software tests are not a claim that every platform/device combination works.
