# Hardware validation notes

## 2026-11-09 macOS capture-mode selection

- Host: MacBook Pro `Mac17,8`, Apple M5 Pro, 64 GB; macOS 27.2
  (`26B123`), arm64 Debug build, AVFoundation.
- Card: `KUHAIMI 4K60 Version`, USB `048d:ca31`, serial `00000001`.
  The user connected it directly to the Mac's Type-C port. IORegistry showed
  its video and microphone functions below `USB2.0 Hub` (`05e3:0608`), with
  the video function negotiating 480 Mb/s. This does not establish a cable
  defect or the card's maximum capability on other hosts.
- Serial/CH9329: not connected or exercised; baud rate and chip configuration
  are not part of this capture-only check.
- Before the fix, all 12 enumerated modes delivered 1920x1080 samples, even
  when 640x480 or 1280x720 was requested. On this Mac, releasing the device
  configuration lock allowed the session's default mode to replace the
  selected mode. The lock is now retained until capture teardown.
- After the fix, all 12 modes delivered 100 samples of the requested size,
  and the active device format/frame duration matched the request: 640x480,
  1280x720 and 1920x1080, each at 25, approximately 30, 50 and approximately
  60 fps. The device advertises 30 as `3000003/100000` and 60 as
  `750003/12500`.
- A short sample-timestamp check, excluding the first ten samples, observed
  approximately 25/30/50/60 fps at 480p and 720p, and 25/30 fps at 1080p.
  The 1080p 50/60 modes delivered approximately 47 fps in this run; sustained
  full-rate delivery is not accepted. These are real capture samples, not
  glass-to-glass latency or long-run GUI performance measurements.
- 1440p and 4K were not enumerated by AVFoundation in this connection and
  remain unverified. No synthetic modes were added.

## Observed, read-only identification

- Capture card: `LCC2003B`, MACROSILICON, USB `345F:2130`, serial `20210623`.
- Prior macOS AVFoundation inspection listed `LCC2003B` as a video and audio
  device and successfully read UYVY frames at 1920x1080. An FFmpeg-reported
  `1000k` frame-rate metadata value is not valid evidence of 60 fps.
- The capture device also exposes a vendor HID feature report. It is not the
  design-specified CH9329 serial control channel and KVMux does not write it.
- The supplied control hardware is a CH9329 endpoint plus a CH340/CH343 host
  serial adapter. On the current Mac only debug/Bluetooth serial ports are
  enumerated. An old WCH macOS driver system extension is waiting for user and
  reports that it needs a developer update. KVMux does not lower macOS security
  settings to install it.

## Required future real-device procedure

1. Record Mac/Windows/Linux version, UVC/serial driver version, USB topology,
   CH9329 supply/configuration, selected port, baud rate and `GET_INFO` reply.
2. Verify target USB-ready followed by both initial clear mouse reports and
   keyboard clear acknowledgement before attempting capture.
3. Exercise preview, absolute/relative positioning, drag outside video bounds,
   scroll, left/right modifiers, quick taps, Host key, focus loss, stale video,
   serial unplug/replug and target USB loss.
4. Measure only with the design's stated clocks and external high-speed-video
   method. Do not use fake source values as KVM performance data.

## Jetson USB serial discovery

SSH inspection of a Jetson validation host (Ubuntu, aarch64, `5.15.148-tegra`) identified
`1a86:7523`, a CH340-family host adapter. The kernel had
`CONFIG_USB_SERIAL_CH341` disabled. The initially loaded CH343 driver did not
match this device. The installed driver came from
[WCHSoftGroup/ch341ser_linux](https://github.com/WCHSoftGroup/ch341ser_linux).
After installing WCH CH341 V1.9 (2025.12), re-probing created
`ttyCH341USB0`, but the kernel explicitly logged BRLTTY setting configuration 1
and the CH341 driver disconnecting two seconds later.

`brltty.service` was inactive while `brltty-udev.service` remained running;
its udev rule matched `1a86/7523/*`. The user subsequently reported finding the
port. No CH9329 transaction or input-control acceptance was established by
this discovery. User-facing diagnosis and recovery steps are in
[LAN relay troubleshooting](lan-relay.md#troubleshooting-usb-serial-port-is-missing-on-jetsonubuntu).

## CH341 direct (libusb) prerequisites

Build and install requirements are in [Building KVMux](building.md#ch341-direct-serial).
This section records what a hardware run still has to establish. No `1A86:5523`
device was attached on the Linux host that added the backend (`lsusb` showed
no `1a86` or `ch34` device). Enumeration of a real `ch341-usb:` port, the
open-failure text, a CH9329 session at 9600 through that port, and the macOS
default path are unverified. Pending a macOS run.

Before that run, the host has to meet one of these conditions:

- Linux: permission to open USB `1a86:5523`, and acceptance that opening a
  `ch341-usb:` name detaches the kernel driver on that device (usually
  `ch341`). The open does not fall back to a `/dev/tty*` node.
- Windows: the interface bound to WinUSB, and `LIBUSB_ROOT` set to the
  official prebuilt tree described in the build document.
- macOS: WCH `CH34xVCPDriver` left uninstalled. `KVMUX_CH341_USB` defaults to
  `ON`, so the libusb development files must be present at configure time.
  Auto-select may choose `ch341-usb:` when that device is listed.

A later record should include the OS, libusb version, USB topology, the
`ch341-usb:` port name, baud, which driver was bound before open, and whether
CH9329 reached ready. A successful build is not that record.
