# SEL 业务详解：这本"硬件事件账本"到底记什么、给谁看

> 面向 Java 背景、7 年没碰 C++、做过 openUBMC、正在学 OpenBMC 的你。
> 本文**不重复讲代码结构**（那些看 `ONBOARDING.zh-CN.md`），也不逐行拆代码（那些看 `SEL机制讲解.md`），
> 而是回答三个问题：
> ① 真实运维场景里 SEL 被拿来干什么？② 为什么 OpenBMC 非要单独做一本 SEL 账本？
> ③ 它和 phosphor-logging 是什么分工？
>
> 讲法：先给你一个能想象的真实场景，再点出背后的业务概念，最后告诉你"对应代码里的哪一段"。
>
> 📌 **路径说明**：全部代码就两个地方——`src/sel_logger.cpp`（主程序 + 落地逻辑）和
> `include/*.hpp`（core API + 各 monitor）；`meson.options` / `meson.build` 是编译开关；
> `rsyslog/phosphor-sel-logger.conf` 决定 journal 字段怎么落盘成文件。
> 所有 `文件:行号` 都按当前仓库用 sed 核对过。⚠️ 注意 `ONBOARDING.zh-CN.md` 和
> `业务流程交互图.svg` 里的旧行号有偏移（把 `main` 写成 418、`getNewRecordId` 写成 118、
> `selDeleteTargetRecord` 写成 253），当前代码里分别是 `src/sel_logger.cpp:396`、
> `:96`、`:167`，**本文一律以当前代码为准**。

---

## 0. 一句话说清业务

把你熟悉的 openUBMC 心智搬过来：这个守护进程就是 OpenBMC 里的**"硬件事件记账员"**。

BMC 自己是一块永远通电的小电脑，它身边围着一堆会"出事"的部件：温度传感器会超温、
watchdog 会超时、Host 会报错、风扇会停。这些事发生时，**人不在现场，主机 OS 可能已经死了**。
本进程做的事就是：**把这些事件一条条记进一本符合 IPMI 标准的账本（SEL），
让运维事后能用 `ipmitool sel list` 查到"什么时候、什么部件、发生了什么"**。

**这就是它的全部业务：把 BMC 上转瞬即逝的硬件事件，变成可追溯、可查询、标准统一的记录。**

---

## 1. 先讲个真实场景：运维是怎么用 SEL 破案的

把这段故事在脑子里过一遍，后面所有业务概念都从这里长出来：

```
① 半夜 2 点，机房某台服务器没响应了
   → 业务 OS 卡死，SSH 连不上；运维人在家里，不可能跑到机房。

② 运维用带外管理口（BMC 网口）连上去，第一件事就是查 SEL：
   ipmitool -I lanplus -H <BMC_IP> -U ADMIN -P <pwd> sel elist
   → 这行命令不是连主机，是连 BMC。主机死了它照样能查。

③ 屏幕上出现类似这样的账本（每行 = 一条 SEL 记录）：
   1 | 11/16/2026 | 02:10:03 | Temperature sensor #0x67
     | Upper Critical going high - Reading=92 Threshold=85
   2 | 11/16/2026 | 02:10:05 | Watchdog Timer (BIOS POST)
     | Timer expired - Hard Reset action
   3 | 11/16/2026 | 02:11:01 | Host0 state is on
     | DCPowerOn

④ 运维据此还原出因果链：
   传感器温度冲到 92°C（超过 85°C 红线）→ 触发Critical告警 →
   BIOS POST 阶段 watchdog 超时 → 按策略 HardReset → 主机重启 → 又起来了。
   → 结论：散热出问题了（风扇故障/硅脂干），换风扇、清灰、重涂硅脂。

⑤ 这些行是谁写进去的？—— 就是本进程 sel-logger：
   第 1 行来自阈值监控  include/threshold_event_monitor.hpp:339（监听 ThresholdAsserted）
   第 2 行来自看门狗监控 include/watchdog_event_monitor.hpp:255（监听 Watchdog.Timeout）
   第 3 行来自脉冲监控   include/pulse_event_monitor.hpp:100（监听 State.Host 属性变化）
```

> 📌 这个"查账破案"的故事就是 sel-logger 的业务主线。它做的事 =
> **监听事件（第①②路的信号）→ 填成标准 SEL 格式 → 写进账本**，供第②步的 `ipmitool` 查询。

---

## 2. 为什么非得单独做一本 SEL 账本（业务价值）

你可能会问：事件写普通日志不就行了吗，为什么要专门搞个 SEL？四个真实价值：

| 业务价值 | 场景 | 没有 SEL 会怎样 |
|---|---|---|
| **带外可查**（最重要） | 主机 OS 死机、没装系统、正在 POST | 普通日志在主机上，死了就查不到；SEL 在 BMC 上，主机死了照样查 |
| **IPMI 标准互通** | 用 `ipmitool`、厂商管理卡、第三方网管平台都能读 | 各家自定义格式，工具链对不上，运维要学 N 种命令 |
| **因果可追溯** | 机器半夜重启、反复宕机，要查"为什么" | 只有"当前状态"，没有"历史流水"，破案无门 |
| **结构化字段** | 要做告警平台、要按传感器聚合统计 | 纯文本日志难解析；SEL 的 event data 是定长字节，机器可读 |

一句话：SEL 的价值就在"**主机越是不省心的状态，它越能留下线索**"——
主机活着时你看 `dmesg`，主机死了你只能靠 BMC 的 SEL。

> Java 类比：SEL 不是 `System.out.println`，而是数据库里一张带固定 schema 的 `event_log` 表
> + 一个对外提供标准查询接口的 DAO。可查询、可聚合、格式稳定。

---

## 3. SEL 是什么：IPMI 标准里的硬件事件账本（先场景，后术语）

### 3.1 从 `ipmitool sel list` 的每一行看起

`ipmitool sel elist` 输出的每一行，就是一条 SEL record（记录）。它不是一个随意的字符串，
而是**IPMI 规范定义好的定长结构**。业务上你只要记住一条记录里装了这几样东西：

| 字段 | 白话含义 | 本仓库里怎么来的 |
|---|---|---|
| **Record ID** | 账本行的流水号（2 字节） | `getNewRecordId()`，`src/sel_logger.cpp:96` / `:286` |
| **Timestamp** | 事件发生时间 | journal 自带时间戳；清空时间另存 `/var/lib/ipmi/sel_erase_time`（`src/sel_logger.cpp:66`） |
| **Generator ID** | "谁报的案"，通常是 BMC 的 IPMB 地址 | 常量 `selBMCGenID = 0x0020`，`include/sel_logger.hpp:38` |
| **Record Type** | 这条是系统事件还是 OEM 自定义 | `selSystemType = 0x02`（系统），`include/sel_logger.hpp:37` |
| **Event Data** | 事件的核心内容，**标准记录最多 3 字节** | `selEvtDataMaxSize = 3`，`include/sel_logger.hpp:41` |

### 3.2 术语点出来：SEL = System Event Log

**SEL（System Event Log，系统事件日志）是 IPMI 规范定义的、BMC 上的一块"硬件事件账本"。**
它有三个关键业务属性：

- **标准定长**：一条 SEL record 有固定字节布局，所以任何兼容 IPMI 的工具都能解析——
  这就是 `ipmitool sel list` 能跨厂商通用的原因。
- **硬件视角**：它记的是"传感器越限、看门狗超时、电源状态变化"这类**硬件/固件层**事件，
  不是应用层日志。
- **持久留痕**：掉电、重启后仍在（本仓库用文件 + journal 实现，见第 4 节）。

> 📌 openUBMC 对照：如果你在 openUBMC 里习惯的是"传感器告警 → 统一日志/告警服务"
> 那条链路，那么这里的 SEL 就是**同一件事的 IPMI 协议视角**——它面向的是 IPMI 工具链，
> 而不是 Web 界面。一个事件可以同时进 logging（给 Redfish 看）和进 SEL（给 ipmitool 看），
> 这就是第 6 节要讲的"同源两视图"。

### 3.3 3 字节 event data 的业务含义（先记住，第 7 节展开）

SEL record 里真正描述"发生了什么"的只有 **3 个字节**（`include/sel_logger.hpp:41`），
这是 IPMI 规范对标准记录的硬限制，不是本仓库偷懒。所以代码里每个 monitor 都在绞尽脑汁
把信息塞进这 3 字节：

- 阈值事件：第 0 字节放"哪种越限"（`include/threshold_event_monitor.hpp:27-33` 的枚举），
  第 1、2 字节放"触发读数"和"阈值"（`:152`、`:198`，经 `getScaledIPMIValue` 换算，
  实现在 `include/sensorutils.hpp:327`）。
- watchdog 事件：3 字节被拆成位域——第 0 字节放"超时后动作"（`include/watchdog_event_monitor.hpp:27-33`、
  赋值 `:99-114`），第 1 字节高低 4 位分别放"中断类型"和"定时器用途"（`:116-181`）。
- Host 错误：干脆只放 1 字节类型，剩下 2 字节填 `0xff` 表示"未指定"（`include/host_error_event_monitor.hpp:73-75`）。

超出 3 字节直接抛异常 `invalid_argument`（`include/sel_logger.hpp:76-78`）。

---

## 4. 谁在写这本账：本进程在整条链路里的位置

理解业务边界最重要的一句话：**sel-logger 只管"记账"，不管"查账"。**

```
   传感器 / watchdog / Host 状态 / 电源
        │  D-Bus 信号（事件发生了）
        ▼
   ┌─────────────────────────────────────┐
   │  sel-logger（本进程）= 记账员/生产者  │
   │  monitor 监听 → 填 3 字节 → 分配 id   │
   └─────────────────────────────────────┘
        │  写 journal 字段（MESSAGE_ID + IPMI_SEL_*）
        ▼
   rsyslog 按 MESSAGE_ID 过滤，落盘成 /var/log/ipmi_sel
   （rsyslog/phosphor-sel-logger.conf:18-19）
        │
        ▼
   ipmi-daemon（另一个进程）= 查询员/消费者
   把文件/ journal 里的字段组装成 IPMI 响应
        │
        ▼
   ipmitool sel list  ← 运维在这里看到账本
```

对应代码证据：

- 本进程向 D-Bus 注册的接口名叫 `xyz.openbmc_project.Logging.IPMI`
  （`include/sel_logger.hpp:28-31`，注册动作在 `src/sel_logger.cpp:408-413`），
  提供 `IpmiSelAdd` / `IpmiSelAddOem` 等方法（`:416-430`）——
  这是**别人喊它记账**的入口。
- 它自己主动监听各种事件信号（`src/sel_logger.cpp:444-462` 启动的 monitor）——
  这是**它自己发现事情并记账**的入口。
- 落盘的 journal 字段约定写在 `include/sel_logger.hpp:129-135`，
  `MESSAGE_ID` 是统一鉴别码 `b370836ccf2f4850ac5bee185b77893a`（`:35`），
  rsyslog 靠它把 SEL 记录从海量日志里捞出来（`rsyslog/phosphor-sel-logger.conf:18`）。

> 📌 这就是典型的 OpenBMC **"用 journal 当数据库、用字段名当表结构"**模式（README.md:6-8）：
> `MESSAGE_ID` 相当于表名/ discriminator，`IPMI_SEL_RECORD_ID/TYPE/DATA...` 相当于列。
> 两个进程通过约定的字段格式解耦——很像 Java 里"生产者写消息、消费者按 schema 读消息"，
> 只不过这里的"消息中间件"是 systemd journal。

---

## 5. 事件从哪来：五类 monitor 对应的真实业务场景

本仓库每个 monitor 都是**头文件内联实现**（一个 `startXxxMonitor(conn)` 返回 D-Bus match），
这是它作为"最小仓库"的特点。五类事件源对应的业务场景如下：

| Monitor | 真实触发场景（业务语言） | 监听的信号 | 代码位置 |
|---|---|---|---|
| **阈值告警** | 温度/电压/风扇转速越过 Warning/Critical 红线 | `ThresholdAsserted` | `include/threshold_event_monitor.hpp:40`、match `:339` |
| **阈值告警（新）** | 同上，但走新的 `*AlarmAsserted/Deasserted` 信号 | 8 种 Alarm 信号 | `include/threshold_alarm_event_monitor.hpp:39-47`、`:258` |
| **watchdog** | BIOS/OS 喂狗超时，按策略重启或断电 | `Watchdog.Timeout` | `include/watchdog_event_monitor.hpp:253-257` |
| **Host 错误** | CPU 热跳变（ThermalTrip）、机器检查错误（IERR） | `HostErrorMonitor.Processor.*` 的 `PropertiesChanged` | `include/host_error_event_monitor.hpp:28-29`、`:87-90` |
| **脉冲事件** | Host 开机/关机这类"状态跳变" | `State.Host` 的 `PropertiesChanged` | `include/pulse_event_monitor.hpp:24`、`:100` |

有两条 monitor 是**默认不启用**的，靠编译开关打开（这是 openBMC 常见的可裁剪设计）：

- 阈值告警新信号：`log-alarm` → `-DSEL_LOGGER_MONITOR_THRESHOLD_ALARM_EVENTS`
  （`meson.options:16-20`，`meson.build:22-24`），启动点在 `src/sel_logger.cpp:456-458`。
- Host 错误：`log-host` → `-DSEL_LOGGER_MONITOR_HOST_ERROR_EVENTS`
  （`meson.options:21-25`，`meson.build:25-27`），启动点在 `src/sel_logger.cpp:460-462`。

> 📌 一个值得注意的业务例外：**pulse monitor 其实不算"标准 SEL 记录"**。
> 它在 journal 模式下只发 `MESSAGE` 和 `REDFISH_MESSAGE_ID`（`include/pulse_event_monitor.hpp:90-92`），
> **没有** `MESSAGE_ID=SEL` 鉴别码、**没有** `IPMI_SEL_RECORD_ID`、也不分配 record id。
> 也就是说它记的是"给 Redfish 看的普通日志"，跟前面四类走的是不同通道（对比
> `include/sel_logger.hpp:129-135`）。排查"为什么 ipmitool sel list 看不到 Host 开关机记录"时，
> 这条是头号嫌疑。

---

## 6. 和 phosphor-logging 的同源两视图分工（本节是理解全局的关键）

这是本仓库最容易混、也最能体现 OpenBMC 设计思想的一层。

### 6.1 一个事件，两种"看法"

同一个硬件事件（比如"温度超过 Critical 上限"），在 OpenBMC 里可以同时有两种呈现：

| 视图 | 面向谁 | 关注什么 | 格式 |
|---|---|---|---|
| **SEL 视图** | IPMI 工具链（`ipmitool`） | 3 字节 event data + record id，协议兼容 | journal 的 `IPMI_SEL_*` 字段 |
| **logging 视图** | Web / Redfish / 运维界面 | severity（Critical/Warning/...）+ AdditionalData | `xyz.openbmc_project.Logging.Create` 调用 |

业务上这叫**同源两视图**：事件是同一个，只是给它套了两种不同的"外衣"，服务不同的消费者。

### 6.2 代码里怎么体现：编译期二选一

两种落地路径由编译开关 `send-to-logger` 决定（`meson.options:26-30`，`meson.build:28-32`），
在 `selAddSystemRecord` 里用 `#ifdef` 分成两支（`include/sel_logger.hpp:83` 与 `:125`）：

- **默认（不开）**：`sd_journal_send` 直接写 SEL 字段（`include/sel_logger.hpp:129-135`），
  这就是第 4 节那条链路。
- **开了**：调用 `xyz.openbmc_project.Logging.Create`（`include/sel_logger.hpp:106-123`），
  把 `RECORD_TYPE`、`SENSOR_DATA`、`EVENT_DIR` 等塞进 AdditionalData（`:116-122`），
  并把 severity 从 event data 第一个字节映射出来（`:87-104`）。

### 6.3 severity 映射规则（业务上"这条告警有多严重"）

`include/sel_logger.hpp:84-104` 的规则，翻译成人话：

- 默认 `Informational`（`:84`）；
- 只有 sensor 路径才做严重级映射（`:85` 判断路径含 `sensor`）：
  - `lowerCritGoingLow` / `upperCritGoingHigh` → **Critical**（`0x02` / `0x09`，见枚举 `:57-63`、`:89-92`）；
  - `lowerNonCritGoingLow` / `upperNonCritGoingHigh` → **Warning**（`0x00` / `0x07`，`:93-96`）；
- deassert（告警恢复）一律降为 **Informational**（`:101-104`）——
  业务含义：故障恢复不是故障，不该再报红。

> Java 类比：`selAddSystemRecord` 像一个统一的 `EventPublisher.publish(event)` 门面，
> 底下有两个 `@ConditionalOnProperty` 的实现——`JournalSelDao` 和 `LoggingServiceDao`。
> 编译开关就是 Maven profile，选哪个实现由打包时决定，调用方代码完全不用改。
> 这正是 OpenBMC "一套逻辑、多种部署形态"的松耦合套路。

---

## 7. 3 字节硬限制：为什么这么抠，以及"空洞"是怎么回事

### 7.1 3 字节 / 13 字节的边界

IPMI 规范给标准 SEL record 的 event data 只留了 **3 字节**（`include/sel_logger.hpp:41`），
OEM 记录放宽到 **13 字节**（`:42`）。对应两个入口的校验：

- 标准记录：`include/sel_logger.hpp:76-79`，超限抛 `invalid_argument("Event data too large")`。
- OEM 记录：`src/sel_logger.cpp:348-351`，同样抛异常。

字节内容用大写十六进制字符串编码（`toHexStr`，`src/sel_logger.cpp:331-340`），
写进 journal 的 `IPMI_SEL_DATA` 字段（`include/sel_logger.hpp:134`）。

**业务结论**：传给 `IpmiSelAdd` 的 `selData` 超过 3 字节，D-Bus 调用会失败——
这是协议限制，不是 bug（README.md:38、ONBOARDING.zh-CN.md:96-97 都强调了这点）。

### 7.2 record id 分配与删除后的"空洞"

账本每行有个 2 字节流水号（record id），满到 `0xFFFF` 就返回这个"无效值"表示 SEL 满了
（`selInvalidRecID`，`include/sel_logger.hpp:39-40`）。

删除功能打开时（开关 `sel-delete`，`meson.options:31-35`、`meson.build:33-37`），
id 分配不是无脑递增，而是**有空洞优先复用**：

- 删一条记录时，把它的 id 塞回 `nextRecordsCache`（`src/sel_logger.cpp:243`），
  并追加到备份文件 `/var/log/next_records`（`:245-247`，文件名定义在 `include/sel_logger.hpp:48`）。
- 新增一条时从 cache 里"弹"一个 id 出来（`src/sel_logger.cpp:96-110`，核心是 `:98` 的 `back()` 和 `:104` 的 `pop_back()`）。
- 删除时还特意**保留 Last Add Time、只更新 Last Del Time**（`:248-251`），
  以满足 IPMI 对时间戳语义的要求。

**业务现象**：运维删了几条旧记录后，再产生新事件，新记录的 id 可能是之前删掉的小号码，
`ipmitool sel list` 看到的 id 序列**不连续、甚至会"倒退"**——这是空洞复用，
不是 id 分配错乱。没开删除功能时才是简单递增（`src/sel_logger.cpp:286-292`）。

> ⚠️ 排查"SEL id 为什么重复/不连续"时，先确认 `sel-delete` 开关和 `next_records` 备份文件，
> 别急着当 bug 报。

---

## 8. 和 openUBMC 对照（把你已有经验接上）

| 你熟悉的 openUBMC 思路 | OpenBMC 这里的对应物 | 差异点 |
|---|---|---|
| 传感器告警 → 统一日志服务 | D-Bus 信号 → `sel-logger` 的 monitor → SEL 账本 | 这里额外强调 IPMI 标准格式（3 字节、record id） |
| 通过配置/使能项决定功能开关 | `meson.options` 编译开关（`:1-35`） | 是**编译期**裁剪，不是运行时配置 |
| 告警分级（严重/警告/提示） | `eventReading` → severity 映射（`include/sel_logger.hpp:57-63`、`:87-104`） | 分级依据来自 SEL event data 第一个字节 |
| 一个事件多处消费 | 同源两视图：SEL（ipmitool）+ logging（Redfish） | 两视图由编译开关二选一落地 |
| 日志查询/告警平台 | `ipmitool sel list` 读 `ipmi-daemon` 组装的结果 | 账本在 BMC，主机死了仍可查 |

---

## 9. 常见业务现象速查（现象 → 业务原因 → 代码落点）

| 业务现象 | 可能原因 | 对应代码 |
|---|---|---|
| `ipmitool sel list` 一条都没有 | journal 模式没起来，或 rsyslog 没按 MESSAGE_ID 过滤 | 落盘配置 `rsyslog/phosphor-sel-logger.conf:18-19`、启动 `service_files/xyz.openbmc_project.Logging.IPMI.service:6` |
| 能看到阈值告警，看不到 Host 开关机 | pulse 事件走的是普通 journal，不是标准 SEL | `include/pulse_event_monitor.hpp:90-92` |
| 调用 `IpmiSelAdd` 报 "Event data too large" | selData 超过 3 字节 | `include/sel_logger.hpp:76-78` |
| SEL id 不连续 / 倒退 | 删除后空洞复用 | `src/sel_logger.cpp:243`、`:96-110` |
| 机器重启了但查不到原因 | watchdog 设了 "don't log" 位，主动不记 | `include/watchdog_event_monitor.hpp:214`、`:223` |
| 阈值恢复（deassert）没单独报红 | 业务上恢复不算故障，severity 降为 Informational | `include/sel_logger.hpp:101-104` |
| 告警级别不对 | sensor 路径没含 "sensor"，跳过了映射 | `include/sel_logger.hpp:85` |

---

## 10. Java 类比速查

| 本仓库概念 | Java 类比 | 说明 |
|---|---|---|
| SEL record | `EventLog` 实体（定长字段的 DTO） | 字节布局固定，跨工具可解析 |
| journal 字段（`IPMI_SEL_*`） | 数据库表的列 / SLF4J MDC 字段 | 用字段名当 schema |
| `MESSAGE_ID` | 表鉴别字段 / 事件类型 discriminator | 从日志海里捞出 SEL 记录 |
| `sel-logger` | 事件生产者 + 统一写入门面（DAO） | 只管记账 |
| `ipmi-daemon` | 查询服务 / Repository 消费者 | 只管查账 |
| monitor（`sdbusplus::match`） | `@EventListener` 订阅者 | 信号来了回调 |
| `selAddSystemRecord` | `EventPublisher.publish()` 门面 | 屏蔽两种落地实现 |
| 两种落地路径 | 两个 `@ConditionalOnProperty` 的 DAO 实现 | 编译期选一个 |
| 空洞复用 | 主键回收池 / 空闲 id 队列 | 删除的 id 放回池里复用 |
| `meson.options` 编译开关 | Maven profile | 打包时决定编进哪些功能 |

---

## 11. 一页速记（业务版）

- **它管什么**：把 BMC 上的硬件事件（阈值/watchdog/Host 错误/电源跳变）记进 IPMI SEL 账本。
- **谁用账本**：运维用 `ipmitool sel list` 查；**主机死了也能查**——这是核心价值。
- **账本在哪**：journal 字段（`MESSAGE_ID=b370...` 当鉴别码）+ rsyslog 落盘 `/var/log/ipmi_sel`。
- **它的边界**：只生产、不查询；查账是 ipmi-daemon 的事。
- **同源两视图**：SEL 视图给 ipmitool（3 字节 + id），logging 视图给 Redfish（severity + 附加数据），编译开关二选一。
- **3 字节是规范**：标准记录 3 字节、OEM 13 字节，超了抛异常，不是 bug。
- **id 空洞**：删一条就把 id 放回池子复用，看到不连续/倒退是正常的。
- **一个例外**：pulse（Host 开关机）走普通 journal，不进标准 SEL。
- **可裁剪**：五类 monitor 各有编译开关，默认只开阈值/watchdog/pulse。

---

## 12. 排错速查表（业务视角）

| 现象 / 疑问 | 先查哪里 | 正解 |
|---|---|---|
| "`sel list` 看不到我刚加的事件" | 看该 monitor 是否编译进来（`meson.build:13-27`） | 没编进来就不会监听，先确认开关 |
| "Host 开关机怎么不在 SEL 里" | `include/pulse_event_monitor.hpp:90-92` | 它只发普通 journal，不是标准 SEL |
| "为什么阈值恢复没报红" | `include/sel_logger.hpp:101-104` | deassert 一律 Informational，业务上合理 |
| "id 怎么又小回去了" | `src/sel_logger.cpp:243`、backup 文件 `next_records` | 空洞复用，正常的 |
| "SEL 满了会怎样" | `include/sel_logger.hpp:39-40` | 返回 `0xFFFF`（`selInvalidRecID`），不再分配 |
| "告警级别哪里定义的" | `include/sel_logger.hpp:57-63`、`:87-104` | event data 首字节映射到 severity |
| "改了开关要重新编译吗" | `meson.build:13-37` | 是编译期宏，改了要重新构建镜像 |
| "账本文件在哪" | `include/sel_logger.hpp:45-46`、`rsyslog/phosphor-sel-logger.conf:19` | `/var/log/ipmi_sel` |

---

## 13. 一句话总结

> **sel-logger 是 OpenBMC 里的"硬件事件记账员"：它监听传感器阈值、watchdog、Host 错误等
> D-Bus 信号，把每个事件填成 IPMI 标准的 SEL 记录（3 字节 event data + record id），
> 用 journal 字段当账本落盘，供 `ipmitool sel list` 这类标准工具查询。它和 phosphor-logging
> 是"同源两视图"——同一个事件，既能给 IPMI 工具链看（SEL），也能给 Redfish 看（logging），
> 由编译开关决定落地方式。日志在 BMC，主机死了照样能查，这就是它存在的全部意义。**

> 配套：代码结构看 `ONBOARDING.zh-CN.md`；一条事件如何变成 SEL 记录的完整机制看
> `SEL机制讲解.md`；业务流程可视化看 `业务流程交互图.svg`。
