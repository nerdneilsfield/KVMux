# LAN relay quick start

Run `kvmux-relay` on the Windows or Linux computer connected to the capture
card and CH343/CH9329 serial adapter. Run the desktop GUI on your Mac.
The relay does not need a desktop session.

This version uses two unencrypted, unauthenticated TCP connections, as requested
for a controlled LAN. Do not forward these ports to the Internet. Anyone with
access to the ports can attempt to control the attached computer.

## Build the hardware-side relay

Install the repository's documented compiler, CMake, Ninja and FFmpeg development
dependencies. From the repository root:

```sh
cmake --preset linux-debug-headless
cmake --build --preset linux-debug-headless
ctest --preset linux-debug-headless
```

This preset does not configure or build SDL, ImGui, glad or OpenGL.
The executable is `build/linux-debug-headless/kvmux-relay` on Linux and
`build/windows-debug-headless/kvmux-relay.exe` on Windows. On Windows, run the build from an
MSVC developer shell, replace the preset with `windows-debug-headless`, and make the FFmpeg runtime DLLs available on PATH.

## Select capture and serial devices

On Linux:

```sh
build/linux-debug-headless/kvmux-relay --list-devices
build/linux-debug-headless/kvmux-relay --list-modes "DEVICE_ID"
build/linux-debug-headless/kvmux-relay --list-serial
```

Replace `DEVICE_ID` with the exact ID from the first command. On Windows, use
`build/windows-debug-headless/kvmux-relay.exe` for the same commands. Quote IDs containing
spaces or special characters.

Choose a mode whose native format is **MJPEG**. Mode numbers start at zero and
refer to the current enumeration. This first relay implementation does not
encode raw YUY2, UYVY, NV12 or BGRA. Selecting such a mode produces an error;
it does not silently change the requested mode.

## Start the relay

Linux example (replace the device, mode index and serial port):

```sh
build/linux-debug-headless/kvmux-relay --serve --device "/dev/video0" --mode-index 0 --serial "/dev/ttyUSB0" --baud 57600
```

Windows example:

```powershell
build/windows-debug-headless/kvmux-relay.exe --serve --device "DEVICE_ID" --mode-index 0 --serial "COM3" --baud 57600
```

The defaults are `0.0.0.0:17000` for control and `0.0.0.0:17001` for video.
Allow inbound TCP on both ports in the relay computer's firewall, restricted to
the intended LAN. To bind a particular interface, add `--bind 192.168.1.20`.
Use `--control-port` and `--video-port` to change the ports. Press Ctrl+C to stop.

Linux needs permission to open both the video device and serial port. Windows
needs a working serial driver on the relay host; the Mac does not need that
driver. The baud rate must match the CH9329 configuration.

## Connect the GUI

Build the desktop application with the existing `macos-debug` preset. In the GUI:

1. Select **Remote**.
2. Enter the relay computer's IPv4 address, not the Mac's loopback address.
3. Set the control and video ports to match the relay.
4. Select **Connect relay**.
5. Wait for a new video frame and ready/cleared serial status before clicking
   the video area to capture input. The Host key releases input locally.

Only one controller is supported. Reconnection starts in Preview and never
restores held keys. The relay pairs the video connection with the control
session; this pairing is not authentication.

## Limits and verification

Video and control use separate TCP sockets, but share the network's bandwidth.
The sender replaces only unsent frames. Bytes already queued in TCP cannot be
retracted; a slow video connection is closed on its write deadline.

A missing GUI heartbeat, disconnected channel or stale capture requests serial
ReleaseAll. If the serial connection itself is lost, software cannot guarantee
that the target received the release. Reconnect the target HID if needed.

Local automated tests use loopback TCP and fake capture/serial boundaries.
They are not real capture-card or CH9329 hardware acceptance. Windows/Linux
native relay operation and two-host hardware behavior still need measurements
on those hosts. See `acceptance.md` for the existing local-KVM evidence.
