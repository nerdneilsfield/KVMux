# 视频与键鼠分离到不同设备：实施计划

Plan readiness: ready（proof 方案已由用户在 2026-11-12 确认）。

## 目标与范围

让采集卡和 CH9329 可以挂在不同机器上，GUI 分别选择视频来源和键鼠去向：

| 场景 | 视频来源 | 键鼠去向 |
| --- | --- | --- |
| 1 | 本机采集卡 | 只做键鼠的 relay |
| 2 | 只出视频的 relay A | 只做键鼠的 relay B |
| 3 | 只出视频的 relay | 本机 CH9329 |

现有的单 relay 模式（同一台 relay 同时出视频、收键鼠）行为和 proof 规则保持不变。

动机：macOS 上 CH9329 串口适配器支持差，需要把键鼠放到别的机器上，而视频可能更适合在本机或另一台性能更好的机器上处理。

不做：relay 之间的通信、认证或加密（现有 relay 本就只限可信局域网，见 `docs/lan-relay.md`）、一台 relay 同时服务多个 GUI、自动发现。

现有改动：无（开始时工作区干净，HEAD `0a04092`）。

## 现状

- `KvmSession` 已分别持有 `unique_ptr<CaptureSource>` 和 `unique_ptr<ControlSink>`（`src/app/kvm_session.hpp:57-58`），stale video 判定只看 capture（`kvm_session.cpp:408-420`），release 走 `request_release_locked`。
- GUI 只有"本地 / remote"布尔值和 `capture_only`。remote 时一个 `RelayClient` 同时包成 `NetworkCaptureSource` 和 `NetworkControlSink`（`src/app/main.cpp:1482-1514`），共用 start/stop。
- `kvmux-relay --serve` 无条件打开采集与串口（`src/app/relay_main.cpp:187-191`）；`RelayServer` 构造需要两者，`start` 校验 capture mode（`relay_server.cpp:726-737`）。
- 控制授权依赖 raw `Proof`：服务端要求 `presented_sequence` 等于自己正在/刚发送的帧（`relay_server.cpp:395-420`），粘贴授权同样需要当前 proof。它保证"操作者正看着这台 relay 发出的画面"。
- 握手 `Hello` 只有 `codecs`，`Busy` 只有 `busy / no_common_codec`（`src/network/relay_wire.hpp:36-49`）。

## 设计

### 角色

新增 `RelayRole { both, video, control }`。

- 服务端角色由启动参数决定：默认 `both`；`--no-serial` → `video`；`--no-capture` → `control`；两者同时给出是参数错误，进程退出非零。
- 客户端在 `Hello` 中声明请求角色，**必须与服务端角色完全相同**，否则回 `Busy{reason = role_unsupported}`。`both` 服务端不接受 video/control 子角色请求：三个场景都不需要，同一台机器两样都有时 GUI 直接用 `both`。这样会话角色恒等于服务端角色，服务端不需要再区分"能力角色"和"协商角色"。
- `ServerSession` 处理 `Hello` 时先校验角色，再校验 codec；control 角色不做 codec 协商。客户端为 `role_unsupported` 显示单独的错误文案（现有代码把非 `no_common_codec` 都显示成 "Relay already controlled"，`relay_client.cpp:401-405`）。
- 握手、tuple、KCP 会话、challenge 与断线检测对所有角色相同，只裁剪角色之外的功能。

这是线协议变化。项目未发布、两端同仓构建，不做旧版本兼容；协议版本号若存在则递增，两端必须同版本。

### 各角色行为

**video**：只发媒体；对 `Sync`、输入批、所有 `Paste*` 请求一律拒绝，不打开串口。`check_sync`/`check_edge` 会因无串口 snapshot 自然拒绝，但 `ServerSession::paste_begin` 不检查 ready（`relay_session.cpp:395-439`），需按角色显式拒绝，不分配上传缓冲。会话保活沿用现有 challenge/proof 心跳：握手后 10 秒期限靠有效 proof 续期（`relay_session.cpp:192`、`:329`），因此 video 客户端照常回 proof，`active=false`，只用于保活，服务端不以它决定媒体发送。

**control**：`RelayServer::start` 跳过所有视频步骤：不读 capture mode、不算 `nominal_interval`（帧率分母为 0 会除零）、不绑定视频端口、不建 pacer、不起 `media_worker`（`relay_server.cpp:726-760`）。`stop` 与 `relay_main` 的诊断输出、`capture->stop()`、`sink.disconnect()` 对缺席的一侧判空。控制授权的 proof 规则改为：

- 仍要求 `challenge` 在现有时间窗内、`intent` 匹配、`active && video_fresh`；
- 不把 `presented_sequence` 与发送历史对照（服务端没有帧）。

即 proof 退化为"客户端声明画面新鲜 + 链路活着"。粘贴授权沿用同一判定。普通输入的 lease 时长、过期即 `revoke`（`set_control_active(false)`、`release_all`）不变。

粘贴：现有实现在 preparing/executing 期间豁免 proof lease，只靠 `PasteKeepalive` 续租（`relay_session.cpp:173`、`:225`、`:565`）。control 角色缩小这一豁免：**收到 `video_fresh=false` 的 proof 时，preparing/executing 的粘贴立即取消并 revoke**；proof 只是短暂缺席时，仍按 `PasteKeepalive` 租约处理，避免把网络抖动当成画面失效。both 角色保持原行为。

**both**：完全保持现状，包括 `presented_sequence` 校验。

### 画面门槛移到 GUI

分离模式下"看着画面才能控制"由 GUI 端保证：

- `KvmSession::tick` 已在 capture 变 stale、generation 或 mode 变化时请求 release；这一路径对本地采集和网络视频都成立，无需新机制。
- 控制客户端的 `video_fresh` 取自 GUI 当前所用的视频源（本地 `CaptureSource` 或视频 relay 客户端）的展示进度，由现有 `ControlSink::video_presented` 通道传入。
- GUI 每轮都会对当前帧重复调用 `video_presented`（`main.cpp:937`、`:950`），所以 control 角色的 sink **必须保留本地帧身份去重**：(generation, sequence) 与上次记录的值**不相等**即算新展示（不按单调水位比较，否则外部视频源 generation 变化、sequence 从 1 重来时会被当成旧帧），新鲜度按最近一次新展示的时间失效。
- proof 中 `presented_sequence` 填本地展示序号（非零）。这样线协议 `encode_raw`/`decode_raw` 的 `!video_fresh || presented_sequence != 0` 约束（`relay_wire.cpp:263-265`、`:318-320`）和 `ClientSession::set_intent` 的 `sequence != 0` 约束（`relay_session.cpp:895`）都不用改；control 服务端只是不拿它对照历史。
- 风险评估：relay 信任客户端的声明。由于 relay 无认证，能伪造声明的攻击者本来就能直接控制，安全边界不变。

### 客户端拆分

- `ClientOptions` 增加 `role`。场景 2 构造两个独立 `RelayClient`（各自主机与端口、各自 start/stop/统计）。
- control 客户端不创建 `MediaReceiver`、不起 `decoder_worker`、丢弃 media 数据报（现在 `established` 时无条件创建，`relay_client.cpp:383-396`）。
- 启动：现有单客户端靠 `capture_loop` 里 `capture_->start()` 顺带启动（`kvm_session.cpp:541`）。分离后 control 客户端由 GUI 组装时显式 `start()`；`KvmSession::connect_control` 对空端口的拒绝（`kvm_session.cpp:147`）不经过这条路。
- `NetworkCaptureSource` 只能绑定 role 为 `video` 或 `both` 的客户端；`NetworkControlSink` 只能绑定 `control` 或 `both`。错配在构造时以前置条件失败（程序错误），不是运行时分支。
- control 链路断开或 lease 丢失时，snapshot 不得报告 `release_confirmed`（AGENTS 不变量）。现有 `control_snapshot` 在 challenge 超过 250ms 未刷新时只把状态降为 `stalled`，保留 `release_confirmed`，直到 10 秒会话期限才失败（`relay_client.cpp:1038-1049`）。修正为：challenge 超时或 stopped 时清零 `release_confirmed`；仅画面不新鲜而链路正常时保留，避免 `InputRouter` 卡在 releasing（`input_router.cpp:454`）。这同样修正 both 模式的现有缺口。video 链路断开时，GUI 的 stale 判定触发公共 release，release 由控制 sink（本地串口或 control relay）执行。
- `recoverable_transport` 语义沿用：control relay 的 sink 为 true（stale 时 suspend 而非本地 release_all），本地串口 sink 保持现状。

### GUI

`main.cpp` 中"本地 / remote + capture_only"替换为两个选择：

- 视频来源：本地设备 / relay（主机、视频端口、控制端口——视频 relay 也需要控制端口完成握手）
- 键鼠去向：本地串口 / relay（主机、控制端口）/ 无（等价于现有 capture-only，调用 `set_capture_only(true)`）

两项都选 relay 且主机端口相同时，使用一个 `both` 客户端（即现有行为）；否则按角色分别建客户端。`KvmSession` 要求 control 非空（`kvm_session.cpp:29`），键鼠去向为本地串口或"无"时传入 `Ch9329ControlSink`，"无"再调 `set_capture_only(true)`。

流量与诊断按客户端拆开：现有采样要求单个 `remote_client` 同时满足视频 streaming 与控制已连接，状态栏要求 V/C 速率同时存在（`main.cpp:985-1016`、`:1833-1838`），改为两个客户端各自维护基线、各自显示 `V:` 和 `C:`。串口扫描与串口耗时诊断（`:899`、`:2005`）只在键鼠去向为本地串口时显示；解码器与媒体诊断（`:980`、`:1932`）只在视频来源为 relay 时显示。切换任一选项走现有的 release + 重建 `KvmSession` 流程（`main.cpp:1447` 附近）。不新增配置文件键；是否持久化沿用现有 UI 状态的做法。

## 验收

| ID | 行为 | 任务 | 检查与预期 |
| --- | --- | --- | --- |
| A1 | video 角色服务端只发媒体，拒绝控制，会话长时间保活 | T1/T2 | CTest：FakeCapture 启动 video relay，客户端收到帧且持续超过 10 秒不断开；发送 Sync 被拒，FakeSerial 无字节 |
| A2 | control 角色服务端只执行键鼠 | T1 | CTest：control relay + FakeSerial，客户端声明新鲜后 Sync 与按键到达 FakeSerial；全程无媒体数据报 |
| A3 | 角色不匹配握手失败 | T1/T2 | CTest：向 video relay 请求 control、向 both relay 请求 video，均得到 `role_unsupported`，会话未建立，客户端错误文案为角色不匹配 |
| A4 | `both` 模式行为不变 | T1/T2 | 现有 relay 测试全部通过，`presented_sequence` 校验仍生效 |
| A5 | control relay 在 proof 声明不新鲜或过期时 revoke | T1 | CTest：(a) 按住键后停止新鲜声明，超过 lease 后 FakeSerial 收到全释放；(b) `relay_session_test`：preparing 状态收到 `video_fresh=false`，粘贴取消并产出 `revoke_input`（`RelayServer` 内 preparing→executing 是同一轮同步完成的，集成层无法注入）；(c) relay 集成测试：executing 期间发送 `video_fresh=false`（PasteKeepalive 照常），粘贴取消且 FakeSerial 收到全释放；(d) both 角色粘贴豁免行为不变 |
| A6 | 视频源 stale 时经 control relay 释放 | T2 | CTest：本地 FakeCapture + control relay sink 组成 `KvmSession`，按下按键后 (a) 采集停止，(b) 采集继续但重复上报同一 (generation, sequence)，两种情况都在 lease 后 FakeSerial 收到全释放；(c) 视频源 generation 加一、sequence 从 1 重来，控制保持可用不中断 |
| A7 | control 链路丢失不报告已确认 release | T2 | CTest：release 已确认后停掉 control relay，challenge 超时（>250ms、远小于 10s）后 snapshot `release_confirmed == false`；链路正常仅画面不新鲜时仍为 true |
| A8 | `kvmux-relay` 参数 | T1 | 参数解析在枚举设备前完成：任意机器上运行 `kvmux-relay --serve --no-capture --no-serial` 退出码非零并打印错误；`--no-serial` 在有采集卡的机器上启动时不打开串口（手动） |
| A9 | GUI 三种组合可组装运行 | T3 | `make` + `make lint` 通过；手动在 GUI 选择三种组合，连接 fake/本地设备，状态栏显示视频与控制各自状态 |

硬件实测（真实采集卡、CH9329、跨机器）不在本计划内验收；完成后在 `docs/hardware-validation.md` 按项目要求记录或标为未验证。

## 执行

按 counterweight `references/deep.md` 的执行循环：按依赖顺序逐个任务实现、`make lint` 与受影响测试、检查 diff、本地提交、回填本文件进度。

### T1：relay 服务端角色

Status: done
Depends on: none
Acceptance: A1 A2 A3 A4 A5 A8
Targets: `src/network/relay_wire.*`（Hello 角色、Busy 原因）、`src/network/relay_session.*`、`src/network/relay_server.*`（可选 capture/sink、按角色裁剪、control 角色 proof 规则）、`src/app/relay_main.cpp`（参数）、`tests/relay_*_test.cpp`

- [x] 线协议加入角色与 `role_unsupported`，编解码测试覆盖新字段；`ServerSession` 先校验角色再校验 codec。
- [x] `RelayServer` 以可空的 capture / sink 构造，角色由二者推出；control 角色跳过 `start` 中全部视频步骤，`stop` 对缺席一侧判空。
- [x] video 角色显式拒绝 `Paste*`。
- [x] control 角色 proof 判定按上文，含 `video_fresh=false` 取消 preparing/executing 粘贴，取消时必定产出 `revoke_input`（不能只 `cancel_ascii_paste`，`relay_session.cpp:134-136`）；both 不变。
- [x] `relay_main` 增加无值标志 `--no-capture`、`--no-serial`（现在的解析循环每次跳两个参数，`relay_main.cpp:68`，需改为按参数名步进），解析与互斥检查在设备枚举前完成；状态输出与清理对缺席一侧判空。
- [x] `make lint`、`make test`，A1–A5、A8 通过。
- [x] 提交 `feat(relay): support video-only and control-only relay roles`。

Evidence:

- `make`（无目标）只打印帮助，退出码 0。
- `HEADLESS=1 CMAKE_PREFIX_PATH=/tmp/ffmpeg-dev/prefix/usr make test`：23 项中 22 项通过，约 46 秒。失败项是 `relay_client`。`relay_wire`、`relay_session`、`relay_role`（12.66 秒，含 11 秒保活）、`relay_server`、`relay_recovery` 通过。
- `relay_client` 与 `format-check` 的失败复现于干净的 `76eecbf`：`paste_keepalive_blackholed_status` 的 `GuiFixture::activate()` 连续失败（控制状态 `reconnecting`）；`chunked_ascii_paste_loopback` 偶发键盘报告少 1；`tests/codec_startup_fallback_test.cpp:21` 不符合本机 clang-format 23。`cppcheck` 未安装，`make lint` 因此不能跑完。`HEADLESS=1 make clang-tidy` 退出码 0。T1 改动过的 C/C++ 文件 `clang-format --dry-run --Werror` 通过。
- 使用 `HEADLESS=1`：GUI 预设配置 SDL3 时缺少 XINPUT（`libxi-dev`），本机不能安装。`CMAKE_PREFIX_PATH=/tmp/ffmpeg-dev/prefix/usr`：系统没有 FFmpeg 开发包；该前缀里的静态库已移开，链接的是系统共享库。
- 验收测试：A1 `video_relay_keeps_session_and_rejects_control`；A2 `control_relay_delivers_keys_without_media`；A3 服务端 `role_checked_before_busy_and_codec`、`mismatched_role_is_rejected` 与 wire 的 role/`role_unsupported`；A4 除基线 `relay_client` 外的 relay 测试通过；A5a `control_lease_expiry_releases_keyboard`、`control_proof_lease_revokes_when_freshness_stops`；A5b `control_stale_proof_revokes_preparing_paste`；A5c `control_stale_picture_cancels_paste_and_releases`；A5d `both_stale_proof_keeps_paste_exemption`；A8 `combined_flags_exit_before_device_enumeration`。

### T2：客户端按角色拆分

Status: done
Depends on: T1
Acceptance: A1 A4 A6 A7
Targets: `src/network/relay_client.*`、`tests/relay_client_test.cpp`、`tests/kvm_session_input_test.cpp` 或新的 relay 集成测试

- [x] `ClientOptions.role`；video 角色客户端不发 Sync/输入，照常回 inactive proof 保活；control 角色客户端不建 receiver/decoder、丢弃 media，`video_fresh` 来自 `video_presented` 的本地不等即新去重，proof 填本地展示序号。
- [x] `control_snapshot`：challenge 超时或 stopped 时清零 `release_confirmed`（所有角色）。
- [x] `role_unsupported` 的客户端错误文案。
- [x] 包装器与角色错配为前置条件失败。
- [x] `make lint`、`make test`，A6、A7 通过，A4 回归通过。
- [x] 提交 `feat(relay): split relay client into video and control roles`。

Evidence:

- `HEADLESS=1 CMAKE_PREFIX_PATH=/tmp/ffmpeg-dev/prefix/usr cmake --build --preset linux-debug-headless` 后，在 `build/linux-debug-headless` 跑 `ctest --output-on-failure`：24 项中 23 项通过，约 63 秒。失败项是 `relay_client`。`relay_client_role` 通过（16.25 秒）。`relay_wire`、`relay_session`、`relay_role`、`relay_server`、`relay_recovery`、`relay_client_status`、`kvm_session_input` 通过。
- `relay_client` 仍是基线失败，没有新的失败模式。单独跑 `--case chunked_ascii_paste_loopback`：4 次里 3 次通过、1 次在 `tests/relay_client_test.cpp:291` 断言键盘报告少 1。`--case paste_keepalive_blackholed_status` 与这次全量 `ctest` 都失败在 `tests/relay_integration_fakes.hpp:216` 的 `GuiFixture::activate()`。这两处与干净的 `76eecbf` 相同。
- `make format-check` 仍失败在 `tests/codec_startup_fallback_test.cpp:21`（基线，clang-format 23）。T2 改动过的 C/C++ 文件 `clang-format --dry-run --Werror` 通过。`make cppcheck` 因未安装 cppcheck 退出码 127，`make lint` 因此不能跑完。
- 使用 `HEADLESS=1`：GUI 预设配置 SDL3 时缺少 XINPUT（`libxi-dev`），本机不能安装。`CMAKE_PREFIX_PATH=/tmp/ffmpeg-dev/prefix/usr`：系统没有 FFmpeg 开发包；该前缀里的静态库已移开，链接的是系统共享库。
- 验收测试：A1 客户端 `video_client_stays_up_without_sending_control`；A3 客户端 `role_mismatch_reports_unsupported`；A4 除基线 `relay_client` 外的 relay 测试通过；A6a `control_session_releases_when_capture_stops`；A6b `control_session_releases_when_presentation_repeats`；A6c `control_session_accepts_restarted_sequence`；A7 `release_confirmed_tracks_challenge_not_picture`。

### T3：GUI 独立选择视频来源与键鼠去向

Status: pending
Depends on: T2
Acceptance: A9
Targets: `src/app/main.cpp`

- [ ] 两个选择器替代 remote 布尔值与 capture-only 复选框；按规则构造 0–2 个客户端并组装 `KvmSession`，control 客户端显式 `start()`，本地/无键鼠时传 `Ch9329ControlSink`。
- [ ] 流量基线、状态栏 `V:`/`C:`、串口与解码诊断按上文拆开。
- [ ] `make`、`make lint`；手动走三种组合。
- [ ] 提交 `feat(app): choose video source and control target independently`。

Evidence:

### T4：文档

Status: pending
Depends on: T3
Targets: `docs/lan-relay.md`、`README*.md`（若有 relay 使用说明）

- [ ] 三种拓扑的启动命令与 GUI 选择；说明分离模式下画面门槛在 GUI 端、relay 信任客户端声明。
- [ ] 硬件组合标记为未验证。
- [ ] 提交 `docs(relay): document split video and control topologies`。

## 进度

T2 已完成。下一个任务：T3。
