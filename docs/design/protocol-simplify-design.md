## 审阅结论

**可以简化，而且值得简化，但不应通过合并 raw UDP、KCP、视频恢复和串口 ACK 来实现。真正需要收敛的是：控制权归属、paste 的生命周期、截止时间处理，以及“串口准备完成后开始执行”的交接。**

我不建议直接把 `107b4d5` 的协议语义定稿。当前实现已经保留了不少必要的安全边界，但这些边界没有一致地覆盖 paste 的所有阶段；同时，一些本应只是进度展示的消息，实际上又承担了授权推进和续租启动的职责，因而形成了时序敏感的依赖。

审阅固定在 `107b4d5cc25e79be7f5b0142a62fa7f872bae97a`。指定的上下文文档在该提交中返回 404，因此以你上传的同名文件为审阅基准。它明确要求完整上传、校验、proof、串口 fence 和提交完成后才能执行，并要求生命周期失效时取消输入。**以下是实现和测试源码的静态审查；没有编译运行测试，也没有把文档中的集成现象当作已经复现的结果。**

---

## 一、应当先修复的边界问题

### 1. 【P1】paste 没有完整绑定控制权，旧 Cancel 又可以取消新 paste

这是我认为最重要的问题。

`ServerSession::PasteUpload` 保存了 transaction、上传内容、授权请求、token 和 lease，却没有保存创建或授权时的 `epoch / intent`。`revoke()` 默认只取消 **executing** 的 paste，不会统一作废 uploading、complete、authorizing 和已经拿到 token 的事务。与此同时，`paste_commit()` 在 transaction 和 token 匹配后就转入 executing，没有再次验证原授权所属的 epoch、intent 或同步边界。

因此，源码允许出现这样的路径：

```text
事务 T 在 epoch E、intent I 下获得 token
→ serial epoch 或控制 intent 发生变化
→ T 仍停留在 complete，token 仍存在
→ 新控制状态恢复 ready
→ 旧 PasteCommit(T, token) 到达
→ T 被转入 executing
```

串口入口还会检查当前 ready、队列和 heartbeat，所以不是所有这样的旧提交都会执行成功；但**这些检查只能证明“现在的串口可用”，不能证明“这次执行仍属于原来的授权”**。

另一个方向也存在漏洞：`cancellation()` 在检查 Cancel 的 intent 是否过期之前，已经无条件调用了 `cancel_paste()`。于是旧 intent 的迟到 Cancel，虽然不会撤销新的普通输入 barrier，却会取消新 intent 的 paste。现有测试确实检查了旧 Cancel 不撤销新 intent，但没有在这个场景中放入新的 paste。

**建议：为事务建立不可变的归属，并让失效规则覆盖全部非终态。**

归属可以是一个很小的结构：

```text
PasteOwner = 当前连接 tuple + serial epoch + input intent
```

外层 envelope 已有 tuple，不必在每条消息里重复序列化全部字段。关键是服务端不能只保存 transaction id，而把其他身份从“当前全局状态”中临时取出。

新的 intent、serial epoch 变化、会话结束和显式释放，必须统一使相应事务的上传、准备、token、待执行请求和迟到回调失效。旧 Cancel 只能影响其覆盖的 intent，不能跨代取消后来建立的事务。

---

### 2. 【P1】终态没有成为真正的终态，状态发布也没有区分进度与结果

客户端收到 `PasteStatus` 时，只有本地 `canceled` 状态获得了特殊保护。`completed / rejected / expired` 都可以被后续同 transaction 的状态覆盖。更进一步，`PasteAuthorized` 的处理没有检查事务是否已经终结，迟到的授权响应仍可以把状态改回 `authorized`，重新打开提交路径。

服务端则在收到串口终态后直接删除事务；发送端使用一个 `optional<PasteStatus>` 保存最新状态，将进度和终态一起合并。会话建立、销毁时，也没有像普通 `Status` 和 `StateAck` 那样完整清理 paste 的待发布状态及授权响应。**这意味着终态记录、待发送消息和当前事务不是同一个生命周期对象。**

这里需要特别区分两个结论：

* 文档记录的“同一 transaction 先 completed，后来 expired/deadline”，**客户端确实允许这种覆盖**。
* 但不能据此认定“服务端完成后的下一次 tick 必然把它过期”。当前完成处理会清除活动事务，单纯下一次 tick 不足以解释第二个终态。第二个状态究竟如何产生，仍需按事件轨迹复现。

**建议：把结果保留和发送合并规则明确分开。**

进度可以覆盖旧进度；终态不能被进度、授权响应或另一个事务的状态覆盖。终态至少保留到成功交给当前 KCP 通道，同时在 session 内保留最后一个事务结果，供重复请求查询。

这里不需要数据库或持久化事务日志。采用**会话内递增 transaction id、一个活动事务、最后一个终态和完成高水位**就足够。更旧的 transaction 可以拒绝，但不能重新执行，也不能凭空生成一个与原结果不同的终态。

客户端的“用户请求取消”也不应立刻等同于“远端已经取消”。当前代码把本地取消直接映射为 terminal，而新的 paste 又允许替换 terminal 对象，可能连尚未发出的取消意图一起替换掉。应区分：

```text
本地已请求取消
≠ 服务端已停止这个 job
≠ CH9329 已确认释放全部输入
```

这不一定需要三种新 wire 消息，但客户端不能把第一项当作后两项已经完成。

---

### 3. 【P2】授权重试和执行续租仍然依赖不稳定的往返时序

这里有三个具体问题。

**首先，授权重试存在 request id 卡死路径。** 客户端授权等待超过一秒后会发起新的 `request_id`；服务端一旦生成 token，重复授权请求始终获得最初保存的 `authorization_request`。客户端又只接受当前 request id 的响应。因此，只要 token 响应延迟跨过客户端重试边界，客户端与服务端就可能永久等待不同的 request id。

```text
客户端发送 Authorize(request=1)
→ 服务端生成 Authorized(request=1)
→ 响应迟到，客户端改为 request=2
→ 服务端仍返回 request=1
→ 客户端丢弃
```

短期修复很直接：**同一次逻辑授权的重试必须保持 request id 不变**。不能把重传和重新授权混为一谈。

**其次，`request.challenge == latest_proof` 过于依赖 raw 与 KCP 的处理先后。** 一个仍处于有效时间窗口的授权请求，会因为更新的 raw proof 先被处理而遭拒绝。这不是 proof 过期，而是“它不再是最新编号”。我建议保留有效性和归属检查，但不要把“恰好等于此刻最新编号”作为独立安全条件；或者让服务端在收到执行意图后，自己等待符合要求的有效 proof。

**第三，执行 lease 的启动仍依赖首个 executing 状态。** 服务端收到 Commit 后立即启动 500 ms lease；客户端却只有收到 `PasteStatus(executing)` 才开始发送 keepalive。如果首个 executing 状态被 KCP 背压或反向链路延迟挡住，服务端可能在客户端仍正常运行时取消任务。当前“屏蔽服务端状态”的测试是在客户端已经观察到 executing 后才屏蔽，覆盖不到这个启动窗口。

短期应从 **Commit 已进入可靠发送路径** 开始维持该事务的续租意图，直到收到权威终态或本地生命周期失效。服务端仍只对合法的执行中事务续租，keepalive 本身不能授予执行权限。这样，进度状态才真正是观察结果，而不是维持执行所必需的隐式 ACK。

---

### 4. 【P1】deadline 处理尚未真正集中，会话销毁后还存在继续使用 KCP 的路径

`RelayServer::run()` 在循环后段依次处理串口 paste snapshot、`session.tick()` 和 control snapshot。但 `tick()` 可以产生 `expired` action，而该 action 会立即 `kcp.reset()`。后续代码没有再次检查 KCP 是否存在，仍会进入状态提交或 `kcp->update()`。这是明确的空指针解引用路径，应先修复。

另外，“先处理 KCP 和串口终态，再检查 deadline”的规则现在只在循环尾部成立。`on_datagram()`、Cancel、Authorize、Commit 等入口仍会先调用 `deadlines()`。同一个截止时刻，前面是否恰好收到一个 raw 包或另一条控制消息，会改变 keepalive 和串口完成事件是否有机会被处理。

现有边界测试直接调用“keepalive 或 completed snapshot，然后 tick”，验证的是这一种调用顺序，不是实际网络循环允许的全部顺序。

**建议：把一轮处理的时间和顺序变成显式契约。**

每轮收集有界输入和串口事件，保留必要的接收时间；按定义处理撤销、完成和续租，再进行统一 deadline 推进。任何导致会话销毁的转换，都必须终止本轮属于旧 session 的状态发布和传输操作。

“同轮已到达的完成事件优先”也不能扩大为“任何迟到 keepalive 都可以续命”。`now == deadline` 和 `now > deadline` 的行为需要分别定义、分别测试，不能再靠某个方法有没有先调用 `deadlines()` 决定。

---

### 5. 【P2】控制背压和 paste 生命周期会阻塞视频恢复

这里存在两个不必要的跨协议依赖。

服务端持有未发出的 KCP UDP batch 时，`drain_control()` 失败就直接 `continue`。这不仅暂停 KCP 更新，还跳过了后面的 pacer deadline、视频发送和 source admission 服务。**控制 socket 的 would_block 因而影响了使用另一个 socket 的视频路径。**

客户端则在整个 paste 生命周期内停止提交 `RefreshRequest` 和 `MediaFeedback`。对于普通统计 feedback，这可能只是暂时降低自适应能力；对于 HEVC refresh，这会阻断恢复链：

```text
paste 正在上传或等待执行条件
→ HEVC 丢失依赖帧，receiver 等待 IDR
→ refresh 因 paste_lifecycle_pending 被压住
→ 画面与 proof 无法及时恢复
→ paste 又等待健康的控制/画面条件
```

代码中的禁止条件是整个 paste 生命周期，而不是“当前确实没有可用发送容量”。

**建议保留 KCP 的 held-batch 规则，但缩小它的作用范围。**

未发送 batch 没有排空时，不调用 KCP update；然而 raw Cancel、challenge/proof、视频 pacer 和所有截止时间仍应继续服务。

发送调度也应区分“撤销和事务控制”“恢复请求”“普通数据”“可覆盖的统计进度”，而不是一个 paste 标志关闭整个反馈通道。优先级必须在消息进入 KCP 之前落实；已经进入同一可靠有序通道的消息，不能靠后来的优先级重新越过前面的消息。

第一版不需要新增第二条 KCP 通道。**先用有界保留容量和明确的提交顺序解决，避免把局部调度问题升级为多通道协议问题。**

---

### 6. 【P1/P2】串口交接不够原子，worker 的私有状态泄露到了调用线程

串口 worker 的 ACK 主链是合理的：同步先发送键盘报告，收到 ACK 后发送鼠标报告，第二个 ACK 后才发布 immutable applied state；取消后的迟到 ACK 不会重新发布旧同步结果。这个边界应该保留。

问题在准备和启动之间。

当前 relay 先提交 private Sync，轮询 applied snapshot，发 token，再等待 Commit，最后调用 `start_ascii_paste()`。这中间串口状态可以变化。`start_ascii_paste()` 还会检查 `impl_->transaction`，而该对象在 worker 的多条路径中由 worker 私有地更新，并没有统一受调用线程持有的 mutex 保护，存在并发访问风险。即使暂不考虑数据竞争，准备完成后的 GET_INFO 事务也可能使启动返回 not_ready，随后被 relay 当作 paste 启动失败。

**建议把“安全准备并开始这个 job”的最终决策移入串口 worker。**

短期不必增加通用任务系统：一个有界的待启动 job 槽位就够。worker 自己决定何时排在诊断事务后执行，并检查预期 owner、取消状态和有效 lease。relay 不再通过读取 worker 私有 transaction 来判断能否启动。

同时，`AsciiPasteJob / AsciiPasteSnapshot` 应有可关联的 job id。现在 snapshot 只有状态与进度，服务端只能把它解释为“当前执行中的那个 paste”，缺少明确的异步结果归属。

---

## 二、哪些边界应该保留，哪些可以合并

### 会话身份、控制权和执行确认不能合成一个 generation

目前这些编号有不同用途，不是简单的重复状态：

| 对象                                           | 建议              | 原因与职责                              |
| -------------------------------------------- | --------------- | ---------------------------------- |
| `peer + tuple`                               | **保留。**         | 它隔离不同连接及旧连接报文，不应被新的控制 intent 替代。   |
| 媒体 `generation`                              | **保留。**         | 它标识协商的视频流，防止旧媒体进入新流。               |
| serial `epoch`                               | **保留。**         | 它标识串口故障、释放和重建之后的执行边界。              |
| input `intent`                               | **保留。**         | 它区分用户新一轮接管、释放之后的输入意图。              |
| `Sync.revision`、`edge_floor` 和 edge sequence | **保留。**         | 前者关联状态同步，后两者阻止缺口或恢复后的不确定 edge 重放。  |
| decoder recovery marker                      | **保留，但保持本地职责。** | 它用于淘汰已经进入解码线程的旧工作，不需要变成额外控制权协议。    |
| paste owner、worker job id                    | **补齐。**         | 它们将事务和异步串口结果绑定到上述既有边界，而不是再造一套身份体系。 |

现有实现已经对 tuple、展示身份、串口 applied snapshot 和 edge floor 做了相应隔离；应补齐 paste 的覆盖范围，而不是删除这些区分。

会话握手也暂时保留 `Hello → Welcome → Confirm → Ready`。已有测试覆盖各阶段重试、重复确认、Busy 和旧 nonce；为了少一两种消息而让会话建立隐含在 KCP 或媒体包中，收益不大。普通 release 应释放当前 intent，但继续保留预览会话；disconnect 或 session expiry 才释放整个控制者占用。

另外，**tuple 和 PasteAuthorized token 都不是身份认证机制**。仓库部署文档本来就限定可信 LAN、无加密和无认证。删减 token 往返时必须保留其对应的执行前置条件，但不应把这个 token 当作需要独立保留的密码学安全层。

### Sync、StateAck 和 Cancel 应保留，但归属更明确

普通输入仍应遵循：

```text
当前控制权有效
→ Sync 被串口执行
→ 两个 CH9329 ACK 完成
→ StateAck
→ 接受后续 edge
```

KCP ACK 只证明传输层收到数据，不能替代串口 StateAck。`SubmitResult::accepted` 也只证明接纳，不等于设备执行完成。

raw Cancel 与 KCP Cancel 的双路径值得保留：前者可以越过可靠通道中的积压，后者提供可靠补送。两者必须进入同一个幂等的 intent 撤销入口，而不是各自影响不同范围的状态。现有串口测试已经验证了相对运动超时后不重放、同步双 ACK、取消后迟到 ACK 不发布；这些应继续作为硬约束。

`Edge.challenge` 也不宜因为已有 execution lease 就直接删除。可靠有序不等于及时，积压的旧点击或相对运动不能借后来续上的 lease 重新执行。可以统一 freshness 检查代码，但不能顺手删除“这条输入本身是否已经陈旧”的保护。

---

## 三、我推荐的 paste 简化路线

### 将两阶段授权往返，改成一次执行意图

当前客户端收到 token 后会自动 Commit，中间没有新的用户确认。因此，`Authorize → Authorized → Commit` 并没有表达三个不同的用户决定，却带来了 request id 重试、token 缓存、token 对当前 owner 是否仍有效等额外状态。

我建议目标协议收敛为：

```text
PasteBegin(tx, owner, byte_count, crc32)
PasteChunk(tx, index, payload)*
PasteExecute(tx)
PasteCancel(tx, reason)
PasteKeepalive(tx)
PasteStatus(tx, phase, progress, outcome, reason)
```

这里的 `PasteExecute` 只是**提交执行意图**，不是“收到就执行”。服务端仍然必须等待：

```text
上传完成并通过校验
∧ owner 仍有效
∧ 满足约定的展示 proof 条件
∧ 串口准备完成
∧ 没有取消
→ 最多启动一个 worker job
```

这样可以删除公开的 `PasteAuthorize / PasteAuthorized`、一次性 token、客户端授权 request id 重试，以及“拿到 token 后还没 Commit”的独立等待阶段。**删除的是跨网络往返，不是 proof 或串口 fence。**

服务端内部可以使用少量显式阶段：

```text
Uploading → Uploaded → Preparing → Executing → Finished
```

`Finished` 带完成、取消或失败结果及原因；执行中的取消可以经过 stopping/draining，但它不应再被若干互相独立的 bool 隐式表示。

`complete` 应至少重命名为 `uploaded`，避免和 `completed` 混淆。校验失败也应明确进入失败终态，而不是停在 `complete + invalid`。当前 complete 阶段不会再受到上传 30 秒 deadline 约束，只要会话继续存活就可以长期保留，因此还需要明确**执行前等待的上限或失效条件**。

### paste 不必先做一次普通 GUI Sync，再做一次 private Sync

当前 `start_text()` 只允许从 preview 发起，随后远端路径先建立普通 Sync barrier，上传后又执行 private authorization fence。对于这种不允许与普通捕获输入并行的 paste，两个串口准备过程值得合并。

我建议：

**上传阶段没有 HID 副作用，不要求串口已经为 paste 完成同步；真正执行前，由 worker 做唯一一次 paste 专用准备。**

这需要把“建立新的 paste intent”从普通 `synchronize()` 的副作用中分离出来，并在上传、执行意图和 proof 之间明确绑定 owner，但不需要再增加一种网络握手。

worker 的准备过程负责确认中性键盘状态、正确鼠标模式、取消边界和可执行条件。完成后直接开始同一个 job，不再把执行权交回网络层等待 token 往返。普通 GUI 的 revision 空间保持原样，private fence 则改为 worker 内部带类型和 job id 的操作，不再占用 revision 的高位。

这是第二阶段重构，不建议与第一轮缺陷修复混在一起做。

### 删除旧的 GUI 文本执行器，只保留一个执行者

`InputRouter` 仍保留了逐 edge 文本调度、等待 `completed_ordinary_sequence`、`text_edge_inflight_` 和旧的 `schedule_text()` 路径；但当前本地 paste 已交给 serial worker，远端 paste 也已交给 relay transaction。

这部分是明确的删除候选。GUI 应负责文本输入、规范化检查、用户意图和生命周期；不再保留第三套文本执行节拍。

可以同时减少远端路径中不必要的 HID gesture 构造和副本。原始文本在 App 检查、relay 再校验是合理的；**真正用于执行的 HID 映射和 ACK 游标应由硬件侧路径持有**。普通 special key 功能不要因为也使用 edge 就一起删除。

---

## 四、媒体、proof 和 lease 应如何调整

### 保留媒体算法，收拢发送完成的职责

当前 raw UDP 媒体的主体不需要推翻：固定 MTU、8 个 data 加一个 XOR parity、有界重组、MJPEG 丢旧帧、HEVC 连续访问单元与 IDR 恢复，以及 source admission 与字节 pacer 的分离都有明确用途。媒体测试也覆盖了单片恢复、不可恢复丢失、重复冲突、HEVC gap、IDR 越过缺口和 blackout。

应调整的是发送完成的定义和归属：

目前 pacer 在最后一次 **pull** 后释放 active frame，server 继续保存未发送的 final datagram 和 deadline，等 UDP 真正发送成功后才记入 history，proof 又可能提前到达。这些状态分散在两个对象中，才需要额外的 pending proof 协调。

建议把以下内容收进同一个小型媒体发送组件：

```text
当前帧与 packet cursor
未发出的单个 datagram
该 datagram 的 deadline
实际 send 结果
可供 proof 检查的帧记录
```

这只是职责移动，不需要通用调度框架。

还可以进一步区分“所有 data 已成功提交给 socket”和“包含 parity 的整个 pacing 已完成”。前者可能已足以支持客户端展示，后者并不总是展示成立的必要条件。但这一步必须有 final parity would_block 的确定性测试，不能直接把任意 `sending` 帧视为有效 proof。

在此之前，**保留 pending proof 的原始接收时间检查**。不能为了减少状态而让原本迟到的 proof 因为后续处理延迟获得新窗口。

### 不要再用一个 `fresh()` 同时代表所有健康条件

当前客户端的 `fresh()` 同时包含 GUI 进度、challenge 新鲜度和实际展示进度；它又被用于控制 snapshot、Sync 准入和 paste 生命周期。服务端 executing paste 却另有独立 keepalive lease。于是“短暂 raw proof 缺口是否允许 paste 继续”，在 session、client 和 GUI 层会得到不同答案。

我建议明确区分：

| 条件                     | 应表达的事实                        |
| ---------------------- | ----------------------------- |
| GUI progress           | 用户界面仍在运行，没有失去本地生命周期约束。        |
| Presentation freshness | 当前 generation 的画面确实被展示，且没有陈旧。 |
| Raw proof lease        | relay 最近接受过符合要求的展示声明。         |
| Paste execution lease  | 执行中的事务仍收到有效的续租意图。             |
| Serial readiness       | CH9329 执行链和释放确认处于可用状态。        |

这些事实可以共同派生“能否控制”“能否启动 paste”，但不应该被压缩成一个含义不清的 ready/stalled 状态，再由上层反推取消原因。

**建议保留上下文中的严格安全规则：真正的 video stale、generation/mode 变化、focus/host、epoch/intent 变化和 serial fault 都终止 paste；是否容忍短暂 raw challenge 缺口则单独定义。** 不应让网络线程持续发出的 keepalive 独自证明 GUI 和真实展示仍健康，也不应仅因一个统计反馈或进度响应延迟就认定它们已经失效。

---

## 五、可行的实施顺序

我建议分三步，不做一次性大重写。

| 阶段                     | 范围                                                                                                                                     | 验收重点                                             |
| ---------------------- | -------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------ |
| **第一步：修复当前协议的语义缺口。**   | 补齐 paste owner；修正旧 Cancel；保护终态；稳定授权 request id；消除 executing 状态对续租启动的依赖；修复会话销毁后访问；集中 deadline 规则；让 KCP 背压不阻塞视频与 refresh；修正 worker 并发准入。 | 不改变整体传输选择，先让当前消息在延迟、取消、超时和重试下得到一致结果。             |
| **第二步：收敛 paste 协议。**   | 用一次 Execute 意图替代 token 往返；将串口准备与 job 启动交给 worker；取消 paste 对普通 GUI Sync 的重复依赖；删除旧 GUI 文本执行器。                                            | 同一活动会话内，每个 transaction 最多启动一次 job；新旧意图和迟到结果完全隔离。 |
| **第三步：收拢媒体发送及健康状态投影。** | 统一 pending datagram、deadline、发送完成记录；明确 data/parity 与 proof 的关系；拆分展示健康、proof lease 和控制许可。                                               | 视频恢复不被 paste 阻塞；所有恢复路径遵守同一组取消规则。                 |

这里的执行保证应限定为**当前活动会话内的不重复启动**。断线或进程故障后，已经发生多少目标端输入可能无法完全确认；不要自动重放部分 paste，也不必为此引入持久化分布式事务系统。

---

## 六、必须补充的确定性测试

现有测试有价值，但最缺的是**组合边界**，不是再增加几个单独的消息 roundtrip。

我建议先给网络循环增加可单步推进的测试入口，注入单调时钟、UDP send 结果和带 job id 的串口事件；不必做大型网络仿真器。真实线程与真实 socket 测试保留为少量集成验证。

| 测试主题                      | 精确构造的场景                                                                                  | 必须成立的结果                                                            |
| ------------------------- | ---------------------------------------------------------------------------------------- | ------------------------------------------------------------------ |
| **会话销毁与接管。**              | 在 session deadline 触发销毁时，仍存在待发布 Status、paste 结果或 held KCP batch；随后建立新 tuple。             | 不访问已销毁 KCP，不向新会话发布旧状态，旧 tuple 报文不影响新控制者。                           |
| **paste 全阶段归属。**          | 在 uploading、uploaded、preparing、授权完成、executing 各阶段切换 epoch/intent；再投递旧 Commit、旧 callback。 | 没有跨 owner 执行，没有旧结果归入新 job。                                         |
| **旧 Cancel 与新 paste。**    | intent I 释放，I+1 启动 paste，随后收到 I 的 raw 和 KCP Cancel。                                      | 新 paste 不被取消；旧请求仍幂等处理。                                             |
| **授权延迟与重试。**              | token 响应延迟超过一秒；重复请求；有效旧 challenge 与更新 raw proof 交错。                                      | 不出现 request id 永久不匹配，也不因“不是最新编号”无意义地追逐重试。                          |
| **首个 executing 状态丢失。**    | 服务端已经启动 job，但客户端还没看到 executing；反向状态被阻塞超过执行 lease。                                        | 合法客户端续租不依赖首个进度响应；本地取消仍能终止事务。                                       |
| **终态与取消竞争。**              | 最后一个 HID ACK 与 Cancel 分别按两个顺序到达；终态后再投递旧 progress、Authorized、Commit。                      | 结果符合约定的事件顺序，终态不倒退，job 不重启；释放确认独立成立。                                |
| **deadline 同刻事件。**        | 在 deadline 前、恰好 deadline、deadline 后投递 keepalive/串口完成，并排列无关 raw、Sync、Cancel。              | 结果由时间及明确优先规则决定，不由方法调用顺序偶然决定。                                       |
| **真实 would_block 调度。**    | 第 N 个控制 UDP 包返回 would_block，持续跨过若干循环，同时视频 socket 可写。                                     | held batch 有界、不提前 update KCP；视频 deadline、raw Cancel 和 pacing 仍被服务。 |
| **paste 与 HEVC 恢复。**      | 上传、准备和执行期间分别丢失依赖 AU，触发 waiting IDR。                                                      | refresh 有发送机会；恢复或取消行为符合所选展示安全规则，不形成互相等待。                           |
| **proof 与 final parity。** | data 已完成、parity 尚在 pacing；proof 在窗口内或窗口外到达；final send 再发生 would_block。                   | 不认可未具备资格的帧，不接受迟到 proof，也不因处理推迟错误拒绝准时 proof。                        |
| **串口准备与启动。**              | GET_INFO 正在进行、同步只收到第一个 ACK、取消发生在部分写入或 ACK 前后、旧 job snapshot 迟到。                          | 启动由 worker 决定；没有半个 fence 被当作完成，没有旧 ACK 推进新 job。                    |
| **上传、次数与恢复。**             | 960、961、65536 字节；错误 CRC/ASCII；重复或冲突 chunk；release 后立即再次 paste；完成后切换相对鼠标。                 | 校验失败有终态；一次事务仅启动一次；报告次数按用途精确核对；普通同步 revision 未被污染。                  |

还需要先修正几处现有测试的有效性：

**第一，部分 session 测试出现时间倒退。** 例如在较晚时刻 Commit 后，再用更早时刻调用 `paste_started` 和 progress。应让 fake clock 强制单调，避免测试通过了真实运行不可能出现的顺序。

**第二，`paste_keepalive_survives_blackholed_server_status_test` 的前置条件互相冲突。** 它在 `activate()` 前就 hold 所有键盘 ACK，会阻止激活所需的同步；激活成功后的状态又是 captured，而 `start_text()` 只允许 preview。即使修正这两处，hold 单个串口 ACK 750 ms 也会碰到 worker 的 100 ms stall 和 500 ms hard timeout，不能用来证明单纯的网络 keepalive 行为。应保持串口逐报告 ACK 正常，用足够长的 job 覆盖网络测试窗口。

**第三，961 字节集成测试对键盘报告总数使用的是 `>=`。** 它能证明至少发生了所需报告，却不能排除重复输入。应将准备、文本执行和释放报告分开计数，对 job 的报告数及 ACK 推进作精确断言。

---

## 最后的取舍

我的推荐不是“把所有状态机统一成一个”，而是：

**会话层决定谁有控制权；媒体层证明哪一帧具备展示资格；控制层确认普通输入的同步边界；paste 层保存唯一事务结果；串口 worker 独占实际执行和 ACK 推进。**

应删除的是跨这些层重复表达的授权等待、GUI 文本节拍和隐式推进状态；应保留的是 tuple/epoch/intent fencing、真实展示确认、串口双 ACK、不可重放输入的边界，以及有界队列。

按这个方向，协议消息会更少，状态职责会更清楚，而且能够直接针对当前时序敏感问题补出确定性测试。最重要的变化不是减少几个枚举值，而是让**同一个事务在同一组输入事件下，只能得到一个可解释、不会回退的结果**。
