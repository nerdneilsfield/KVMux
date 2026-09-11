# Hardware validation notes

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

SSH inspection of `jetson-hy` (Ubuntu, aarch64, `5.15.148-tegra`) identified
`1a86:7523`, a CH340-family host adapter. The kernel had
`CONFIG_USB_SERIAL_CH341` disabled. The initially loaded CH343 driver did not
match this device. After installing WCH CH341 V1.9 (2025.12), re-probing created
`ttyCH341USB0`, but the kernel explicitly logged BRLTTY setting configuration 1
and the CH341 driver disconnecting two seconds later.

`brltty.service` was inactive while `brltty-udev.service` remained running;
its udev rule matched `1a86/7523/*`. The user subsequently reported finding the
port. No CH9329 transaction or input-control acceptance was established by
this discovery. User-facing diagnosis and recovery steps are in
[LAN relay troubleshooting](lan-relay.md#troubleshooting-usb-serial-port-is-missing-on-jetsonubuntu).
