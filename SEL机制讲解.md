# SEL 机制讲解：一个硬件事件是怎么变成一条 SEL 记录的

> 面向 Java 背景、7 年没碰 C++、做过 openUBMC、正在学 OpenBMC 的你。
> 业务背景看 `SEL业务详解.md`，代码结构看 `ONBOARDING.zh-CN.md`；
> 本文只追**一条主线**：一个硬件事件从 D-Bus 信号出发，怎么一步步变成 `ipmitool sel list`
> 里的一行。每个环节都带 `文件:行号`，可跳转核对。
>
> 📌 **路径说明**：`src/sel_logger.cpp` 是主程序（main + record id + 落地逻辑），
> `include/sel_logger.hpp` 是核心 API（3 字节校验 + 两种落地），`include/*_monitor.hpp`
> 是五类内联 monitor，`meson.build`/`meson.options` 是编译开关，
> `rsyslog/phosphor-sel-logger.conf` 是落盘模板。
> ⚠️ 所有行号按当前代码 sed 核对过；`ONBOARDING.zh-CN.md` 与 SVG 里的
> `main:418`、`getNewRecordId:118`、`selDeleteTargetRecord:253` 是旧值，
> 当前为 `src/sel_logger.cpp:396`、`:96`、`:167`，本文以当前代码为准。

---

## 0. 主线一句话

> **信号 → monitor 回调解析 → 拼 3 字节 event data → `selAddSystemRecord`
> 校验长度并转十六进制 → `getNewRecordId` 分配流水号 → 二选一落地
> （写 journal 字段 / 调 logging 服务）→ rsyslog 落盘 → ipmi-daemon 读出。**

本文就沿这条线，把它拆成 8 站讲清楚。

---

## 1. 先走一遍真实例子：一次"CPU 温度越上限"是怎么被记下来的

先给你一条能想象的链路，后面每一站都对应其中的一步（行号先混个脸熟，第 3~9 节展开）：

```
① 温度传感器越过 Critical High 红线
   → 传感器服务发 D-Bus 信号 member='ThresholdAsserted'
     匹配规则写在 include/threshold_event_monitor.hpp:339

② sel-logger 的 monitor 回调被触发
   → 读信号参数(sensorName, interface, event, assert, value)
     include/threshold_event_monitor.hpp:59
   → 去重：同一个(路径,事件)的 assert 只记一次  :75 / :85

③ 拼 3 字节 event data（这是 SEL 的核心载荷）
   → eventData[0] = "Upper Critical going high" = 0x09  :94-113 + 枚举 :27-33
   → eventData[0] |= 0x50 表示 byte2/byte3 是触发读数  :115
   → eventData[1] = 换算后的当前读数  :152（算法在 sensorutils.hpp:327）
   → eventData[2] = 换算后的阈值      :198

④ 调统一入口 selAddSystemRecord
   → 入参：message、path、eventData、assert、genId
     include/threshold_event_monitor.hpp:330-334
   → 入口实现在 include/sel_logger.hpp:68-139

⑤ 入口内部：长度校验 + 转十六进制
   → 3 字节校验  include/sel_logger.hpp:76-79
   → toHexStr    :81（实现 src/sel_logger.cpp:331-340）

⑥ 分配 record id
   → getNewRecordId()  include/sel_logger.hpp:126 → 实现 src/sel_logger.cpp:96 / :286

⑦ 落地（编译期二选一）
   → 默认：sd_journal_send 写 IPMI_SEL_* 字段  include/sel_logger.hpp:129-135
   → 可选：调 Logging.Create                    :106-123

⑧ rsyslog 按 MESSAGE_ID 过滤，落盘 /var/log/ipmi_sel
   → rsyslog/phosphor-sel-logger.conf:18-19
   → ipmitool sel list 最终读到这一行
```

---

## 2. 总览图：整条主线的 ASCII 流程图

```
                 硬件/固件世界
   ┌──────────┬──────────┬──────────┬──────────┐
   │ 传感器    │ watchdog │ Host状态 │ 电源/脉冲 │
   └────┬─────┴────┬─────┴────┬─────┴────┬─────┘
        │ D-Bus 信号（ThresholdAsserted / Timeout / PropertiesChanged）
        ▼
┌─────────────────────────────────────────────────────────────┐
│ ① monitor 层（include/*_event_monitor.hpp，头文件内联）        │
│    sdbusplus::match 匹配信号 → 回调解析参数                    │
│    · threshold_event_monitor.hpp:40/339                       │
│    · threshold_alarm_event_monitor.hpp:49/258                 │
│    · watchdog_event_monitor.hpp:240/255                       │
│    · host_error_event_monitor.hpp:32/87                       │
│    · pulse_event_monitor.hpp:24/100                           │
└───────────────────────────┬───────────────────────────────────┘
                            │ 填 std::vector<uint8_t> eventData（≤3 字节）
                            ▼
┌─────────────────────────────────────────────────────────────┐
│ ② 统一入口 selAddSystemRecord   include/sel_logger.hpp:68    │
│    · 长度校验 >3 → 抛 invalid_argument            :76-79     │
│    · toHexStr 转大写 hex 字符串                   :81         │
└───────────────────────────┬───────────────────────────────────┘
                            │
              ┌─────────────┴─────────────┐
              │  #ifdef 编译期二选一        │
              ▼                           ▼
┌────────────────────────────┐  ┌────────────────────────────┐
│ ③A journal 模式（默认）     │  │ ③B logging 模式             │
│ sel_logger.hpp:125-138      │  │ sel_logger.hpp:83-124       │
│ · getNewRecordId()  :126    │  │ · severity 映射     :84-104  │
│ · sd_journal_send   :129    │  │ · 调 Logging.Create :106-123 │
│   MESSAGE_ID=SEL 鉴别码     │  │ · 返回 0（id 交给logging）   │
│   IPMI_SEL_RECORD_ID/TYPE/  │  └────────────────────────────┘
│   GENERATOR_ID/SENSOR_PATH/ │
│   EVENT_DIR/DATA            │
└──────────────┬─────────────┘
               │ rsyslog 按 MESSAGE_ID 过滤落盘
               ▼
   /var/log/ipmi_sel （rsyslog/phosphor-sel-logger.conf:1-20）
               │ ipmi-daemon 读出并组装 IPMI 响应
               ▼
   ipmitool sel list / sel elist  ← 运维看到的一行
```

> 看点：**monitor 只负责"把事件翻译成 3 字节数据"，真正决定写哪里、分配 id 的
> 是第②步的统一入口**。这就是为什么新增一种事件源时，只要写个 monitor 回调调
> `selAddSystemRecord`，不用碰 record id 和落盘逻辑（ONBOARDING.zh-CN.md:131-133）。

---

## 3. 第①站：main 启动骨架——谁把 monitor 挂上去的

一切从 `main`（`src/sel_logger.cpp:396`）开始。它做四件事：

### 3.1 准备 D-Bus 连接（`:403-409`）

```cpp
boost::asio::io_context io;                              // :404 事件循环上下文
auto conn = std::make_shared<sdbusplus::asio::connection>(io);  // :405
conn->request_name(ipmiSelObject);                       // :408 申请服务名
auto server = sdbusplus::asio::object_server(conn);      // :409
```

`ipmiSelObject` 的值在 `include/sel_logger.hpp:28`：
`xyz.openbmc_project.Logging.IPMI`（接口名在 `:30-31`）。

### 3.2 注册"别人喊它记账"的 D-Bus 方法（`:411-442`）

| 方法 | 注册位置 | 实际调用 |
|---|---|---|
| `IpmiSelAdd` | `src/sel_logger.cpp:416-423`（lambda 起于 `:418`） | `selAddSystemRecord`（`:421`） |
| `IpmiSelAddOem` | `:425-430` | `selAddOemRecord`（`:429`） |
| `Clear` | `:434` | `clearSelLogFiles`（`:130` / `:295`） |
| `SELDelete` | `:437-439` | `selDeleteRecord`（`:230`，实现在 `:230-252`） |

最后 `ifaceAddSel->initialize()`（`:442`）让接口正式上线。

### 3.3 按编译开关启动 monitor（`:444-462`）

这是本仓库"事件源可裁剪"的核心。5 个开关、5 个启动点：

| 编译宏 | meson 选项 | 启动调用 | 位置 |
|---|---|---|---|
| `SEL_LOGGER_MONITOR_THRESHOLD_EVENTS` | `log-threshold`（`meson.options:1-5`，`meson.build:13-15`） | `startThresholdAssertMonitor(conn)` | `src/sel_logger.cpp:445` |
| `REDFISH_LOG_MONITOR_PULSE_EVENTS` | `log-pulse`（`meson.options:6-10`，`meson.build:16-18`） | `startPulseEventMonitor(conn)` | `src/sel_logger.cpp:449` |
| `SEL_LOGGER_MONITOR_WATCHDOG_EVENTS` | `log-watchdog`（`meson.options:11-15`，`meson.build:19-21`） | `startWatchdogEventMonitor(conn)` | `src/sel_logger.cpp:453` |
| `SEL_LOGGER_MONITOR_THRESHOLD_ALARM_EVENTS` | `log-alarm`（`meson.options:16-20`，`meson.build:22-24`） | `startThresholdAlarmMonitor(conn)` | `src/sel_logger.cpp:457` |
| `SEL_LOGGER_MONITOR_HOST_ERROR_EVENTS` | `log-host`（`meson.options:21-25`，`meson.build:25-27`） | `startHostErrorEventMonitor(conn)` | `src/sel_logger.cpp:461` |

### 3.4 进入事件循环（`:463`）

`io.run()` 把主线程交给 boost::asio，之后所有工作都由 D-Bus 信号回调驱动。

> Java 类比：`main` 相当于 Spring Boot 的启动类——注册 Bean（D-Bus 方法）、
> 按 profile 装配监听器（monitor）、然后进入事件循环（相当于 servlet 容器常驻）。
> `meson.options` 就是那套 profile 定义。

---

## 4. 第②站：monitor 层——信号怎么变成 3 字节数据

五种 monitor 的共同套路是三步：**match 规则订阅 → 回调解析参数 → 调 `selAddSystemRecord`**。
下面逐个拆，重点看它们各自怎么填那 3 字节。

### 4.1 阈值监控（最复杂：含量程换算）

`include/threshold_event_monitor.hpp`：

- **match 规则**：`member='ThresholdAsserted'`（`:339`），
  `startThresholdAssertMonitor` 在 `:40`，回调 lambda 在 `:43`。
- **解析参数**：`msg.read(sensorName, thresholdInterface, event, assert, assertValue)`（`:59`）。
- **防重复**：用 `assertedEvents` 集合（`:46-47`）做去重——
  assert 时插入，已存在就返回（`:75`）；deassert 时擦除，没擦到就返回（`:85`）。
  业务含义：同一个故障只记一次，恢复也只记一次，别刷屏。
- **填 3 字节**（关键）：
  - `eventData` 先全部初始化成 `0xFF`（"未指定"），`:48-49`；
  - 第 0 字节按越限类型赋值（`:94-113`），取值来自枚举 `thresholdEventOffsets`（`:27-33`）：
    `lowerNonCrit=0x00 / lowerCrit=0x02 / upperNonCrit=0x07 / upperCrit=0x09`；
  - 第 0 字节再 `|= 0x50`（`:115`），置 bit6、bit4，告诉 IPMI"byte2/byte3 是触发读数和阈值"；
  - 第 1 字节 = 当前读数换算值 `getScaledIPMIValue(assertValue, max, min)`（`:152`）；
  - 第 2 字节 = 阈值换算值（`:198`），换算前先乘 `10^Scale`（`:194`）；
  - 换算失败时兜底填 `0xFF`（`:157`、`:203`）。
- **落地**：journal 模式下调 `selAddSystemRecord`（`:330-334`），
  并附带 `REDFISH_MESSAGE_ID` / `REDFISH_MESSAGE_ARGS` 额外字段（`:332-334`）；
  logging 模式下自己调 `Logging.Create` 并按 eventType 定级（`:289-328`）。

### 4.2 新告警信号监控（阈值监控的"孪生版"）

`include/threshold_alarm_event_monitor.hpp`：

- 监听 8 种新信号（`*AlarmAsserted/Deasserted`），matchers 表在 `:39-47`，
  match 规则动态拼 `member=`（`:258`）。
- 回调走 `generateEvent(signalName, ...)`（`:49`），逻辑和 4.1 几乎一样：
  填 `eventData`（`:70-78`）、换算读数与阈值（`:190`、`:227`）、
  最后调 `selAddSystemRecord`（`:244-248`）。
- 区别：它**不做去重的 assertedEvents 集合**（对比 4.1 的 `:75/:85`），
  因为新信号本身自带 assert/deassert 成对语义。

### 4.3 watchdog 监控（3 字节当位域用）

`include/watchdog_event_monitor.hpp`：

- **match 规则**：`interface='xyz.openbmc_project.Watchdog',member='Timeout'`（`:255`），
  启动函数在 `:240`，回调在 `:243-251`。
- **入口函数**：`sendWatchdogEventLog`（`:58`），先造 3 字节 `0xFF` 数组（`:64`）。
- **读状态**：通过 D-Bus `GetAll` 拿 watchdog 属性（`:66-78`），包括：
  - `ExpireAction`（`:90-96`）→ 填第 0 字节（`:99-114`），取值来自枚举 `:27-33`：
    `noAction=0x00 / hardReset=0x01 / powerDown=0x02 / powerCycle=0x03`；
  - `PreTimeoutInterrupt`（`:116-125`）→ 填第 1 字节**高 4 位**（`:126-147`，
    配合 `interruptTypeBits=4`，`:56`）；
  - `CurrentTimerUse`（`:149-156`）→ 填第 1 字节**低 4 位**（`:157-181`），
    取值来自枚举 `watchdogTimerUseOffsets`（`:35-44`）；
  - `Interval`（`:183-188`）只用于生成可读消息，不进 event data。
- **don't log 特例**：通过 IPMI 命令读到"don't log"位（`:214`，`wdtNologBit=1<<7`，`:55`），
  若置位则**整条不记**（`:223` 的条件）。这是排查"watchdog 超时却没 SEL"的关键。
- **落地**：`selAddSystemRecord`（`:233-236`），附带 Redfish 消息
  `OpenBMC.0.1.IPMIWatchdog`（`:231`）。

> 看点：同样的 3 字节，阈值监控当"数值"用，watchdog 当"位域"用——
> 这正是 IPMI SEL 的通用设计：字节含义由 record type / 事件类型决定。

### 4.4 Host 错误监控（最简单：只填 1 字节）

`include/host_error_event_monitor.hpp`：

- 监听两类错误 matchers：`ThermalTrip` 和 `IERR`（`:28-29`），
  match 规则是 `PropertiesChanged` + `arg0namespace='xyz.openbmc_project.HostErrorMonitor.Processor.<类型>'`
  （`:87-90`，启动函数 `:79`）。
- 回调 `hostErrorEventMonitor`（`:32`）：读 `Asserted` 属性（`:48-53`），
  用 `hostErrorEvents` 集合做去重（`:55-68`）。
- 3 字节：`selType = ThermalTrip ? 0x01 : 0x00`（`:73`），
  然后直接 `{selType, 0xff, 0xff}`（`:75`）——后两字节未指定。
- 落地：调 `selAddSystemRecord`（`:76`）。

### 4.5 脉冲监控（例外：不进标准 SEL）

`include/pulse_event_monitor.hpp`：

- match 规则：`PropertiesChanged` + `arg0namespace='xyz.openbmc_project.State.Host'`（`:100`），
  启动函数 `:24`。
- 只处理 `CurrentHostState` 属性（`:49`）：`Off` → Redfish `DCPowerOff`（`:61-65`），
  `Running` → `DCPowerOn`（`:66-71`）。
- **落地是例外**：journal 模式下只发 `MESSAGE` + `REDFISH_MESSAGE_ID`（`:90-92`），
  **没有** `MESSAGE_ID=SEL`、**没有** record id；logging 模式下调 `Logging.Create`（`:76-88`）。
  → 所以它记的是"给 Redfish 看的普通日志"，不会出现在 `ipmitool sel list` 里。

> Java 类比：monitor 就像一组 `@EventListener` 方法，各自监听不同事件类型，
> 把领域事件转换成统一的 `SelEvent(3字节payload)`，再交给同一个 `publish()` 门面。
> pulse 那个是"没走统一门面"的特例。

---

## 5. 第③站：统一入口 selAddSystemRecord 内部发生了什么

入口是模板函数 `selAddSystemRecord`（`include/sel_logger.hpp:68-139`），
签名最后一包 `T&&... metadata`（`:73`）允许调用方追加任意 journal 字段
（比如前面看到的 `REDFISH_MESSAGE_ID`）。内部三步：

### 5.1 长度校验（3 字节硬限制）

```cpp
if (selData.size() > selEvtDataMaxSize)   // :76，selEvtDataMaxSize=3 定义在 :41
{
    throw std::invalid_argument("Event data too large");  // :78
}
```

超过 3 字节直接抛异常——这是 IPMI 规范限制（标准 SEL record 的 event data 字段就是 3 字节）。
OEM 记录放宽到 13 字节（`selOemDataMaxSize=13`，`:42`），在 `selAddOemRecord` 里单独校验
（`src/sel_logger.cpp:348-351`）。

### 5.2 转十六进制字符串

`toHexStr(selData, selDataStr)`（`:81`），实现在 `src/sel_logger.cpp:331-340`：
用 `std::hex << std::uppercase << setfill('0') << setw(2)` 把每字节转成两位大写 hex
（`:334-338`）。比如 `{0x09, 0x5A, 0x55}` → `"095A55"`。

### 5.3 分两支落地（下一节详述）

- `#ifdef SEL_LOGGER_SEND_TO_LOGGING_SERVICE` 分支（`:83-124`）：走 logging 服务；
- `#else` 分支（`:125-138`）：走 journal。

注意一个细节：**journal 分支会返回分配到的 record id（`:137`），
logging 分支返回 0（`:124`）**——因为走了 logging 服务后，id 由 logging 那边管理，
调用方拿不到 SEL 的 2 字节流水号。

---

## 6. 第④站：record id 分配——递增、空洞与 0xFFFF

record id 是 SEL 记录的 2 字节流水号，`selInvalidRecID = 0xFFFF`（`include/sel_logger.hpp:39-40`）
表示"无效/已满"。分配逻辑在 `src/sel_logger.cpp`，分两套实现，由 `SEL_LOGGER_ENABLE_SEL_DELETE`
编译宏（`meson.build:33-37`、`meson.options:31-35`）切换。

### 6.1 关闭删除功能时：简单递增（`:284-293`）

```cpp
static unsigned int recordId = initializeRecordId();  // :284 从现有文件算初始值
unsigned int getNewRecordId()                         // :286
{
    if (++recordId >= selInvalidRecID)                // :288 满到 0xFFFF 就卡住
        recordId = selInvalidRecID;
    return recordId;
}
```

`initializeRecordId()`（`:254-282`）读取最新一条 SEL 文件记录，取它的 id 作为起点（`:281`），
这样进程重启后 id 能接着涨，不会从头覆盖。

### 6.2 打开删除功能时：空洞复用（`:85-128`）——重点

这是最容易看懵的一套。核心数据结构是 `nextRecordsCache`（`:85`，一个 `uint16_t` 向量），
它其实被当作**栈**用：

- **初始化**（`initializeRecordId`，`:113-128`）：从备份文件 `/var/log/next_records`
  （文件名在 `include/sel_logger.hpp:48`）逐行读入 cache（`:124-127`）；
  文件不存在就创建并写入初值 `1`（`:116-122`）。
- **分配**（`getNewRecordId`，`:96-111`）：
  - 取栈顶 `nextRecordId = nextRecordsCache.back()`（`:98`）；
  - 若栈顶已是 `0xFFFF`，说明 SEL 满，直接返回（`:100-103`）；
  - 否则弹出（`:104`）；若栈空了，再压入 `nextRecordId + 1` 作为下一个候选（`:105-108`）；
  - `backupCacheToFile()`（`:109`）把 cache 写回文件，保证重启不丢。
- **删除时回收**：`selDeleteRecord`（`:230-252`）找到目标记录并删掉后，
  把它的 id 压回 cache（`:243`）并追加到备份文件（`:245-247`）。

**结果**：被删的 id 会进入"空洞池"，下次新增优先复用。所以 `ipmitool sel list`
里 id 会不连续、甚至倒退——这是设计如此，不是 bug（详见 `SEL业务详解.md` 第 7.2 节）。

### 6.3 删除/清空的配套动作

- `selDeleteTargetRecord`（`:167-228`）：逐行扫描 `/var/log/ipmi_sel*` 文件
  （文件枚举 `getSELLogFiles`，`:45-64`），把 record id 等于目标的那些行跳过，
  其余写到临时文件再回写（`:187-225`）。它解析行格式时依赖 rsyslog 模板的
  "`<id>,<type>,<data>,...`" 布局（`:190-194` 找空格和逗号）。
- `selDeleteRecord`（`:230-252`）：记录删除前的 Last Add Time（`:232-233`），
  删完把这个时间恢复回去（`:249`），只更新 Last Del Time（`:251`）——
  满足 IPMI 对时间戳语义的要求。
- `clearSelLogFiles`（`:130-165` 或 `:295-327`）：删掉所有 ipmi_sel 文件、
  通过 D-Bus 让 systemd `ReloadUnit rsyslog.service`（`:147-153`）重新打开文件、
  然后把 id 重置为 1（`:160-164`）。
- `saveClearSelTimestamp`（`:66-82`）：维护 `/var/lib/ipmi/sel_erase_time` 的 mtime，
  对应 IPMI 的 Get SEL Time 语义。

> Java 类比：关闭删除时就是一个 `AtomicInteger` 自增；打开删除时是
> `AtomicInteger` + 一个"空闲 id 队列"（删除时 `offer`，新增时 `poll`），
> 并且队列持久化到文件防止重启丢失。

---

## 7. 第⑤站：两种落地路径逐字段对比

编译开关 `send-to-logger`（`meson.options:26-30`、`meson.build:28-32`）决定走哪支。

### 7.1 路径 A：直接写 journal（默认，`#else` 分支）

`include/sel_logger.hpp:125-138` 的核心调用：

```cpp
unsigned int recordId = getNewRecordId();              // :126 先分配 id
if (recordId < selInvalidRecID)                        // :127 没满才写
{
    sd_journal_send(
        "MESSAGE=%s", message.c_str(),                 // :130 可读消息
        "PRIORITY=%i", selPriority,                    // :130 优先级 5(notice)，:36
        "MESSAGE_ID=%s", selMessageId,                 // :131 鉴别码 b370...，:35
        "IPMI_SEL_RECORD_ID=%d", recordId,             // :131
        "IPMI_SEL_RECORD_TYPE=%x", selSystemType,      // :132 系统记录 0x02，:37
        "IPMI_SEL_GENERATOR_ID=%x", genId,             // :133
        "IPMI_SEL_SENSOR_PATH=%s", path.c_str(),       // :133
        "IPMI_SEL_EVENT_DIR=%x", assert,               // :134 assert/deassert
        "IPMI_SEL_DATA=%s", selDataStr.c_str(),        // :134 3 字节 hex
        std::forward<T>(metadata)..., NULL);           // :135 附加字段
}
return recordId;                                       // :137
```

**字段约定表**（README.md:16-26 有英文原版）：

| journal 字段 | 含义 | 来源 |
|---|---|---|
| `MESSAGE_ID` | SEL 鉴别码 `b370836ccf2f4850ac5bee185b77893a` | `include/sel_logger.hpp:35` |
| `IPMI_SEL_RECORD_ID` | 2 字节流水号 | `getNewRecordId()`，`:126` |
| `IPMI_SEL_RECORD_TYPE` | 系统记录 `0x02` | `selSystemType`，`:37` |
| `IPMI_SEL_GENERATOR_ID` | 报案者，BMC = `0x0020` | `:38`，调用方传 `selBMCGenID` |
| `IPMI_SEL_SENSOR_PATH` | 相关传感器的 D-Bus 路径 | 调用方传 `path` |
| `IPMI_SEL_EVENT_DIR` | assert=1 / deassert=0 | 调用方传 `assert` |
| `IPMI_SEL_DATA` | 3 字节事件数据（hex 字符串） | `toHexStr`，`:81` |
| `REDFISH_*` | 可选附加字段 | 模板参数 `metadata`，`:73`/`:135` |

### 7.2 落盘：rsyslog 把字段拍平成一行

`rsyslog/phosphor-sel-logger.conf`：

- `:1-16` 定义模板 `IPMISELTemplate`，按固定顺序拼字段：
  `时间 RECORD_ID,RECORD_TYPE,DATA,GENERATOR_ID,SENSOR_PATH,EVENT_DIR`
  （`:4-14`，字段间用逗号分隔，`:5`/`:7` 等）；
- `:18-19` 判断 `MESSAGE_ID == "b370..."`，命中就写到 `/var/log/ipmi_sel`。

这就解释了第 6.3 节 `selDeleteTargetRecord` 为什么要按"空格 + 逗号"去解析行——
**写端（journal 字段）和读端（rsyslog 模板 + 删除解析）是靠约定对齐的**。

> 📌 该 conf 只在**不开** `send-to-logger` 时才安装（`meson.build:58-63`），
> 因为走 logging 服务后数据不落这个文件了。

### 7.3 路径 B：经 logging 服务（`#ifdef` 分支）

`include/sel_logger.hpp:83-124`：

1. **定 severity**（`:84-104`）：默认 `Informational`；sensor 路径才按
   `selData[0]` 查 `eventReading` 枚举（`:57-63`）定级——`0x02/0x09` → Critical（`:89-92`），
   `0x00/0x07` → Warning（`:93-96`）；deassert 一律 Informational（`:101-104`）。
2. **拼可读消息**：`message + " from " + path + RecordType/GeneratorID/EventDir/EventData`（`:110-114`）。
3. **调 Create 方法**（`:106-108`）：目标
   `xyz.openbmc_project.Logging` / `xyz/openbmc_project/logging`，接口
   `xyz.openbmc_project.Logging.Create`，方法 `Create`。
4. **塞 AdditionalData**（`:116-122`）：`SENSOR_PATH`、`GENERATOR_ID`、
   `RECORD_TYPE`、`EVENT_DIR`、`SENSOR_DATA` 五个键值。
5. 返回 0（`:124`）。

### 7.4 两路径对比表

| 维度 | journal 模式 | logging 模式 |
|---|---|---|
| 编译开关 | 默认（不开 `send-to-logger`） | 开 `send-to-logger` |
| 出口代码 | `include/sel_logger.hpp:125-138` | `:83-124` |
| 写入方式 | `sd_journal_send` | D-Bus `Logging.Create` |
| id 分配 | 自己分配（`:126`） | 不分配，返回 0（`:124`） |
| 严重级 | `PRIORITY=5` 固定 | 按 event data 映射（`:84-104`） |
| 落盘文件 | `/var/log/ipmi_sel`（rsyslog conf:19） | 由 phosphor-logging 管 |
| `ipmitool sel list` 能查到 | ✅（ipmi-daemon 按字段读） | ❌（走 Redfish 日志通道） |
| rsyslog 配置 | 需要安装（`meson.build:58-63`） | 不需要 |

> Java 类比：`selAddSystemRecord` 是门面，底下两个策略实现由编译期决定：
> `JournalSelDao`（自己管 id + 直接写库）和 `LoggingServiceDao`（委托外部服务 + 不管 id）。
> 调用方只认 `publish(event)`，完全感知不到差异。

---

## 8. 第⑥站：OEM 记录的落地路径（旁支，但要认识）

除了标准系统记录，入口 `IpmiSelAddOem`（`src/sel_logger.cpp:425-430`）走的是
`selAddOemRecord`（`:342-394`）：

- 事件数据上限是 **13 字节**（`selOemDataMaxSize=13`，`include/sel_logger.hpp:42`），
  校验在 `src/sel_logger.cpp:348-351`；
- logging 模式：调 Create，把 `RECORD_TYPE`（调用方自定义）、`GENERATOR_ID=0`、
  `EVENT_DIR=0`、`SENSOR_DATA` 塞进 AdditionalData（`:355-381`）；
- journal 模式：`sd_journal_send` 写的字段**更少**（`:386-390`）——
  只有 `MESSAGE/PRIORITY/MESSAGE_ID/RECORD_ID/RECORD_TYPE/DATA`，
  **没有** `GENERATOR_ID`、`SENSOR_PATH`、`EVENT_DIR`（对比 7.1 的字段表）。
  业务含义：OEM 记录把"这 13 字节怎么解释"完全交给了自定义 record type。

---

## 9. 构建与部署：开关怎么变成进程

- **编译产物**：`meson.build:39-48` 定义可执行文件名 `sel-logger`，
  源码只有 `src/sel_logger.cpp` 一个 `.cpp`，头文件目录 `include/`。
- **服务文件**：`service_files/xyz.openbmc_project.Logging.IPMI.service`
  由 `meson.build:50-56` 安装；内容是 `Type=simple`、`Restart=always`、
  `ExecStart=/usr/bin/sel-logger`（`:6`）、`WantedBy=multi-user.target`（`:10`）——
  即 BMC 开机自启、挂了自动拉起。
- **rsyslog 配置**：仅 journal 模式安装（`meson.build:58-63`）。
- **依赖**：`sdbusplus`、`libsystemd`、`boost`（`meson.build:11`）；
  开 logging 时再加 `phosphor-logging`（`:31`），开删除时加 `phosphor-dbus-interfaces`（`:36`）。

> 这就是 OpenBMC 的"配置驱动 + 松耦合"：功能增减靠编译开关，
> 进程间靠 D-Bus 和 journal 字段约定解耦，C++ 代码本身几乎不用改。

---

## 10. Java 类比速查（机制版）

| 本仓库机制 | Java 类比 | 对应位置 |
|---|---|---|
| `sdbusplus::match` + 回调 | `@EventListener` 订阅方法 | 各 `startXxxMonitor` |
| monitor 填 3 字节 | 把领域事件转成统一 DTO | `:48-198` 等 |
| `selAddSystemRecord` 模板门面 | 泛型 `publish(T event, Object... metadata)` | `include/sel_logger.hpp:68` |
| 3 字节校验 | DTO 注解校验（超限抛异常） | `:76-79` |
| `toHexStr` | 序列化器（对象 → hex 字符串） | `src/sel_logger.cpp:331` |
| `getNewRecordId` 递增版 | `AtomicInteger.getAndIncrement()` | `:286` |
| `getNewRecordId` 空洞版 | `AtomicInteger` + 持久化空闲 id 队列 | `:96` |
| `nextRecordsCache` | `Deque` 当栈 + 写文件持久化 | `:85`/`:109` |
| journal 字段 | 消息 header / SLF4J MDC | `include/sel_logger.hpp:129` |
| rsyslog 模板 | `Formatter`/序列化模板 | `rsyslog conf:1` |
| 两种落地 `#ifdef` | 两个 `@ConditionalOnProperty` 策略实现 | `:83`/`:125` |
| `meson.options` | Maven profile 开关 | `meson.options:1-35` |

---

## 11. 排错速查表（机制视角）

| 现象 | 先查哪里 | 正解 |
|---|---|---|
| 新增 monitor 不触发 | 编译开关是否打开 | `meson.build:13-27`，改了要重新构建 |
| `IpmiSelAdd` 报 data too large | 3 字节校验 | `include/sel_logger.hpp:76-79`；OEM 上限 13 字节（`:42`） |
| event data 全是 `0xFF` | 量程换算失败兜底 | `threshold_event_monitor.hpp:157`/`:203`；watchdog 初值 `:64` |
| 阈值告警重复/恢复也记了 | 去重集合逻辑 | `threshold_event_monitor.hpp:75`/`:85` |
| watchdog 超时却没 SEL | don't log 位 | `watchdog_event_monitor.hpp:214`/`:223` |
| 同一故障刷屏很多条 | assert 去重 | `host_error_event_monitor.hpp:55-68` |
| Host 开关机不在 SEL 里 | pulse 走普通 journal | `pulse_event_monitor.hpp:90-92` |
| `ipmitool sel list` 查不到 | 是否走了 logging 模式 | logging 模式不落 ipmi_sel（`meson.build:58-63`、`:124`） |
| SEL id 不连续/倒退 | 空洞复用 | `src/sel_logger.cpp:243`、`:96-111` |
| SEL id 一直是 `0xFFFF` | SEL 已满 | `include/sel_logger.hpp:39-40`、`src/sel_logger.cpp:100`/`:288` |
| 删了记录文件没刷新 | Clear 会 reload rsyslog | `src/sel_logger.cpp:147-153` |
| 重启后 id 从头开始 | 初始化读文件失败 | `initializeRecordId` `:113-128` / `:254-282` |
| severity 不对 | 路径含不含 "sensor"、是否 deassert | `include/sel_logger.hpp:85`、`:101-104` |

---

## 12. 一句话总结

> **整条主线就是：D-Bus 信号 → monitor 把事件翻译成最多 3 字节的 event data
> （阈值当数值、watchdog 当位域）→ `selAddSystemRecord` 校验长度并转 hex →
> `getNewRecordId` 分配流水号（带删除空洞复用）→ 编译期二选一：
> 默认用 `sd_journal_send` 写 `MESSAGE_ID + IPMI_SEL_*` 字段再由 rsyslog 落盘
> `/var/log/ipmi_sel`，或调 `Logging.Create` 转给 phosphor-logging → ipmi-daemon 读出。
> 读懂这条线，5 个 monitor 只是"3 字节怎么填"的变体，新增事件源也只需照抄一个 monitor。**

> 配套：业务背景看 `SEL业务详解.md`；代码结构与目录看 `ONBOARDING.zh-CN.md`；
> 可视化看 `业务流程交互图.svg`。
