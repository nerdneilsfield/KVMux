# KVMux

USB KVM desktop application with a headless LAN relay. The relay host connects
to the capture card and CH9329 serial control cable; the desktop client displays
video and forwards captured input.

- [Build and platform presets](docs/building.md)
- [LAN relay: automatic selection, connection and troubleshooting](docs/lan-relay.md)
- [Hardware identification and validation](docs/hardware-validation.md)
- [Acceptance evidence and remaining checks](docs/acceptance.md)

The LAN relay sends native MJPEG or encodes raw capture as H.265. It uses two
server UDP ports, KCP control, and bounded media with XOR recovery. It has no
authentication or encryption; use a trusted LAN only. `--transport-rate` caps
UDP media bytes/s separately from the H.265 `--bitrate` in bits/s. Receiver
feedback slows source admission; it does not change codec bitrate at runtime.
See the [LAN guide](docs/lan-relay.md#transport-cap-and-source-admission) for
limits and blocked-frame recovery. Hardware validation is incomplete; software
tests do not establish that every platform/device combination works.
