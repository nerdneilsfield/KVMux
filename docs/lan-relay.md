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

## Troubleshooting: USB serial port is missing on Jetson/Ubuntu

Do not select a board UART such as `ttyTHS*`, `ttyTCU0` or `ttyAMA0` simply
because it appears in `--list-serial`. Identify the USB adapter first:

```sh
lsusb
lsusb -t
journalctl -k -n 40 --no-pager
```

### Check the adapter ID and matching driver

The control cable tested on Jetson identified as **`1a86:7523` (CH340 family)**,
not CH343. Installing a `ch343` driver does not provide support for this ID.

On the tested `5.15.148-tegra` kernel, `CONFIG_USB_SERIAL_CH341` was disabled.
A matching CH341 driver had to be installed. Check your own kernel before
installing anything:

```sh
modinfo ch341
zcat /proc/config.gz | grep CONFIG_USB_SERIAL_CH341
```

Some distributions do not provide `/proc/config.gz`; check their kernel config
under `/boot` instead. Any external module must match the running kernel and
architecture. Do not install a module built for another Ubuntu kernel.

The mainline driver commonly creates `/dev/ttyUSB0`. The tested WCH CH341
V1.9 driver instead creates **`/dev/ttyCH341USB0`**. The numeric suffix can vary.
Use the node actually reported by the driver, not a guessed name.

### Check whether BRLTTY takes the device

On Ubuntu, BRLTTY's udev rules can match `1a86:7523` and claim the adapter as a
Braille device. In the observed failure, the serial node appeared and then
vanished two seconds later. The kernel reported:

```text
ttyCH341USB0: ch341 USB device
interface 0 claimed by usb_ch341 while 'brltty' sets config #1
ch341 usb device disconnect.
```

Check **both** services:

```sh
systemctl status brltty.service brltty-udev.service --no-pager
```

Stopping or disabling `brltty.service` alone is not sufficient: the separate
`brltty-udev.service` can remain running. `Driver=usbfs` in `lsusb -t` suggests
userspace ownership, but does not by itself prove that BRLTTY is responsible;
confirm with the service state and kernel log.

**If this host does not use Braille devices**, temporarily stop and mask the
udev service, then stop the ordinary service:

```sh
sudo systemctl mask --runtime --now brltty-udev.service
sudo systemctl stop brltty.service
systemctl is-active brltty-udev.service
```

Expect `inactive` from the last command; its nonzero exit status is normal.
Check any errors from the mask command before continuing. Unplug and reconnect
only the USB serial adapter on the relay host, then repeat `lsusb -t` and
`kvmux-relay --list-serial`.

If physical reconnection is impractical, re-probe the adapter's **current USB
interface ID**, obtained from sysfs/udev. For example, `1-2.1:1.0` was the ID on
the tested Jetson, but it is not a universal value:

```sh
printf '%s' 'YOUR_USB_INTERFACE_ID' | sudo tee /sys/bus/usb/drivers_probe
```

The runtime mask lasts only until reboot. To retain the workaround on a host
that does not need BRLTTY, replace it with a persistent mask:

```sh
sudo systemctl mask --now brltty-udev.service
```

To undo these masks:

```sh
sudo systemctl unmask brltty-udev.service
sudo systemctl unmask --runtime brltty-udev.service
```

This workaround disables BRLTTY's udev service for all matching devices. Do not
use it on a host that needs Braille-device support without arranging a narrower
device-specific rule.

### Check permissions after the node appears

Inspect the actual node, for example:

```sh
ls -l /dev/ttyCH341USB0
id
```

If it belongs to group `dialout` and your user is not a member:

```sh
sudo usermod -aG dialout "$USER"
```

Log out and reconnect before starting the relay. Do not use `chmod 777` or run
the entire relay as root to bypass device permissions. If the node exists but
KVMux does not list it, record its exact path and ownership; driver binding,
permissions and application enumeration are separate checks.

A visible serial port proves only device enumeration. It does not prove a
successful CH9329 handshake, the correct baud rate, or keyboard/mouse control.
