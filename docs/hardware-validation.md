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
