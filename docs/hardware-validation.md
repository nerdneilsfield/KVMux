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
CH9329 reached ready. A successful build is not that record. The commands that
collect it are in the next section. No macOS run of those commands is recorded
here.

## WCH 直连家族（2026-11-14）

直连后端按 PID 决定列不列出。判定和第三代单串口的 8N1 分频有 CTest
`ch341_direct`。没有芯片插在这台机器上。CH343、CH9102、CH9101、CH9111
依据 WCH ch343 驱动确认的协议事实实现，按官方驱动源码实现，未经硬件验证。
CH340、CH340K、CH341 沿用原有厂商请求，同样未经硬件验证。实现依据写在
`third_party/INVENTORY.md` 的 “WCH serial protocol”。

| 家族 | PID | 软件 | 硬件 |
| --- | --- | --- | --- |
| CH340 | `7523` | 已实现，走原 CH341 厂商请求。CTest 只检查会被列出。 | 未验证 |
| CH340K | `7522` | 同上。 | 未验证 |
| CH341 串口 | `5523` | 原实现。端口名和分频有 CTest。 | 未验证。此前记录的 macOS `/dev/cu.usbserial-20110` 没有对应的 `ch341-usb:` 日志，芯片 PID 仍未知。 |
| CH343 | `55d3` | 已实现。单串口，8N1 分频有 CTest。依据 WCH ch343 驱动确认的协议事实，没有拷贝该驱动。 | 按官方驱动源码实现，未经硬件验证 |
| CH9102 | `55d4` | 同上。 | 按官方驱动源码实现，未经硬件验证 |
| CH9101 | `55d8` | 同上。 | 按官方驱动源码实现，未经硬件验证 |
| CH9111 | `55e9`、`55ea` | 同上。只覆盖 CH9329 的九个波特率。 | 按官方驱动源码实现，未经硬件验证 |
| CH342、CH344、CH9103、CH347、CH9104、CH9114、CH346、CH9105 | `55d2`、`55d5`、`55d7`、`55da`、`55df`、`55e8`、`55ec`、`55ef` | 枚举会跳过，原因是多串口，未支持，请使用系统驱动。`55d5` 是 CH344，`55d7` 是 CH9103。 | 未实现，未验证 |
| CH9143 | `55d6` | 枚举会跳过。驱动表里有这个 PID，直连未实现，请使用系统驱动。 | 未实现，未验证 |
| 需要指定接口号的型号 | `55db`、`55dd`、`55de`、`55e7`、`55eb`、`5610` | 枚举会跳过。 | 未实现，未验证 |
| 其他 `1A86` PID | — | 枚举会跳过，原因是协议未确认。 | 未实现，未验证 |

`--debug` 只在 `1A86` 设备集合或判定变化时打 `WCH enumerate`。一行里有
VID、PID、`bcdDevice`、首个接口类、`decision=list` 或 `decision=skip`，以及
跳过原因。集合没变时不再每秒重复。GUI 串口下拉框和
`kvmux-relay --list-serial` 用同一个 `enumerate_serial_ports()`。被列出的
芯片描述是 `<芯片名> (USB direct)`，CH341 仍是 `CH341 (USB direct)`。

## macOS 截图与录像日志（2026-11-14）

日志是 Darwin 27.2.0 上的 `kvmux`，4139 行，没有 warn 或 error。

读到的事实：

- 采集设备是 KUHAIMI 4K60 Version，模式是 1920x1080 `420v`。
- `WCH enumerate` 有 32 行，不是日志的大部分。重复的两台设备是
  `1a86:7523`（list，CH340，`ch341-usb:32-1.1`）和 `1a86:9354`
  （skip，class `0x03`，PID 未确认）。
- 同一份日志里大量行是 `CH341 direct in/out` 和 CH9329 ACK。控制后来到过
  Ready，也有键盘 ACK。
- `media=snapshot-queued` 只出现一次，8 ms 后变成 `snapshot-saved`。
  后面 10 行里的 `snapshot-saved` 是同一状态被别的字段带出来，不是 10 次截图。
- `recording` 始终是 `idle`，包括输入已经是 Captured 的时候。
- 日志没有截图路径、JPEG 字节数、编码器名或写入帧数。

代码里对得上的事实：

- `snapshot-saved` 只表示 JPEG 临时文件已经 rename 成功。应用不检查文件是否为空，也不解码看是不是黑的。
- 默认目录是 `SDL_GetUserFolder(SDL_FOLDER_DOWNLOADS)`。仓库里的
  `Info.plist` 没有沙盒 entitlement。这次日志已经是 saved，所以这次写入没有被权限拒绝。文件内容仍然没有看到。
- 写这份日志时，菜单里只有一个 `Start recording`，没有选定区域时是灰的，而且没有说明。全屏截图不需要区域。这份日志里的 idle 不能区分“没点录像”和“菜单项是灰的”。当前菜单分成 `Recording: Full screen`（整帧，不需要区域）和 `Recording: Region`。这次日志没有走到 start。
- `420v` 在 AVFoundation 适配器里记成 NV12、limited range。`process_raw` 把这个范围放在 `VideoFrame` 上，不写进 `AVFrame.color_range`。截图和录像的 swscale 不读 `color_range`。
- 录像用 `avcodec_find_encoder(AV_CODEC_ID_H264)`，不是按名字找 `libx264`。这份日志没有走到 start，所以看不出本机 FFmpeg 会返回哪个 H.264 编码器。

没有根据这份日志改像素转换。Linux 上的 `recording` CTest 用 stride 大于宽度的 limited-range NV12 走截图和录像。通过时只说明这条软件路径能写出可打开的 JPEG（亮度不全是同一值）和尺寸正确的 MP4，不能代替 macOS 上的文件。

## macOS 区域录像日志（2026-11-15）

日志文件是 `kvmux_log_20261115.txt`。读到的事实：

- `region confirm: x=316 y=387 width=551 height=261` 之后，`recording start request` 的裁剪是 `551x261+316,387`，源是 1920x1080 NV12 limited。
- 紧接着的结果是 `rejected`，原因是 `recording crop must be inside the source frame with even coordinates and dimensions`。
- 这份日志没有 `scroll start request`，Session 行里当时也没有 scroll 字段。不能从这份日志判断滚动截图有没有被点到。

这次改动在确认区域时把裁剪对齐到偶数。上面这组数应对齐成 `316,386,552x262`。滚动截图的启动、采样、结束和取消现在有 debug 行，Session 行里有 `scroll=`。这份日志本身仍然没有滚动截图的文件或失败原因。

## macOS CH341 与分离 relay 自检步骤

本节是 Mac 上的操作步骤，用来检查 CH341 libusb 后端，以及分离后的 control relay 和 video relay。写本节时这台开发机没有 `1A86:5523`，这些命令也没有在 macOS 上运行过。跑完以后，把文末字段的实际输出补进本节。没有这条记录时，对应硬件项保持未验证。

开始之前确认这四件事：

- Mac 上已安装 `libusb` 和 `pkg-config`。安装命令是 `brew install libusb pkg-config`。不要安装 WCH `CH34xVCPDriver`。macOS 上 `KVMUX_CH341_USB` 默认为 `ON`，缺少 libusb 开发文件时配置会失败。
- CH9329 使用它当前的波特率。出厂值是 9600。芯片被改过波特率时，用改后的值。改法见 [Configure CH9329 baud rate on Linux](ch9329-baud.md)。
- 端口名以 `ch341-usb:` 开头时，后端只接受 1200、2400、4800、9600、14400、19200、38400、57600、115200。其他波特率会在 open 失败，原因是 `CH341 direct 不支持该波特率`。
- Relay 使用未加密、未认证的 UDP。只在可信任的局域网上运行这些探针，不要把端口暴露到公网。

**Caution:** `--keys` 会在 CH9329 所接的电脑上按下并松开左 Shift（HID usage `0xE1`）。`--mouse` 会让该电脑的指针相对向右移动 20，再向左移动 20。两步都要等到 CH9329 确认。目标电脑当时不能接受这些输入时，不要加这两个参数。不加它们时，探针不发送键鼠。

下面的 `build/macos-debug-headless/kvmux-relay` 都从仓库根目录执行。如果构建的是 GUI 预设 `macos-debug`，把路径换成 `build/macos-debug/kvmux-relay`。

自检命令加上 `--debug --log-file kvmux-probe.log`。程序把同一份日志追加写入当前目录的 `kvmux-probe.log`，并写到标准错误。单个文件超过 10 MiB 后轮转，保留 3 个旧文件。请发回这个 `kvmux-probe.log`，以及标准输出里的 `[PASS]` / `[FAIL]` 汇总。CH341 或采集卡在另一台电脑上时，连同那台电脑工作目录里的 `kvmux-probe.log` 一起发回。日志文件打不开时，进程以退出码 2 结束，原因写在标准错误。带 `--debug` 时，文件开头的 info 行记录程序名、版本 `0.1.0`、操作系统，以及参数里的串口名和波特率。仓库没有嵌入提交号。

### 1. 构建

配置并构建 headless 预设。这个预设包含 `kvmux-relay`，不构建 GUI。

```sh
cmake --preset macos-debug-headless
cmake --build --preset macos-debug-headless
```

配置输出包含 `KVMux CH341 direct backend: enabled`。可执行文件是 `build/macos-debug-headless/kvmux-relay`。

### 2. 列出串口

```sh
build/macos-debug-headless/kvmux-relay --list-serial
```

第一行是 `PORT` 和 `DESCRIPTION`，中间是制表符。`1A86:5523` 在位时，有一行的端口名以 `ch341-usb:` 开头，描述是 `CH341 (USB direct)`。名字由 bus 和端口路径组成，例如 `ch341-usb:20-1`。把打印出来的整段名字记为 `PORT`，后面原样代入，保留 `ch341-usb:` 前缀。

没有 `ch341-usb:` 行时，记下完整输出，并停在后面的本地 CH341 步骤。Relay 步骤仍可进行。一个端口都没有时，程序打印 `No serial ports found.`。

### 3. 打开、握手并释放

这一步不发送键鼠。本地 `--serial` 不会打印“无画面，CLI 自行声明新鲜，仅用于自检”。

```sh
build/macos-debug-headless/kvmux-relay --debug --log-file kvmux-probe.log --probe-control --serial "PORT" --baud 9600
```

把 `PORT` 换成上一步的名字。`--baud` 只能和 `--serial` 一起使用。默认超时是 5000 ms。握手更慢时加上 `--timeout-ms`，取值 1 到 600000。

CH9329 应答时，标准输出的形状如下。`0xNN` 和 `N` 是当次运行打印的值。名字不足 9 个字符时，程序用空格补到 9 个字符。

```
[PASS] open     backend=ch341-usb port=PORT baud=9600
[PASS] ready    usb=ready version=0xNN
[SKIP] keys     (pass --keys to send Left Shift)
[SKIP] mouse    (pass --mouse to move the pointer)
[PASS] release  confirmed in N ms
如需测试拔出：重新运行并在 ready 后拔掉设备
```

五行状态都如上，并且进程退出码为 0，这一步才通过。

`ready` 行的 `version=` 是 CH9329 `GET_INFO` 的芯片版本，两位十六进制。它不是 CH341 的版本字节。

`--debug` 把日志级别设为 debug。打开成功时，标准错误和 `kvmux-probe.log` 里都有一行：

```
CH341 direct open: path=PORT baud=9600 version=0xNN
```

这一行的 `version=` 才是 CH341 版本字节。不加 `--debug` 时，级别是 warn，这一行不出现。`--debug` 和 `--log-file` 可以放在参数的任意位置。

如果代入的是系统串口名，而不是 `ch341-usb:` 前缀，open 行写 `backend=system`。

打开失败时，第一行是 `[FAIL] open` 加上当时的错误文本，进程以退出码 1 结束，并打印上面的拔出提示。设备不存在时，文本是 `设备不存在或已拔出`。接口被厂商驱动占用，或当前用户没有权限时，文本是 `设备被系统或厂商驱动（如 macOS 的 WCH CH34xVCPDriver）占用，或当前用户没有权限（Linux 提示 udev 规则）`。

### 4. 左 Shift 和相对鼠标

先确认目标电脑可以接受一次左 Shift，以及一次向右再向左的指针移动。

```sh
build/macos-debug-headless/kvmux-relay --debug --log-file kvmux-probe.log --probe-control --serial "PORT" --baud 9600 --keys --mouse
```

成功时，第 3 步里的两条 `[SKIP]` 换成下面两行，退出码为 0：

```
[PASS] keys     Left Shift acknowledged
[PASS] mouse    relative +20,-20 acknowledged
```

任一步出现 `[FAIL]` 后，程序释放按键和鼠标，断开连接，打印拔出提示，以退出码 1 结束，不再打印后面的 `[PASS]`。

### 5. 拔出

再次运行第 3 步的命令，不要加 `--keys` 或 `--mouse`。标准输出出现 `[PASS] ready` 之后，拔掉这只 CH341。

程序应以退出码 1 结束。失败行包含 `故障`、`state=`、`release_confirmed=`；快照里有错误文本时还包含 `error=`。把这一行原样记下来。设备在运行中消失时，设计上的 `release_confirmed` 是 `false`。记录以打印出来的值为准。

### 6. Control relay

`--probe-control --relay` 使用 control 角色，只和 `--serve --no-capture` 匹配。`--probe-video` 使用 video 角色，只和 `--serve --no-serial` 匹配。同时打开采集和串口的 `--serve` 会拒绝这两种角色。角色不一致时，输出包含 `角色不匹配`，退出码为 1。

在接有 CH9329 的电脑上启动 control relay。那台电脑是这台 Mac 时，把下面的可执行文件换成 `build/macos-debug-headless/kvmux-relay`，参数不变。Linux 上是：

```sh
build/linux-debug-headless/kvmux-relay --debug --log-file kvmux-probe.log --serve --no-capture --serial "/dev/ttyUSB0" --baud 9600
```

串口名和波特率换成那台电脑上的实际值。那台电脑使用 `ch341-usb:` 时，`--serial` 写成该名字。只放行控制端口的入站 UDP，默认端口是 17000。`--no-capture` 不打开采集卡，也不监听视频端口。

在 Mac 上运行：

```sh
build/macos-debug-headless/kvmux-relay --debug --log-file kvmux-probe.log --probe-control --relay RELAY_HOST --control-port 17000 --keys --mouse
```

把 `RELAY_HOST` 换成 control relay 的地址。`--control-port` 只能和 `--relay` 一起使用。对 `--relay` 再写 `--baud` 时，程序打印 `--baud applies only to --serial`，退出码为 2。`--serial` 和 `--relay` 同时给出，或者两个都不给，程序打印 `--probe-control requires exactly one of --serial or --relay`，退出码为 2。

成功时，`ready` 之前有这一句：

```
无画面，CLI 自行声明新鲜，仅用于自检
```

open 行是：

```
[PASS] open     backend=relay host=RELAY_HOST port=17000
```

`ready` 行的 `version=` 为 `0x00`。Relay 状态帧不携带 CH9329 的 `chip_version`。CH9329 版本在接串口的那一侧；那一侧以 `--debug --log-file kvmux-probe.log` 启动时，打开记录写在那台电脑的 `kvmux-probe.log` 里。这条探针在内部解析视频端口 17001，但不打开媒体。`--probe-control` 没有视频端口参数。

不加 `--keys` 和 `--mouse` 时，键和鼠标两行是 `[SKIP]`，程序不发送输入。上面那句新鲜声明仍然会打印。退出码为 0，并且 open、ready、release 都是 `[PASS]`，这一步才通过。

角色不一致时，open 行是 `[FAIL] open     角色不匹配`，退出码为 1。程序不会把 ready 打成 `[PASS]`。

### 7. Video relay

在接有采集卡的电脑上启动 video relay。那台电脑是这台 Mac 时，把下面的可执行文件换成 `build/macos-debug-headless/kvmux-relay`，参数不变。Linux 上是：

```sh
build/linux-debug-headless/kvmux-relay --debug --log-file kvmux-probe.log --serve --no-serial --device "/dev/video0" --mode-index 0
```

设备 ID 和模式索引换成那台电脑上 `--list-devices` 与 `--list-modes` 给出的值。自动选择找到唯一采集设备和可用模式时，可以省略 `--device` 和 `--mode-index`。放行控制端口和视频端口，默认是 17000 和 17001。`--no-serial` 不打开串口。

在 Mac 上运行：

```sh
build/macos-debug-headless/kvmux-relay --debug --log-file kvmux-probe.log --probe-video RELAY_HOST --control-port 17000 --video-port 17001 --seconds 11
```

主机名是第一个不以 `--` 开头的参数。`--seconds` 的范围是 1 到 86400，默认值是 5。这里用 11 秒，让运行跨过 10 秒，以便看到保活期间连接是否中断。

标准输出的第一行是：

```
帧数与最长间隔只是接收计数，不是延迟或性能结论
```

握手完成时打印 `[PASS] handshake`。`handshake` 正好是 9 个字符，后面的内容紧挨着名字，中间没有额外空格：

```
[PASS] handshakecodec=MJPEG size=WxH
```

`MJPEG` 是采集快照里的格式名，实际输出可能是 `H.264` 或 `H.265`。间隔结束时，帧数大于 0 且采集没有进入 fault，进程退出码为 0，并打印：

```
[PASS] video    codec=CODEC size=WxH frames=N longest_gap_ms=M
```

`CODEC`、`W`、`H`、`N`、`M` 都来自当次输出。`frames` 和 `longest_gap_ms` 只是接收计数。采集在间隔中进入 fault 时，程序立刻打印 `[FAIL]`，以退出码 1 结束，不会等满 `--seconds`。

把这条命令指到 `--serve --no-capture` 时，失败行是 `[FAIL] handshake角色不匹配`，退出码为 1，同样不会等满 11 秒。

### 要记下的字段

把下面各项的实际输出补进本节。写本节的 Linux 开发机没有这些结果。

- 日期、macOS 版本、机型。
- `pkg-config --modversion libusb-1.0` 的输出。
- USB 拓扑，以及 WCH `CH34xVCPDriver` 是否已安装。
- `--list-serial` 打印的 `ch341-usb:` 名字。
- 使用的波特率。
- `kvmux-probe.log` 里 `CH341 direct open` 的 `version=`。标准错误是同一份日志。
- `ready` 行的 `version=`。本地串口探针上，这一值是 CH9329 版本。Control relay 探针上，这一值是 `0x00`。
- 每条探针的退出码和完整标准输出。
- 使用 `--keys` 和 `--mouse` 时，目标电脑上是否出现左 Shift，以及指针是否向右再向左移动。
- 拔出时的失败行，包括 `state=`、`release_confirmed=` 和 `error=`。
- 每台 relay 的操作系统、启动命令、主机、端口、角色，以及 Mac 上对应探针的标准输出。
- Video 探针的 `codec`、`size`、`frames` 和 `longest_gap_ms`。这些数字不是延迟或性能结论。

## GUI 帧上传（2026-11-13）

macOS 诊断面板上还没有对比「只在新帧上传」前后的 `sample_to_gpu_submit` 和 `SwapWindow blocking`。这项保持未验证。Linux 上的 FakeCapture 单测只检查 `sample_to_gpu_submit` 的次数是否等于新帧数，不是 KVM 延迟，也不是这块面板的性能记录。

## GUI 结构拆分（2026-11-13）

面板、事件路由和链路状态的拆分没有在 macOS 上打开过 GUI。连接窗口、菜单、粘贴、副屏、状态栏和诊断窗是否和拆分前一样，都还没有人工核对。这项保持未验证。Linux 上的 `make test` 不覆盖这些窗口。

## GUI 连接窗口（2026-11-14）

下面几项没有在 macOS 上打开过界面。Linux 开发机没有 `/dev/video*`，`make test` 也不绘制这些控件。保持未验证。

- 浮动图标是否躲开 Video source 标签，并且仍能点开菜单。
- 串口下拉框是否完整显示 `/dev/cu.*` 这类路径，列表里的描述是否也被截断。
- 重启后采集模式下拉框是否回到上次的分辨率，并在精确匹配时自动开始预览。
- 保存的串口是否出现在下拉框里；未勾选「Connect serial on startup」时是否不会自动连接。
- 首次引导、可关闭错误横幅、置灰按钮的 tooltip、Apply、两条链路都 ready 才关闭窗口、状态栏粘贴进度。

## GUI 字体与 HiDPI（2026-11-14）

下面几项没有在 macOS 上打开过界面。Linux 开发机的窗口缩放是 1，不能代替 Retina。保持未验证。

- Inter 是否替代了默认字体，中文是否仍由系统字体补上。
- Retina 上字是否清晰，控件是否被放大两次。
- 空视频区、错误横幅、状态栏和暗铜按钮是否符合品牌色。
