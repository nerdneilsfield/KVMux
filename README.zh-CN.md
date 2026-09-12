[English](README.md) | [简体中文](README.zh-CN.md)

![KVMux](assets/branding/banner.png)

# KVMux

通过 USB 采集卡查看另一台电脑的画面，并用 CH9329 控制它的键盘和鼠标。
**Local** 模式直接使用本机连接的硬件；**Remote** 模式通过可信局域网连接硬件所在的中继电脑。
被控电脑无需安装 KVMux。

## 准备硬件

- USB 采集卡，连接被控电脑的视频输出。
- CH9329 控制线或开发板。串口端连接运行 KVMux 或中继程序的电脑，USB HID 端连接被控电脑。
- 运行桌面 GUI 的电脑。Remote 模式还需要一台连接硬件的中继电脑。

```text
被控电脑视频输出 ──> USB 采集卡 ──> KVMux 电脑
被控电脑 USB HID <── CH9329     <── 串口适配器

Local： KVMux 电脑运行桌面 GUI
Remote：KVMux 电脑运行中继程序 <── 局域网 ──> 桌面 GUI
```

KVMux 使用外接硬件，不是软件屏幕共享工具。可用的采集模式取决于采集卡。
识别采集设备和串口适配器的方法见[硬件指南](docs/hardware-validation.md)。

## 获取 KVMux

按[构建指南](docs/building.md)从源码构建。仓库的 [CI](.github/workflows/ci.yml)
会构建和测试源码，但不发布安装包下载。构建需要 C++20 编译器、CMake、Ninja 和 FFmpeg 开发库。
Windows 构建需要在 MSVC 开发者命令行中运行。

macOS 用户安装 C++20 编译器和 CMake 后，可用 Homebrew 安装 Ninja 和 FFmpeg，
再从仓库根目录构建 Release 版本：

```sh
brew install ninja ffmpeg
cmake --preset macos-release
cmake --build --preset macos-release
open build/macos-release/kvmux.app
```

构建指南也列出了 Linux、Windows、Release 和无界面中继的预设。
macOS Developer ID 签名和公证尚未完成。Windows/Linux 硬件覆盖、持续性能和长时间运行稳定性仍未完成验证；
选择硬件前可查看[验证记录](docs/acceptance.md)。

## 使用本地硬件

1. 将采集卡和 CH9329 串口适配器接到运行 GUI 的电脑。
   macOS 本地采集需要允许摄像头访问；Linux 用户需要有权打开视频设备和串口。
2. 打开 **Connections**，选择 **Local**。设置 **Capture device**（采集设备）和
   **Capture mode**（采集模式），然后选择 **Start preview**。
3. 设置 **Serial port**（串口）和 **Baud rate**（波特率），然后选择 **Connect**。
   默认波特率为 **9600**；如果 CH9329 已改过配置，请使用它的实际波特率。
4. 等待实时画面和控制连接就绪。两者就绪后，连接弹窗会自动关闭。
   单击视频区域即可接管键盘和鼠标；这次激活单击不会发送给被控电脑。

## 通过局域网连接

**仅在可信局域网中使用。** Remote 没有身份认证或加密。
任何能访问中继端口的人都可能尝试控制被控电脑，不要将这些端口暴露到互联网。

1. 在连接硬件的 Windows 或 Linux 电脑上，按[局域网快速入门](docs/lan-relay.md)构建并启动中继。
   对于已构建的 Linux Debug 无界面版本，如果只有一张采集卡和一个可识别的串口适配器，可运行：

   ```sh
   build/linux-debug-headless/kvmux-relay --serve
   ```

   如果自动选择失败，按指南列出设备，再明确指定采集模式和串口。
2. 在中继电脑的防火墙中允许 UDP **17000**（控制）和 **17001**（视频）入站，
   并将访问范围限制在预期的局域网内。
3. 在 GUI 中打开 **Connections**，选择 **Remote**，在 **IPv4 host** 中填写中继电脑的局域网地址。
   如果没有修改中继端口，保持 **Control port** 为 `17000`、**Video port** 为 `17001`。
4. 选择 **Connect relay**。等待实时画面和控制连接就绪，再单击视频区域接管输入。
   目前只支持一个控制端。

网络视频默认使用 MJPEG。可选的 H.265 编码需要原始视频采集，不支持将 MJPEG 转码为 H.265。
编码器要求和 GUI 的 **Decode** 设置见 [H.265 配置](docs/lan-relay.md#choose-h265-encoding)。

## 释放输入与打开菜单

按 **Host 键**可退出接管，返回 Preview。Host 键不会发送给被控电脑。
macOS 默认使用 **右 Command**，Windows/Linux 默认使用 **右 Control**。
也可以在 **Connections** 中更改：macOS 可选 **Host: Right Control**，
Windows/Linux 可选 **Host: Right GUI**。

输入接管或恢复期间，顶部菜单会隐藏。单击半透明的 KVMux 标志悬浮按钮可释放输入并打开菜单，
拖动按钮可调整位置，拖动操作不会发送给被控电脑。在 **Relative** 鼠标模式下，需要先按 Host 键解锁指针。
全屏时，释放输入后将指针移到顶部边缘，也可显示顶部菜单。
**Status overlay** 可切换底部状态显示，**Diagnostics** 可查看连接详情。
悬浮按钮位置和状态显示开关不会保存。

系统保留的快捷键可能仍由本机处理。需要时，可使用 **Connections** 中的
**Send Ctrl+Alt+Del** 或 **Send Alt+Tab**。如果串口断开后被控电脑仍有按键或鼠标按钮处于按下状态，
软件无法确认释放是否送达；必要时重新连接被控电脑的 USB HID 线。
视频、输入或串口故障的排查方法见[局域网故障排查](docs/lan-relay.md#troubleshooting-video-appears-but-input-does-not-work)。

## 更多文档

- [构建选项与依赖](docs/building.md)
- [局域网配置与故障排查](docs/lan-relay.md)
- [硬件识别](docs/hardware-validation.md)与[验证状态](docs/acceptance.md)
- 面向贡献者的[技术设计](docs/design/kvm-technical-design-v1.md)
