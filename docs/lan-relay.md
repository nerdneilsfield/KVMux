# LAN relay quick start

Run `kvmux-relay` on the Windows or Linux computer connected to the capture
card and CH340/CH341/CH343 host serial adapter connected to CH9329. Run the desktop GUI on your Mac.
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

## Debug logs

Both executables accept `--debug`. By default, diagnostic logging writes only
warnings and errors to stderr. Debug logs include timestamps, thread IDs and
levels. Relay command results still go to stdout.

```sh
build/linux-debug-headless/kvmux-relay --debug --serve --baud 9600 2>relay-debug.log
build/macos-debug/kvmux.app/Contents/MacOS/kvmux --debug 2>gui-debug.log
```

The relay and new GUI configurations default to 9600 baud. The CH9329 datasheet
(page 1) specifies this factory rate; the serial protocol document (page 11)
lists configuration field (4) as `0x00002580` (9600). Saved GUI baud settings
are preserved. If the chip was reconfigured, select its actual baud rate.

The relay accepts `--debug` before or after its command, including `--help`
and the device-list commands. Run `kvmux --debug --help` to check GUI options
without opening a window. Unknown GUI arguments fail with exit code 2.

Logs show serial open settings and errors, CH9329 handshake commands and ACKs,
response lengths, USB readiness, and capture, control and network state changes.
They do not contain HID input payloads, individual key events or raw byte dumps.
Capture state is observed outside native capture callbacks.

## Start with automatic selection

With one capture device and one known CH340/CH341/CH343 USB serial adapter:

```sh
build/linux-debug-headless/kvmux-relay --serve
```

The relay prints the selected device ID, mode index, dimensions, exact rational
frame rate, native/delivered formats, serial port and baud rate before opening
these devices. The default baud rate is 9600; use `--baud` to match your CH9329.

Automatic capture selection requires exactly one enumerated device. Multiple
devices produce an error listing candidates; choose one with `--device`.
Automatic mode selection requires both native and delivered MJPEG. It prefers
1080p60, 720p60, 1080p30, then 720p30. The 60/30 groups include 59.94/29.97
(including 60000/1001 and 30000/1001); the original rational rate is preserved.
Other usable MJPEG modes sort by descending pixel area, width, height, then
frame rate. Within a preferred group the higher frame rate wins. Exact ties
use the first enumerated index. Modes outside the capture dimension limits or
with invalid frame rates are not usable. Raw modes are never selected.

Automatic serial selection uses libserialport USB VID/PID metadata, not port
names. Supported adapter IDs are `1a86:7523`, `1a86:5523` and `1a86:55d3`. Exactly one match is required. Multiple matches produce an error
listing candidates. Board UARTs, other USB serial adapters and unknown WCH IDs
are not candidates; select them explicitly with `--serial` if appropriate.
A matching host adapter **does not prove CH9329 identity**, the correct baud
rate or a successful handshake. Enumeration does not open candidate ports;
the existing control worker performs the handshake after selection.

`--device`, `--mode-index` and `--serial` each override their automatic choice.
An invalid explicit choice fails; it never falls back to a different device,
mode or port. Device/port open failures also do not trigger a fallback.

## Inspect devices or select explicitly

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

## Start with explicit choices

Linux example (replace the device, mode index and serial port):

```sh
build/linux-debug-headless/kvmux-relay --serve --device "/dev/video0" --mode-index 0 --serial "/dev/ttyUSB0" --baud 9600
```

Windows example:

```powershell
build/windows-debug-headless/kvmux-relay.exe --serve --device "DEVICE_ID" --mode-index 0 --serial "COM3" --baud 9600
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
A matching CH341 driver had to be installed. For this Jetson setup, install
WCH's CH340/CH341 Linux serial driver:
[WCHSoftGroup/ch341ser_linux](https://github.com/WCHSoftGroup/ch341ser_linux).
Do not substitute the CH343 driver. If your adapter already works with the
kernel's built-in CH341 driver, you do not need to replace it.

Check your own kernel before installing:

```sh
modinfo ch341
zcat /proc/config.gz | grep CONFIG_USB_SERIAL_CH341
```

Some distributions do not provide `/proc/config.gz`; check their kernel config
under `/boot` instead. Any external module must match the running kernel and
architecture. Do not install a module built for another Ubuntu kernel.

Install a compiler, `make`, and development headers matching `uname -r` before
building. On Jetson, use the matching NVIDIA/L4T kernel headers; generic Ubuntu
headers for another kernel are not a substitute. Follow the upstream README:

```sh
git clone https://github.com/WCHSoftGroup/ch341ser_linux.git
cd ch341ser_linux/driver
make
sudo make install
```

Run the install command only if `make` succeeds and produces `ch341.ko`.
The upstream `sudo make install` target installs the driver for persistent use.
For a temporary load instead of installation, use `sudo make load` after
building. To remove a persistent installation later, run `sudo make uninstall`
from the same driver directory.

Reconnect the USB adapter and check the kernel log. If the port appears and
then disappears, continue with the BRLTTY checks below; reinstalling the driver
will not fix another service taking the device.

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

## Troubleshooting: video appears but input does not work

Seeing a picture does not prove the connection is live. If the GUI displays
`VIDEO UNAVAILABLE - control disabled`, it may be showing the last frame from
before a disconnect. Do not treat that image as a working control session.

1. Check the GUI's capture, serial and target USB states separately. Input
   requires fresh decoded video, ready serial/target USB state and confirmed
   input clearing. Click the video area to capture input only after these are
   ready; the first activation click is not sent to the target.
2. If the serial node exists, check its permissions and the **running relay
   process**, not only your current shell. On the tested Jetson the node was
   `root:dialout` with mode `0660`, but the relay user was not in `dialout`.
   Device enumeration worked while opening the port could not succeed.
3. Add the user to the node's group if needed, as described above. Log out,
   log back in, confirm `id` contains `dialout`, stop the old relay with Ctrl+C,
   and start it from the new login. An already-running relay does not acquire
   new supplementary groups when `usermod` changes the account.
4. If the port is accessible but control is still not ready, check the selected
   baud rate, CH9329 target-end USB connection and the GUI's serial error. A
   listening TCP server is not evidence of a successful CH9329 handshake.

### Read the disconnect reason

| GUI error | Meaning and next check |
| --- | --- |
| `GUI heartbeat expired` | GUI progress stopped for 250 ms. Check UI/render stalls; a running video network thread does not renew input authority. |
| `Control connection failed: ...` | Check the relay address, listening control port and firewall. |
| `Control status read failed or timed out` | The expected status did not arrive. Check relay output and whether either channel closed; this message alone cannot identify the server-side cause. |
| `Control send failed or timed out` | The control socket could not complete a write within its deadline. Check connection and relay state. |
| `Video connection lost or stale` | No complete video packet arrived within the client's read deadline, or the video stream was invalid/closed. Check capture and network state. |

A video-channel failure can also end the paired control session. A subsequent
control error does not necessarily mean the serial cable disconnected. Older
builds collapsed several causes into `Control connection lost`; update both
ends before collecting another failure report.

### Upside-down MJPEG or repeated swscale warnings

Older GUI builds inverted the video texture's vertical coordinates and sent
MJPEG `YUVJ420P/422P/444P` through CPU conversion. This could produce repeated:

```text
deprecated pixel format used, make sure you did set range correctly
```

The corrected renderer uses the existing planar GPU paths for these formats,
keeps full-range semantics and uses the correct display texture coordinates.
Update and rebuild the Mac GUI rather than suppressing FFmpeg logs. This warning
is not a CH9329 or serial-permission error.

## Rebuild and restart after updating source

After obtaining the updated source on each machine, stop its old process and
build the matching platform preset. On the Jetson:

```sh
cmake --preset linux-release-headless
cmake --build --preset linux-release-headless
./build/linux-release-headless/kvmux-relay --serve
```

On the Mac, quit the old KVMux application, then:

```sh
cmake --preset macos-debug
cmake --build --preset macos-debug
open build/macos-debug/kvmux.app
```

Reconnect with **Remote** using the Jetson's LAN IPv4 address. Rebuilding does
not replace a process already running from an older executable. If a failure
remains, report the exact GUI error, relay output, selected capture mode and
serial settings; a screenshot of the retained video alone is insufficient.

### Target Caps Lock changes

With `--debug`, the relay reports the first observed CH9329 keyboard LED byte
and subsequent changes, including `caps_lock`, `num_lock` and `scroll_lock`.
These are target-reported states; the relay does not toggle Caps Lock to make
it match the Mac. GET_INFO polls only while the serial worker is idle, so the
log is not an exact timestamp for every target-side change.

If Windows unexpectedly types uppercase, distinguish an active Caps Lock
indicator from a held Shift key. Include the LED log before and after input
capture and say whether the change occurs on connection or on the first click.
A Mac `TSM ... CapsLockLED ... Inhibit` message alone does not show the Windows
Caps Lock state. Input regression tests found no Caps Lock event generated by
the activation click; the reported real-device cause remains unconfirmed.

### Green video after reconnecting

A renderer defect retained plane-size caches after deleting the OpenGL textures.
Reconnecting at the same resolution could update a new texture without allocating
its storage, producing green video and `GLD_TEXTURE_INDEX_2D ... texture unloadable`.
The fix resets allocation caches and reallocates storage when the internal pixel
format changes. Rebuild and restart the Mac GUI to use the fix.

A real offscreen OpenGL regression reproduces the old failure and passes with
the fix, including same-size reconnect, resolution/format changes and ImGui
rendering. This fix does not establish the cause of earlier TCP send failures.

### Caps Lock and macOS input-source switching

If macOS uses Caps Lock to switch to the ABC input source, a short press may
switch the local input source rather than produce the Caps Lock event intended
for Windows. Temporarily disable that option in macOS keyboard/input-source
settings to test forwarding. This setting alone does not explain a target that
already has Caps Lock enabled before connection.

KVMux does not intentionally synchronize the Mac's lock state to Windows.
ReleaseAll releases held keys; it does not turn off target Caps Lock. Compare
the first target keyboard LED report before connecting the GUI with subsequent
changes, rather than sending an extra Caps Lock toggle on every connection.
