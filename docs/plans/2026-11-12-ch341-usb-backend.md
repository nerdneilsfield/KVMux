# CH341 libusb 串口后端：实施计划

Plan readiness: ready（第二版，两位审阅者 2026-11-12 APPROVE）。

## 目标与范围

macOS 上 `1A86:5523` 的 CH341 没有可用的系统串口，CH9329 因此连不上。新增一个用 libusb 直接驱动这颗芯片的串口后端，对 `Ch9329ControlSink` 暴露与现在相同的 `SerialIo` 读写接口。CH9329 协议层（`ch9329_protocol.*`、`serial_worker.cpp` 的事务、ACK 与恢复逻辑）不动；`serial_worker` 只改打开失败时的错误文案来源。

- macOS：探测到 `1A86:5523` 时默认走 libusb 后端。
- Linux / Windows：同一后端也编译进来，用户可以显式选择；默认仍是 libserialport 的系统串口。
- 其他 VID:PID 的串口设备行为不变。

芯片控制序列以 Linux 主线 `drivers/usb/serial/ch341.c` 为准，不参考 `ch343ser_linux`。

用户原话先说"只在 macOS 编译"，后说"Windows 和 Linux 用户可以选择"，以后者为准。

不做：其他 WCH 型号（7523、CH343 等）、热插拔自动重连之外的设备追踪、modem 状态读取（中断端点）、硬件流控、非 8N1 格式、CH9329 支持列表以外的波特率。

## 现状

- `SerialIo`（`src/control/serial_worker.hpp:20-25`）：`open(name, baud) -> bool`、`close`、`write(span) -> ptrdiff_t`、`read(span) -> ptrdiff_t`。`write`/`read` 非阻塞：返回本次处理的字节数（可为 0、可部分），负数表示失败。串口线程约每 1 ms 一轮（`serial_worker.cpp:752`），读写见 `:581`、`:629`，失败走 `fail(..., true)`（release 未确认）。
- `io.open` 失败时 worker 固定写入 `"Could not open selected serial port"`（`serial_worker.cpp:559-568`），后端无法传递原因。
- 唯一实现 `make_libserialport_io()`（`src/control/serial_transport.cpp`）：8N1、无流控、打开后 `sp_flush` 两个方向（`:79`）。
- `enumerate_serial_ports()` 返回 `SerialPortInfo{name, description, usb_vendor_id, usb_product_id}`（`control_sink.hpp:48-53`）。GUI 打开连接面板时在主线程每秒调用一次（`main.cpp:1030-1035`）；relay 用于 `--list-serial`（`relay_main.cpp:299`）和自动选口。
- relay 自动选口 `select_serial()` 要求 `1A86` 且 PID 为 `7523/5523/55d3` 的候选恰好一个，多个时报 "Multiple CH340/CH341/CH343 USB serial adapters"（`relay_selection.cpp:50-55`、`:109-121`）。
- CH9329 支持的波特率：1200、2400、4800、9600、14400、19200、38400、57600、115200（`docs/ch9329-baud.md:8`）。CH9329 的 GET_INFO 回复 14 字节，键鼠 ACK 7 字节，GET_CONFIG 回复超过 32 字节（`serial_worker.cpp:292-301`）。
- 构建：libserialport 源码在 `third_party/libserialport`；Windows 运行时 DLL 由 `CMakeLists.txt` 的 `kvmux_runtime_dlls` 拷贝和安装（`:344-383`），所有 Windows 测试和 CI 干净 PATH 启动检查依赖它。编解码后端已有 `AUTO/ON/OFF` 选项先例（`cmake/CodecBackends.cmake:2-9`）。
- 本开发机只有 libusb 运行库，没有 `libusb-1.0-0-dev`。

## 设计

### 选择方式：端口名即后端

不加配置键，也不加后端下拉框。`enumerate_serial_ports()` 在系统串口之外，追加每个通过 libusb 找到的 `1A86:5523` 设备：名字 `ch341-usb:<bus>-<port path>`（如 `ch341-usb:1-2.3`，基于物理拓扑，插回同一口名字不变），描述 `CH341 (USB direct)`，VID/PID 照填。枚举只读设备描述符、bus 号和 port numbers，不调用 `libusb_open`（GUI 主线程每秒都会调用）。

- macOS 上这颗芯片没有系统串口，列表里只有 `ch341-usb:` 条目。
- Linux / Windows 上同一颗芯片可能同时出现系统串口和 `ch341-usb:` 条目，用户选哪个就用哪个。
- 已保存的 `serial_port` 配置值原样生效；relay 的 `--serial ch341-usb:1-2.3` 同理。

relay 自动选口（不给 `--serial`）：

- macOS：`ch341-usb:` 条目算候选（这就是 macOS 默认走 libusb）。
- Linux / Windows：忽略 `ch341-usb:` 条目，只在系统串口里选，行为与现在一致。显式 `--serial ch341-usb:...` 不受影响。

平台判断放在 `relay_selection.cpp` 一处，由一个可在测试中传入的平台参数决定，便于在 Linux 上测试两种规则。

`Ch9329ControlSink` 默认构造使用一个分发 `SerialIo`：`open` 看名字前缀，`ch341-usb:` 交给 libusb 后端，其他交给 libserialport；之后的 read/write/close 转发给本次打开的后端。分发放在 `serial_transport.cpp`。

### 打开失败的错误通道

`SerialIo::open` 改为 `bool(const std::string& name, int baud, std::string& error)`。失败时后端写入面向用户的原因，worker 把它写入 `ControlSnapshot::error`（为空时仍用现有文案）。libserialport 后端填 `sp_last_error_message` 的内容。这是对 `serial_worker` 的唯一改动，测试里的假 `SerialIo` 随签名更新。

### libusb 后端（新增 `src/control/ch341_usb_transport.cpp`）

**打开**

1. 按端口名中的 bus 与 port path 找设备，核对 `1A86:5523`；找不到时报"设备不存在或已拔出"。
2. `libusb_open`。若 `libusb_get_configuration` 不是 1，先 `libusb_set_configuration(1)`：macOS 上没有驱动匹配的厂商类设备可能处于未配置状态，否则认领会得到 `NOT_FOUND`。Linux 上启用 `libusb_set_auto_detach_kernel_driver`（用户显式选了 `ch341-usb:`，就是要绕开内核 `ch341`）。认领接口 0。
3. 认领失败时返回 false 并给出原因，**不回退到 `/dev/cu.*` 或系统串口**：
   - `BUSY` / `ACCESS`：设备被系统或厂商驱动（如 macOS 的 WCH `CH34xVCPDriver`）占用，或当前用户没有权限（Linux 提示 udev 规则）；
   - Windows 上 `NOT_SUPPORTED`：设备没有绑定 WinUSB，需用 Zadig 绑定；
   - 其他错误：带 `libusb_error_name`。
4. 从接口 0 的描述符取 bulk IN / OUT 端点地址，不硬编码。
5. 按主线 `ch341.c` 初始化（`ch341_configure` → `ch341_set_baudrate_lcr` → `ch341_set_flow_control` → `ch341_set_handshake` 的顺序）：
   - 读版本：vendor IN `0x5f`，2 字节，取第一个字节为 `version`；
   - 串口初始化：vendor OUT `0xa1`，`wValue=0`、`wIndex=0`；
   - 波特率：只接受上面 9 个 CH9329 波特率，其他直接返回 false（"CH341 direct 不支持该波特率"）。分频值按主线 `ch341_get_divisor` 计算（48 MHz 时钟取最近值，不要求整除；9600、57600、115200 等都有主线允许的误差）；`version > 0x27` 时置 bit 7（否则 CH341A 会攒满 32 字节才上报，CH9329 的 7/14 字节回复永远等到超时）；写寄存器 `0x9a`，`wValue=0x1312`，`wIndex=` 该值；
   - LCR：`version >= 0x30` 时写 `0x9a`，`wValue=0x2518`，`wIndex=0x00c3`（RX、TX 使能 + CS8，无校验、1 停止位）；`version < 0x30` 时按主线跳过；
   - 流控：写 `0x9a`，`wValue=0x2727`，`wIndex=0`，关闭 RTS/CTS；
   - modem 控制：`0xa4`，`wValue = ~(DTR|RTS)`（主线低有效，`DTR=1<<5`、`RTS=1<<6`），使 DTR/RTS 有效。
   任一控制传输失败：释放并返回 false，错误带步骤名。
6. 丢弃残留：以 1–2 ms 超时同步读 bulk IN 直到超时，丢弃数据（对应 libserialport 后端的 `sp_flush`，防止重连后读到上一轮迟到的 ACK）；清空内部缓冲；然后提交常驻 IN 传输。

**读（非阻塞）**

- 一个 IN 传输，缓冲为端点最大包长（32 字节）。内部有同样容量的已完成数据缓冲。
- `read`：先以零超时调用 `libusb_handle_events_timeout_completed`，再从已完成缓冲拷给调用者。**只有已完成缓冲被取空、且未在关闭中时才重新提交 IN 传输**；未取空时 IN 端点不被读取，数据留在芯片 FIFO（这是天然背压，不丢字节）。GET_CONFIG 这种超过 32 字节的回复会分两次传输读出，worker 的解析器本来就按流处理。
- 回调中只记录长度和状态，不重提、不拷贝到别处。传输状态为 `NO_DEVICE`、`ERROR`、`STALL`、`OVERFLOW` 时标记故障，下一次 `read`/`write` 返回 -1。`TIMED_OUT` 不会出现（IN 传输不设超时）。

**写（非阻塞）**

- 一个 OUT 传输，缓冲 32 字节，同时最多一个在途。
- `write(span)`：先以零超时处理事件；有故障返回 -1；有在途 OUT 返回 0；否则拷入至多 32 字节、提交，返回拷入的字节数。返回值的含义是"后端已接收并提交"，与系统串口驱动接收进内核缓冲的语义相同，worker 推进 `written` 后不会重发这些字节。
- OUT 传输超时 100 ms。完成时 `actual_length` 小于提交长度、状态非 `COMPLETED`，都标记故障，下一次读写返回 -1，交给 worker 的 `fail(..., true)`。
- 读写都不在串口线程上阻塞，ReleaseAll、心跳和关闭请求的检查节奏不变。

**关闭**

- 置 `closing`，阻止任何重提。对每个在途传输调用 `libusb_cancel_transfer`，然后循环 `libusb_handle_events_timeout_completed`（每次 10 ms）直到所有回调都已执行。**在回调执行之前绝不释放 transfer、缓冲或后端对象，也不关闭句柄**。设备已拔出时 libusb 会以 `NO_DEVICE` 或 `CANCELLED` 完成回调，因此循环会结束；等待超过 1 秒时打一条警告日志，但仍继续等。
- 之后释放接口、`libusb_close`、销毁 context。context 每个后端实例一个；全部 libusb 调用都在串口线程上，不新建线程。枚举用一个短生命周期的独立 context，在调用线程内创建和销毁。

### 构建

- CMake 选项 `KVMUX_CH341_USB`，取值 `AUTO/ON/OFF`，沿用 `CodecBackends.cmake` 的写法：macOS 默认 `ON`（找不到 libusb 配置失败并说明），Linux / Windows 默认 `AUTO`（找到就编进去，找不到就不编，配置输出里说明）。`OFF` 或未找到时不编译后端，枚举不出现 `ch341-usb:` 条目，打开 `ch341-usb:` 名字时返回"未编译 CH341 direct 支持"。
- 发现：`pkg_check_modules(libusb-1.0)`；Windows 用 `LIBUSB_ROOT` 指向官方预编译包。不自动下载、不 vendor（libusb 有 darwin/usbfs/winusb 平台后端，抽出来维护成本高；按 AGENTS 对大运行时库的要求文档化）。
- Windows：`libusb-1.0.dll` 与许可证文件在 T1 同一提交里加入 `kvmux_runtime_dlls` 和 `install`，否则所有 Windows 测试与 CI 启动检查都会因缺 DLL 失败。
- 分频计算、波特率白名单与端口名解析写成不依赖 `<libusb.h>` 的纯函数，测试无条件编译。

### 测试

USB 交互不造 libusb 假实现。离线测试只有一个小文件 `tests/ch341_direct_test.cpp`：

- 9 个波特率各自的分频寄存器值，期望值由主线 `ch341_get_divisor` 算法逐个手算后写成常量（不从实现里反推），并分别检查 `version=0x27` 与 `0x30` 时 bit 7 的有无；白名单外的波特率被拒绝。
- 端口名解析：合法 `ch341-usb:1-2.3`、非法格式。

`relay_selection_test` 增加：Linux/Windows 规则下系统串口 + `ch341-usb:` 同芯片时选系统串口；macOS 规则下只有 `ch341-usb:` 时选中它。

硬件验证按 `docs/hardware-validation.md` 的格式记录 OS、机型、USB 拓扑、波特率、CH341 版本字节、CH9329 配置；没有实测前标为未验证。

## 验收

| ID | 行为 | 任务 | 检查与预期 |
| --- | --- | --- | --- |
| B1 | 构建开关 | T1 | 装有 libusb 开发包时 `make` 通过并链接 libusb；`-DKVMUX_CH341_USB=OFF` 时不链接，构建与 `make test` 通过；macOS 上 `ON` 而找不到 libusb 时配置失败并说明 |
| B2 | 分频、白名单与端口名 | T1 | CTest `ch341_direct` 通过，期望值来自主线算法手算 |
| B3 | 枚举与自动选口 | T1 | CTest `relay_selection` 覆盖两种平台规则；插着 `1A86:5523` 时 `kvmux-relay --list-serial` 与 GUI 端口列表出现 `ch341-usb:` 条目，其他条目不变（手动） |
| B4 | 打开失败给出原因 | T1 | Linux 上去掉设备节点权限后选择 `ch341-usb:` 条目，GUI / relay 状态显示权限原因而非通用文案；没有打开任何 `/dev/tty*`（手动） |
| B5 | CH9329 可用 | T1 | Linux 上选 `ch341-usb:` 条目、9600 波特连接 CH9329：握手进入 ready，按键与鼠标到达被控机；断开再连接仍能 ready；拔出设备后状态变为故障且 release 未确认，进程不崩溃（手动，硬件） |
| B6 | macOS 默认路径 | T1 | macOS 上同 B5 流程，且不带 `--serial` 的 relay 自动选中 `ch341-usb:` 条目（硬件；无设备时标未验证） |
| B7 | 文档与许可证 | T2 | `docs/building.md`、第三方声明包含 libusb 版本、安装方式、LGPL-2.1 义务；各平台使用说明（Linux 内核驱动会被分离和 udev 权限、Windows 需 WinUSB、macOS 不能装 WCH 厂商驱动） |

## 执行

排在分离 relay 计划 T4 之后开始。开始前需要本机安装 `libusb-1.0-0-dev`，否则只能验证 `OFF` 路径。

### T1：libusb 后端、错误通道与自动选口

Status: done
Depends on: 分离 relay 计划 T4 完成
Acceptance: B1–B6
Targets: `src/control/ch341_usb_transport.cpp`（新增）、`src/control/serial_transport.cpp`（分发与枚举）、`src/control/serial_worker.{hpp,cpp}`（`open` 错误通道）、`src/network/relay_selection.*`、`CMakeLists.txt`（选项、发现、Windows DLL 与安装）、`tests/ch341_direct_test.cpp`（新增）、`tests/relay_selection_test.cpp`、使用假 `SerialIo` 的现有测试（签名更新）

- [x] 按上文实现；`make`、`make test`、`make lint`（含 `KVMUX_CH341_USB=OFF` 一次构建与测试）。
- [x] B3 的 CTest 覆盖两种选口规则。B3 的手动枚举、B4、B5 未在本机做硬件验证：用户确认没有 `1A86:5523`。Evidence 标为未验证（无硬件），待用户在 macOS 上测试。B6 待 macOS 验证。
- [x] 提交 `feat(control): add CH341 libusb serial backend`。

Evidence:

- 父提交 `87b6653`。未设置 `HEADLESS`。`CMAKE_PREFIX_PATH=/tmp/ffmpeg-dev/prefix/usr:/tmp/x11-dev/prefix/usr`，`CPATH=/tmp/x11-dev/prefix/usr/include`。`make test` 配置行 `-- KVMux CH341 direct backend: enabled`。27 项中 26 项通过，real 63.25 秒。`ch341_direct`、`relay_selection`、`serial_worker` 通过。失败项只有 `relay_client`。
- `relay_client` 仍是基线失败。`chunked_ascii_paste_loopback` 通过；`paste_keepalive_blackholed_status` 失败在 `tests/relay_integration_fakes.hpp:216` 的 `GuiFixture::activate()`。
- `ldd build/linux-debug/kvmux-relay` 链接 `libusb-1.0.so.0`。本机 `pkg-config --modversion libusb-1.0` 为 1.0.27。计划里「本开发机没有 libusb 开发包」已过时。
- `KVMUX_CH341_USB=OFF` 的目录 `build/linux-debug-ch341-off`：配置行 `-- KVMux CH341 direct backend: disabled`。当时完整 GUI `ctest` 同样是 26/27，唯一失败是同一个 `relay_client`。`kvmux-relay` 的 `ldd` 没有 libusb，ninja 不编译 `ch341_usb_transport.cpp`。移位改成无符号之后，该目录增量重编了 `ch341_usb.cpp`，`ch341_direct` 通过；传输对象仍不在该构建里。
- `make format-check` 退出码 2，唯一违规是基线 `tests/codec_startup_fallback_test.cpp:21`。因此 `make lint` 到不了 cppcheck 和 clang-tidy。按 Makefile 配方、用 `build/linux-debug/compile_commands.json` 单独跑了这两步。本次 C++ 文件 `clang-format --dry-run --Werror` 通过。
- cppcheck 退出码 1，唯一告警是既有的 `src/network/relay_server.cpp:566` `identicalConditionAfterEarlyExit`。
- clang-tidy 退出码 0。本次文件里新出现的 `bugprone-signed-bitwise` 和 `bugprone-unchecked-optional-access` 已改掉并复跑确认消失。`tests/ch341_direct_test.cpp` 的 `main` 仍有 `bugprone-exception-escape`，与现有会抛异常的测试相同，保留。`serial_transport.cpp` 的 `close` 也有这条告警；对 `87b6653` 的同一函数（当时在第 84 行）复跑同样报出，不是这次引入的。`control_sink.hpp` 的 `performance-unnecessary-value-param` 是既有告警。
- `lsusb` 没有 `1a86` / `ch34`。B3 手动枚举、B4、B5：未验证（无硬件），待用户在 macOS 上测试。B6：待 macOS 验证。没有加 libusb 假实现。
- Windows 的 `libusb-1.0.dll` 和许可证文件只在 `CMakeLists.txt` 里接到 `kvmux_runtime_dlls` 与 `install`。本机是 Linux，这条路径没有实际拷贝过 DLL。
- 偏离：端口名解析失败的文案是「CH341 direct 端口名无效」（计划只写了设备不存在时的文案）。bulk 包长超过 512 字节时拒绝打开。libusb 以 PUBLIC 链到 `kvmux_core`，静态库才能把 `-lusb-1.0` 传给 `kvmux-relay`。`relay_selection_test` 多断言了 `system_driver` 下显式 `ch341-usb:` 原样返回。版本字节目前以 `spdlog::debug` 记录。

### T2：文档与 CI

Status: pending
Depends on: T1
Acceptance: B7
Targets: `docs/building.md`、`docs/hardware-validation.md`、`third_party/INVENTORY.md` 或第三方声明、`.github/workflows/ci.yml`

- [ ] 写明支持版本、安装方式、许可证义务、三个平台的使用前提。
- [ ] CI 三个平台安装 libusb 开发包（Windows 提供 `LIBUSB_ROOT`）。
- [ ] 提交 `docs(control): document CH341 libusb backend`。

## 进度

T1 完成。下一步 T2。
