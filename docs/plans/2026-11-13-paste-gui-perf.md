# 粘贴状态机、GUI 结构与渲染性能：实施计划

Plan readiness: ready（2026-11-13 两位 reviewer 6740316f、61efff04 复审 APPROVE）。

来源：2026-11-13 三份只读审阅（GUI/UX、ASCII 粘贴状态机、性能）和两位 reviewer 的计划审阅。行号以 8da52ef 为准，执行前请重新定位。

## 用户问题

1. ASCII 粘贴几乎不可能成功。
2. GUI 不直观：设置不保存、没有首次引导、错误看不见、按钮点了没反应。
3. 存在性能问题。
4. 界面观感仍是默认 ImGui，需要按品牌色板美化。

## 安全约束（不变）

以下行为保持现状，本计划不改：

- 画面 stale（`kvm_session.cpp:419`）、UI 心跳超过 250 ms（`serial_worker.cpp:50,534`，`kvm_session.cpp:410`）、焦点丢失、睡眠、设备变化、ACK 超时、退出，都走公共释放路径，并取消进行中的粘贴。
- relay 模式下 challenge 间隔超时会把 `release_confirmed` 置假（`relay_client.cpp:1093-1108`）。粘贴期间 state 不降级，这是唯一的断链信号，router 必须继续据此释放。
- control 角色收到 `proof.video_fresh=false` 时取消 preparing/executing 粘贴（`relay_role_test.cpp:427-518`、`relay_session_test.cpp:379-394` 已验收）。
- 链路丢失后不得报告 `release_confirmed`；旧 epoch 不重放；`release_all` 绕过队列饱和。

所以"粘贴不可能成功"不能靠放宽这些检查解决。要做的是：先准确报出取消原因，去掉确认的误伤，再用硬件诊断找到真实原因，最后收敛重复的状态判断。

## 已确认的事实

- 所有取消和失败都映射成 `TextPasteError::unsupported`（`input_router.cpp:468,485,549,551,659,695`），UI 显示 "Unsupported characters found."（`main.cpp:253`）。用户无法知道粘贴为什么停了。
- 粘贴在 `InputState::preview` 下运行（`start_text` 只接受 preview，`input_router.cpp:652-657`）。物理按键边沿在 `input_router.cpp:142` 取消粘贴，抬起也算。点 Type ASCII 时按着的 Enter/Ctrl 一抬起就取消。
- `temporary_intent_` 不只是粘贴标志：`control_active()` 依赖它（`input_router.cpp:55-57`），`KvmSession::tick()` 每拍据此 `set_control_active`（`kvm_session.cpp:430-433`）。
- "粘贴是否还活着"在 InputRouter、KvmSession、RelayClient、ServerSession、Ch9329 worker 中重复判断，窗口各不相同（100/250/500 ms）。其中一部分是安全约束（见上），一部分是重复推导。
- `paste_keepalive_blackholed_status`：`hold_keyboard_ack` 扣住的是粘贴开始后的第一个 0x02 键盘帧，握手本身能完成（fixture 构造时已完成串口握手，`activate()` 只发鼠标帧）。测试要求粘贴在 ACK 被扣时存活 750 ms，与 100 ms stall / 500 ms 硬超时矛盾。测试想验证的是"KCP status 黑洞时客户端用 PasteKeepalive 续租"，用错了手段。
- `chunked_ascii_paste_loopback` 偶发失败（8 次 1 次）于键盘报文计数，原因未证实。
- `main.cpp:1103-1123` 每次主循环都 `renderer.upload(*current_frame, color_override)`，`video_renderer.cpp:225` 不检查序号，每拍 `glTexSubImage2D` + 颜色转换 pass；并每拍用旧帧到达时间记入 `sample_to_gpu_submit`。`RelayClient::video_presented()` 已按 `(generation, sequence)` 去重（`relay_client.cpp:931-954`），所以重复调用不影响新鲜度语义，只影响统计和 GPU 负载。
- 颜色覆盖的矩阵、range 和转换 pass 都在 `upload()` 里（`video_renderer.cpp:338`）。
- `diagnostics.cpp:280` 在 debug 级别 `flush_on(debug)`。
- `main.cpp` 2312 行，`main()` 里 60 多个局部状态。`Config` 已有 `capture_stable_id/capture_mode/serial_port` 的 JSON 读写和单测（`config.cpp:204-321`、`tests/config_test.cpp`），但 `main.cpp` 没有接线。
- 品牌色板在 `assets/branding/README.md`；`apply_brand_style()`（`main.cpp:53`）已映射 ImGuiCol，按钮用暗铜 `#633A2E`/`#84442E` 保证暖白字可读。其他颜色字面量：`main.cpp:1938,1961-1984,2069,2240`（`glClearColor`）。字体是 ImGui 默认。

## 不做

- 不放宽上面的安全约束。
- 不加新的传输、插件或渲染层；不做 Unicode 或输入法输入。
- 粘贴文本不记录、不持久化；日志只记长度、状态转移和原因。
- PBO、MJPEG 多线程解码、`recvmmsg` 先不做，等 macOS 实测数据。
- 不改 CH9329 协议层。

## 阶段一：可诊断、去误伤（T1、T2）

### T1：粘贴取消原因、按键抬起、硬件自检

Targets：`src/input/input_router.*`、`src/app/kvm_session.*`、`src/control/serial_worker.*`、`src/network/relay_client.*`、`src/app/main.cpp`（错误文案）、`src/app/probe.*`、`tests/relay_client_test.cpp`、`tests/input_router_test.cpp`、相关单测。

- 新增 `TextPasteError::canceled`，携带 `PasteCancelReason`：`user`、`key_press`、`focus_lost`、`video_stale`、`ui_stall`、`link_lost`、`ack_timeout`、`device_fault`、`shutdown`。每个取消点传入真实原因。`unsupported` 只用于字符校验失败。UI 按原因显示中文文案。
- 取消点各层都要带原因：`InputRouter::begin_release` 的调用方、`KvmSession::tick` 的 stale/卡顿路径、serial worker 的心跳/ACK 超时、relay client 的 `cancel_current`。下层原因通过 `ControlSnapshot`（或现有粘贴状态结构）上报，router 取最先发生的原因。
- 物理按键：粘贴期间按下边沿取消粘贴（`key_press`）；抬起边沿直接忽略，不取消、也不向 sink 提交（preview 态本来就不发键，向 sink 提交还会触发 overloaded → fault）。
- 每次粘贴结束在 info 级别记一行：结果、原因、已发送字节数/总字节数、耗时。不记文本。
- 自检：`kvmux-relay --probe-control --serial PORT --paste` 发送固定测试串 `"kvmux paste 0123456789\n"`（非用户数据，可记录），打印每个阶段和最终结果/原因。这让用户在 macOS 上直接复现本地串口粘贴。同步改 `relay_main.cpp` 参数解析与 `tests/probe_cli_test.cpp`；`control_relay_probe_passes` 断言输出无 `[SKIP]`，所以未传 `--paste` 时不打印 paste 行。
- `paste_keepalive_blackholed_status`：`activate()` 后会话处于 Captured，而 `start_text()` 只接受 Preview，所以先释放捕获并等待 Preview（或直接构造满足粘贴条件的 Preview 会话），再 start。去掉 `hold_keyboard_ack`。改用慢速逐帧应答的 FakeSerial 选项（每个键盘 ACK 延迟例如 40 ms，低于 100 ms stall），配合足够长的测试串，让粘贴正常执行超过 750 ms；期间 `drop_server_kcp` 黑洞 status，断言客户端发出 PasteKeepalive、粘贴最终 Completed。ACK 超时由 Q4 单独覆盖。
- `chunked_ascii_paste_loopback`：失败时打印完整报文序列（测试内），`--repeat until-fail:20` 复现并定位原因；在 Evidence 里写清是测试断言过严还是 release 时序缺陷。不靠放宽断言掩盖。

验收：

- Q1 单测（本地）：粘贴中画面 stale → 粘贴以 `canceled/video_stale` 结束，release 走公共路径。
- Q2 单测（relay）：粘贴中 challenge 断流超过 250 ms → `canceled/link_lost`，`release_confirmed` 不报告为真。
- Q3 单测：粘贴前按住 Enter，粘贴中抬起 → 粘贴继续且没有 KeyEdge 提交；粘贴中按下任意键 → `canceled/key_press`。
- Q4 单测（本地）：ACK 扣住超过硬超时 → `canceled/ack_timeout` 或 failed，`release_confirmed=false`。
- Q5 单测：画面新鲜、链路正常时，FakeSerial 上 4 KB 可打印 ASCII 粘贴 Completed，错误类型为空。
- Q6：`relay_client`、`relay_role`、`relay_session`、`input_router` 测试 `--repeat until-fail:20` 全部通过。
- Q7：`--probe-control --serial /dev/does-not-exist --paste` 打印 FAIL open；P3 风格的 relay 自检 `--probe-control --relay 127.0.0.1 --paste` 在 CTest 中 PASS。macOS 硬件上的 `--paste` 结果由用户运行，之前标为未验证。

Status: 完成（2026-11-13）。T2 未开始。

Evidence:

- 环境：linux-debug GUI preset，`CMAKE_PREFIX_PATH=/tmp/ffmpeg-dev/prefix/usr:/tmp/x11-dev/prefix/usr`，`CPATH=/tmp/x11-dev/prefix/usr/include`。未使用 `HEADLESS=1`。
- Q1：`paste_cancel` 中画面暂停后，粘贴以 `canceled/video_stale` 结束，epoch 改变且 `release_confirmed` 最终为真（链路仍在，走公共释放）。
- Q2：`paste_challenge_gap_cancels` 在 executing 后丢弃服务端 challenge 400 ms，结果 `canceled/link_lost`，`release_confirmed` 为假。该用例含在 `relay_client` 的 20 次重复里。
- Q3：`input_router` 中粘贴期间重复和抬起不取消、不提交；按下 `key_press` 且不提交按键。画面变旧为 `video_stale`，`release_confirmed=false` 为 `link_lost`。
- Q4：本地扣住 0x02 ACK。约 100 ms stall 时把原因记为 `ack_timeout`，700 ms 时粘贴已是 `canceled/ack_timeout` 且 `release_confirmed` 仍为假。stall 和 500 ms 硬超时都没有放宽。
- Q5：本地 FakeSerial 上 4096 字节可打印 ASCII 完成，错误和取消原因都是 `none`。一次实测约 18.5 s，`paste_cancel` 在 60 s 时限内通过。
- Q6：`ctest --preset linux-debug --repeat until-fail:20 -R '^(relay_client|relay_role|relay_session|input_router)$'` 退出码 0。四个测试各 20/20，实测约 485 s。
- Q7：`probe_cli` 通过。不存在的串口加 `--paste` 在 open 失败，没有 paste 阶段行。relay `--paste` 为 `[PASS] paste`、`result=completed`、`reason=none`。未传 `--paste` 不打印 paste 行。macOS `--paste` 未验证。
- `make test`：29/29 通过，实测约 100 s。
- `make lint` 停在已知基线：`tests/codec_startup_fallback_test.cpp:21` 的 format-check，因此没有继续跑后面的目标。单独的 `make cppcheck` 只报已知的 `src/network/relay_server.cpp:566` `identicalConditionAfterEarlyExit`。单独的 `make clang-tidy` 退出码 0。这两处都没有当作本次缺陷去改。
- `chunked_ascii_paste_loopback`：一次失败的计数是 before=4、after=1927、expected=1928。报文里有 961 次按下和 961 次抬起，少的是释放阶段的那一帧键盘报告。完成等待接受了释放前仍然为真的 `release_confirmed`。断言没有放宽。等待改为释放之后发布的新 epoch，并且状态回到 ready。随后单独 5/5，Q6 里 `relay_client` 20/20。
- `paste_keepalive_blackholed_status`：先释放到已稳定的 Preview，再把键盘 ACK 延迟设为 40 ms（释放清键本身也是 0x02，不能在释放完成前打开延迟）。20 个字符在 750 ms 黑洞期间保持服务端粘贴为 active，`keepalives_sent > 0`，之后 Completed。
- 偏差：本地 `PasteCancelReason` 走粘贴快照和 router，不走线上枚举。客户端发出的 `PasteCancel` 仍是线上 `user`。线上枚举未改。router 保留先发生的原因；UI 读 router 的结果。从未开始的粘贴不打结束日志。
- 粘贴结束打一行 info：结果、原因、已发送字节/总字节、耗时。不记录粘贴文本。自检固定串可以出现在 probe 输出里。未改 `flush_on`。

### T2：只在需要时上传，统计只记新帧，日志刷新降级

Targets：`src/app/main.cpp`、`src/render/video_renderer.*`、`src/support/diagnostics.cpp`。

- 抽出一个纯函数决定是否 `upload`：收到新 `(generation, sequence)`，或 `color_override` 改变，或某个 renderer（含副屏 `renderer2`）尚未上传当前帧。其他情况只绘制已有纹理。
- `video_presented`、`record_sample_to_gpu_submit`、`record_present` 只在新帧时调用。颜色覆盖改变触发的重新转换不刷新新鲜度。
- `flush_on` 改为 `warn`。

验收：

- Q8 单测：上传判定函数——同一帧不重复上传；颜色覆盖改变时上传；新 renderer 首次上传。
- Q9：Linux 上 FakeCapture 运行时 `sample_to_gpu_submit` 计数等于新帧数（可用诊断计数或单测）。
- 性能改善在 macOS 诊断面板（`sample_to_gpu_submit`、`SwapWindow blocking`）上对比，结果记入 `docs/hardware-validation.md`，之前标为未验证。

## 阶段二：收敛粘贴状态（T3）

依赖 T1 的硬件诊断结果。如果 macOS 上 `--paste` 和 GUI 粘贴已经稳定，T3 只做收敛，不改行为。

### 设计

粘贴只有一个执行所有者：本地是 Ch9329 worker；relay 是 `ServerSession`，由它驱动服务端 sink。客户端（GUI、InputRouter、RelayClient）持有 `PasteHandle{id}`，读取镜像状态和原因，不再各自推导粘贴是否存活。安全检查（见"安全约束"）仍在原处触发，统一转成"取消(原因)"事件送给所有者。

```
Idle → Uploading(仅 relay) → Armed → Executing → Completed
                                              ↘ Canceled(reason) | Failed(reason)
```

| 当前状态 | 事件 | 下一状态 |
| --- | --- | --- |
| Idle | start(text, owner) | 本地 Armed；relay Uploading |
| Uploading/Armed/Executing | start(...) | 拒绝新请求（`busy`），旧任务不受影响 |
| Completed/Canceled/Failed | 结果已被客户端读取（或新的 start 到达） | Idle；新的 start 随即按 Idle 处理。终态结果保留到下一次 start，供 UI 显示 |
| Uploading | 全部 chunk 与 CRC 通过 | Armed |
| Uploading | 序号乱序、超长（>4096）、CRC 失败 | Failed(invalid_upload) |
| Uploading | KCP 窗口拒收 | 保持 Uploading，按现有 PasteExecuteRetry 节奏重试 |
| Armed | 授权尚不可用（relay：有效 proof 未到；本地：未 ready） | 保持 Armed，等待；Execute 先于 proof 到达时同样等待 |
| Armed | 授权可用，且普通输入队列、`input_sync`、`ordinary_inflight` 已排空 | Executing |
| Armed/Executing | 执行端检查 epoch 与 applied intent 不匹配 | Canceled(link_lost)（保留 `serial_worker.cpp:683` 的等价校验，防止旧 epoch 任务在新连接上执行） |
| Executing | 最后一个 gesture 收到 ACK | Completed |
| 任意活动态 | 用户取消、按键按下、失焦、睡眠、退出 | Canceled(对应原因) |
| 任意活动态 | 画面 stale、UI 心跳超时、proof.video_fresh=false（control 角色） | Canceled(video_stale / ui_stall) |
| 任意活动态 | 链路断开、challenge 断流、keepalive 租约到期 | Canceled(link_lost) |
| 任意活动态 | ACK 硬超时、串口失败 | Failed(ack_timeout / device_fault) |

不变式：

- 离开活动态一律走 `release_all`；只有链路仍在时才报告 `release_confirmed`。
- 粘贴期间普通键鼠不进入 sink：按下边沿转成取消事件，抬起忽略，不再返回 overloaded 让 router 进入 fault。
- 粘贴期间需要维持 control active：用一个显式的 `paste_active` 输入意图替代 `temporary_intent_` 中属于粘贴的部分。`control_active()` 保留现有的其他意图（`special_active()`、`pending_keep_alive_` 等，见 `input_router.hpp:123`），只把粘贴来源换成 `paste_active`。

### 收敛清单

删除前逐项确认没有承担其他职责，有则在 Evidence 写明保留原因：

- 删除：`text_completion_pending_`、`paste_lifecycle_action()`、`merge_paste_status()` 中的状态推导（保留纯合并显示的部分）、客户端 `ready()/active` 对粘贴存活的判定、`control_snapshot` 中只为粘贴服务的分支（但断链时 `release_confirmed=false` 必须保留）、服务端 `owner_intent` 的后绑定。
- 合并：`direct_ascii_paste_` / `remote_ascii_paste_` 合并为"粘贴执行位置"一个枚举；`temporary_intent_` 的粘贴用途改为 `paste_active`。
- 保留：`PasteExecuteRetry`、`execute_armed`（若仍用于 Armed 等待）、执行端 epoch/intent 校验、`proof.video_fresh` 取消。
- 同步更新依赖这些分支的测试：`input_router_test.cpp`、`kvm_session_input_test.cpp` 的 FakeSink（`supports_direct_ascii_paste()==false` 走 `text_queue_` 的路径保留或明确改造）。

### Wire 协议

若粘贴消息需改字段（取消原因、busy），同步更新 `relay_wire` 编码与测试。客户端和服务端同仓库同版本，不做旧版本兼容。

### T3 任务

Targets：`src/control/serial_worker.*`、`src/control/paste_*`、`src/input/input_router.*`、`src/app/kvm_session.*`、`src/network/relay_client.*`、`relay_session.*`、`relay_server.*`、`relay_wire.*`、`src/app/main.cpp`、相关测试。

- 先把转移表写入 `docs/design/`（新文件或设计文档对应章节），代码中用一个 enum 和一个转移函数实现。
- 验收：
  - Q10：T1 的 Q1–Q6 继续通过。
  - Q11 relay：多 chunk 粘贴在 loopback 上完成；Execute 先于 proof 到达时等待后执行；KCP 窗口满时重试后完成。
  - Q12：旧 epoch 的粘贴请求在 epoch N+1 ready 后到达 → 不执行，以 `link_lost` 结束。
  - Q13：粘贴中再次 start → `busy`，旧任务完成；完成、取消、失败之后再次 start 均能正常开始（连续两次粘贴都 Completed）。
  - Q14：Uploading 收到乱序 chunk → `Failed(invalid_upload)`，走 release。
  - Q15：所有 relay、串口、输入相关测试 `--repeat until-fail:20` 通过；收敛清单逐项完成或写明保留原因。

## 阶段三：GUI 结构与 UX（T4、T5、T5b）

### T4：结构拆分（不改行为）

Targets：`src/app/main.cpp`，新增 `src/app/ui_state.hpp`、`src/app/ui_panels.*`（或 `src/app/panels/`）、`src/app/event_router.*`。

- `UiState` 分为 `ConnectionForm`、`MediaUi`、`PasteUi`、`OverlayFlags`。
- 每个面板 `UiActions draw_xxx(UiState&, const SessionView&)`，只返回动作，由主循环统一执行；面板绘制时不直接调会话。
- SDL 事件路由移入 `event_router.*`。
- 链路状态只有一个来源：视频与控制各一份 `LinkStatus{state, error, hint}`。
- 验收：Q16 现有测试全过；`main.cpp` 只剩初始化和帧循环。GUI 手动检查在 macOS 上进行，之前标为未验证。

### T5：UX 修复（依赖 T4）

Targets：T4 的文件、`src/support/config.hpp`、`src/support/config.cpp`、`tests/config_test.cpp`。

- 持久化：把已有的 `capture_stable_id/capture_mode/serial_port/baud` 接到 UI；新增视频源（local/relay）、控制目标、relay 主机与端口、Host 键、"启动时自动连接串口"。启动时恢复；视频源恢复后自动开始预览；串口只在勾选时自动连接。
- 首次引导：没有视频时自动打开连接窗口，画面中央显示 logo 与下一步提示（选择视频源 → 开始预览 → 连接控制 → 点击画面开始控制）。
- 错误：独立的错误横幅，可关闭，不依赖 Status overlay，不被截断。
- 按钮：条件不满足时置灰，tooltip 说明原因；relay 端口就地校验。
- 切换视频源或控制目标不立即重建会话，点 Apply 才生效。
- 连接窗口内每条链路一行状态（连接中、失败及原因、ready）；两条都 ready 才自动关闭。控制目标为 None 时状态点只看视频。
- 波特率改为下拉框，只列 CH9329 支持的 9 档。
- 菜单去重，分组为 连接 / 显示 / 输入 / 动作；VSync、色彩、宽高比移出连接窗口。
- 预览状态常驻提示"点击画面开始控制，Host 键：X"。
- 粘贴：进行中在状态栏显示进度和取消按钮；popup 关闭不影响粘贴；错误按 T1 的原因显示。
- 状态栏去掉 `P:/H:/d:/V:/C:` 缩写，移入 Diagnostics。
- 验收：Q17 Config 新字段往返单测，旧配置文件（缺新字段）能加载为默认值。其余 UX 项在 macOS 上手动检查，逐项记入 `docs/hardware-validation.md`，之前标为未验证。

### T5b：视觉美化（依赖 T4，可与 T5 合并提交）

颜色规范：`assets/branding/README.md` 的 Palette——Midnight `#0B1823`、Charcoal `#202A33`、Silver `#BFC5C9`、Warm white `#F3F1EB`、Connection orange `#C45F3C`、highlight `#ED9464`。

- 新增 `src/app/ui_theme.hpp`：色板常量（含 logo 渐变中的 `#08121B`，以及现用的边框 `#576570`、暗铜填充 `#633A2E`/`#84442E`），和语义 token：`bg`、`surface`、`surface_raised`、`border`、`text`、`text_muted`、`accent`、`accent_fill`、`accent_fill_hover`、`accent_fill_active`、`on_accent`、`ok`、`warn`、`error`。新增的 `#08121B`、`#576570`、暗铜、状态色写入品牌 README 的 Palette 作为扩展。
- 对比度按角色要求：
  - 正文 `text` 对 `bg`/`surface` ≥ 4.5:1（现有暖白满足）。
  - 主按钮用暗铜填充（`#633A2E`/`#84442E`）配暖白字（铜橙 `#C45F3C` 配 Midnight 只有约 4.32:1，不采用）。铜橙只用于边框、强调线、选中指示等图形元素（≥ 3:1）。默认、hover、active 状态的按钮文字都 ≥ 4.5:1；disabled 不要求。
  - `ok`/`warn`/`error` 作为文字或状态点使用时，对 `bg`、`surface` ≥ 4.5:1（图形元素至少 3:1）。不要求它们与暖白文字形成对比，横幅不用整块状态色底。`warn` 色相与铜橙可区分。
- 字体：vendored 一款 OFL 无衬线字体（如 Inter）到 `third_party/fonts/`，在 `third_party/INVENTORY.md` 登记版本、SHA-256、许可；许可安装到 `${CMAKE_INSTALL_DATAROOTDIR}/kvmux/licenses/`。优先编译进二进制（`AddFontFromMemoryTTF`），避免 bundle/POST_BUILD/install 三处复制；若按文件加载，则按 `logo.png` 的方式配置三平台复制与安装。中文字形回退到系统字体，只载入需要的字形范围，不打包完整 CJK 字体。
- HiDPI：区分像素密度和内容缩放。SDL3 后端已把像素密度作为 `DisplayFramebufferScale`（macOS Retina 为 2），此时字体按 `基础字号 × framebuffer scale` 栅格化以保证清晰，但逻辑字号和 `ScaleAllSizes` 不乘它。只有内容缩放（`SDL_GetWindowDisplayScale / framebuffer scale` 大于 1，例如 Windows 150%）才缩放样式。显示器变化时从未缩放的基础样式重算，不累乘。
- 层次与间距：8px 网格；窗口/弹窗 Charcoal + 低透明度 Silver 边框；输入框用 `surface_raised`；主按钮用 `accent_fill`，次按钮 `surface`；禁用态 `text_muted`。
- 状态栏：左侧链路状态点 + 文本，右侧分辨率/fps。错误横幅用 `surface` 底 + 左侧 `error` 色条。
- 视频区：无视频时显示居中 logo（已有 `load_brand_texture`）与引导文字，底色 `#08121B`，替代红色 "VIDEO UNAVAILABLE"。
- 验收：
  - Q18：`make lint` 增加检查：`src/app/` 下除 `ui_theme.*` 外，不得出现匹配 `IM_COL32\(|ImColor\(|brand_color\(|ImVec4\(\s*[0-9.]|ImVec4\{\s*[0-9.]|glClearColor\(\s*[0-9.]` 的文本。检查只保证这些构造形式，Evidence 按此措辞记录。
  - Q19：对比度单测：用 WCAG 相对亮度公式计算上面列出的 token 组合，断言达到门槛（期望来自 WCAG 公式，不来自实现）。
  - Q20：字体许可随 `install` 产物出现（检查 install 目录）。
  - 视觉效果、HiDPI 在 macOS 上截图记录，之前标为未验证。

## 阶段四：性能剩余项（T6，低优先级）

- 采样等待从 1 ms sleep 轮询改为条件变量通知（`video_pipeline.cpp:44`）。
- 采集样本 vector 复用（`capture_sample.cpp:95`）；raw 路径直接复制进 AVFrame，避免第二次整帧拷贝。
- sws 回退帧缓存。
- relay 发送：pacer 先按公式算包长，确认要发再建包（`udp_media.cpp:526-544`）。
- relay 接收：设置 `SO_RCVBUF`/`SO_SNDBUF`（例如 4 MB，失败只记 warn），`Address` 复用。
- 验收：Q21 现有测试全过；效果只在实测后写结论，否则标为未验证。

## 顺序

T1 → T2 → T3 → T4 → T5 → T5b → T6。T1、T2 各自独立提交。T1 完成后先请用户在 macOS 上运行 `--probe-control --serial ch341-usb:... --paste` 和 GUI 粘贴，带 `--debug --log-file`，据日志里的取消原因决定 T3 的重点；T4 起不依赖该结果，可继续。每个任务完成后：`make lint`、`make test`，按 AGENTS 要求记录 Evidence。

## 进度

T1 完成。T2 尚未开始。
