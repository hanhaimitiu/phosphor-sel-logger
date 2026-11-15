# phosphor-sel-logger 代码导读（OpenBMC 新人向）

> 面向刚转入 OpenBMC / openubmc 开发的同学。仓库很小（1 个 .cpp + 若干头文件），
> 适合作为读懂 OpenBMC 守护进程写法的第一个练手仓库。

## 1. 这个仓库是干什么的

把 BMC 上发生的事件转成 **IPMI SEL（System Event Log）记录**：监听 D-Bus 上的
阈值告警、watchdog 超时、Host 报错等信号，为每条事件分配 record id，
以 journal（或 phosphor-logging）条目的形式持久化，同时对外提供
`IpmiSelAdd` / `IpmiSelAddOem` / `Clear` 等 D-Bus 方法供 ipmi-daemon 等调用。

一句话：**SEL 的"生产者"后端 —— ipmitool `sel list` 看到的记录很多来自这里。**

## 2. 目录结构（全部代码就在这）

```
include/
├── sel_logger.hpp                    # selAddSystemRecord / selAddOemRecord 等核心 API
├── threshold_event_monitor.hpp       # 监听阈值告警事件 → SEL
├── threshold_alarm_event_monitor.hpp # 监听 ThresholdAlarmCreated 信号 → SEL
├── watchdog_event_monitor.hpp        # 监听 watchdog 超时 → SEL（sendWatchdogEventLog）
├── host_error_event_monitor.hpp      # 监听 Host 报错事件 → SEL
├── pulse_event_monitor.hpp           # 监听 GPIO 脉冲类事件 → SEL
└── sensorutils.hpp                   # 传感器数值换算小工具
src/
└── sel_logger.cpp                    # main + record id 管理 + 所有 SEL 落地逻辑
```

各 monitor 都是**头文件内联实现**（`startXxxMonitor(conn)` 返回一个
`sdbusplus::match`），事件回调里调 `selAddSystemRecord` 写记录。

## 3. 运行时架构

```
main()  (src/sel_logger.cpp:418)
 ├─ sdbusplus::asio::connection + object_server
 ├─ 注册 D-Bus 接口 xyz.openbmc_project.Logging.IPMI.SEL（ipmiSelAddInterface）
 │    ├─ IpmiSelAdd(message, path, selData, assert, genId) → selAddSystemRecord
 │    ├─ IpmiSelAddOem(message, selData, recordType)      → selAddOemRecord
 │    ├─ Clear()（可选编译开关）                            → clearSelLogFiles()
 │    └─ SELDelete(recordId)（可选编译开关）
 ├─ 按编译开关启动各监控器：
 │    startThresholdAssertMonitor / startThresholdAlarmMonitor /
 │    startWatchdogEventMonitor / startHostErrorEventMonitor / ...
 └─ io.run() 事件循环

事件信号 → monitor 回调 → selAddSystemRecord()
    ├─ getNewRecordId()   # record id 分配；SEL 满返回 0xFFFF（selInvalidRecID）
    └─ 两种落地模式（编译期二选一）：
        SEL_LOGGER_SEND_TO_LOGGING_SERVICE 关闭：
            sd_journal_send("IPMI_SEL_RECORD_ID=...", "IPMI_SEL_RECORD_TYPE=...",
                            "IPMI_SEL_DATA=...", ...)   ← journal 字段方式
        打开：调用 xyz.openbmc_project.Logging 的 Create()，
            把 record type/data 塞进 AdditionalData
```

record id / 删除支持由 meson 编译选项控制：
- `SEL_LOGGER_SEND_TO_LOGGING_SERVICE`：SEL 走 phosphor-logging 而不是独立 journal；
- `SEL_LOGGER_ENABLE_SEL_DELETE`：允许单条删除（维护 `nextRecordsCache` 空洞列表，
  并备份到文件以便重启恢复）。

## 4. 关键实现细节

- **record id 分配**（`getNewRecordId`，src/sel_logger.cpp:118）：递增 + 删除空洞复用；
  删除功能关闭时用最简单的"文件行数 + 1"逻辑（#else 分支）。
- **SEL 时间戳**：`saveClearSelTimestamp` 维护 Last Add/Del Time，满足 IPMI
  `Get SEL Time` 语义；Clear 通过删文件实现。
- **journal 字段约定**：`IPMI_SEL_RECORD_ID / RECORD_TYPE / DATA` ——
  上层（ipmi-daemon）按这些字段从 journal 里把 SEL 记录"捞"出来。
  这是一种典型的 OpenBMC 模式：**用 journal 当数据库，用字段名当表结构。**
- 所有 monitor 都是"match 信号 → 解析参数 → selAddSystemRecord"三步，
  读 `include/threshold_event_monitor.hpp` 即可举一反三。

### 4.1 各监控器监听什么信号（对照读 main() 的启动开关）

| 监控器 | 监听的 D-Bus 信号 | 触发逻辑 |
|---|---|---|
| `threshold_event_monitor.hpp` | `PropertiesChanged`（阈值接口，如 `Sensor.Threshold.*`） | 读新阈值属性值，经 `getScaledIPMIValue`（sensorutils.hpp）做量程换算，生成 IPMI 标准的 eventReading 字节（0x00/0x02/0x07/0x09...） |
| `threshold_alarm_event_monitor.hpp` | `ThresholdAlarmCreated` 信号 | 直接消费告警事件，`generateEvent` 拼记录 |
| `watchdog_event_monitor.hpp` | `xyz.openbmc_project.Watchdog` 接口信号 | 超时后 `sendWatchdogEventLog`，同时发 Redfish 消息 `OpenBMC.0.1.IPMIWatchdog`（watchdog_event_monitor.hpp:231） |
| `host_error_event_monitor.hpp` | `PropertiesChanged` + `arg0namespace='xyz.openbmc_project.State.Host...'`（host_error_event_monitor.hpp:85） | Host 状态对象上报错误时记 SEL |
| `pulse_event_monitor.hpp` | `PropertiesChanged` + `arg0namespace='xyz.openbmc_project.State.Host'` | 检测指定属性的脉冲式跳变 |

所有 match 都是标准的 `sdbusplus::match` 规则字符串——
新人把这一张表当作"D-Bus match 规则怎么写"的现成范例。

### 4.2 selAddSystemRecord 的两种落地与严重级映射（include/sel_logger.hpp:68）

- **journal 模式**（默认）：`sd_journal_send` 带固定 `MESSAGE_ID=b370836...`
  （sel_logger.hpp:35，journalctl 可靠它过滤 SEL）、`IPMI_SEL_RECORD_ID/TYPE/DATA` 字段；
- **logging 服务模式**：调 `xyz.openbmc_project.Logging.Create`，
  severity 由 SEL event data 的第一个字节映射（sel_logger.hpp:57 的 `eventReading` 枚举）：
  lowerCritGoingLow / upperCritGoingHigh → Critical；
  lowerNonCrit / upperNonCrit → Warning；deassert 一律 Informational；
- 标准 SEL 事件数据最多 **3 字节**（`selEvtDataMaxSize`），OEM 记录最多 13 字节，
  超长直接抛 `invalid_argument`——这是 IPMI 规范限制，不是 bug。

### 4.3 record id 的空洞复用（src/sel_logger.cpp:118）

`SEL_LOGGER_ENABLE_SEL_DELETE` 打开时：id 从 `nextRecordsCache`（文件
`/var/log/next_records` 持久化）弹出；有空洞优先复用空洞，否则递增；
SEL 满返回 0xFFFF（`selInvalidRecID`）。删除记录时保留"Last Add Time"不变、
只更新"Last Del Time"（selDeleteTargetRecord，sel_logger.cpp:253）——
满足 IPMI 对时间戳语义的要求。

## 5. D-Bus 接口速查

| 方法 | 说明 |
|---|---|
| `IpmiSelAdd(message, path, selData[byte], assert, genId)` | 加一条标准 SEL |
| `IpmiSelAddOem(message, selData[byte], recordType)` | 加 OEM SEL（自定义 record type） |
| `Clear()` / `SELDelete(id)` | 清空 / 删除（编译开关） |

调试：`ipmitool sel list` / `ipmitool sel elist`，或 journalctl 按字段查。

## 6. 构建与测试

meson，编译选项见 `meson.options`（上面几个 SEL_LOGGER_* 开关都在这）。
本仓库自带单测较少，主要靠 ipmi-daemon 侧联调。

## 7. 新手学习路径

1. `src/sel_logger.cpp` 的 `main()`（418 行起）—— 整个服务的骨架；
2. `selAddSystemRecord` —— 一条 SEL 是怎么写出去的；
3. `include/watchdog_event_monitor.hpp` —— 最简单的 monitor 样例；
4. 对比 `SEL_LOGGER_SEND_TO_LOGGING_SERVICE` 两种模式 —— 理解 SEL 与
   phosphor-logging 的关系（可与本目录 phosphor-logging 文档对照读）。

## 8. 常见改动场景

- **新事件源要记 SEL**：仿照任一 monitor 头文件写一个 `startXxxMonitor`，
  在 `main()` 加编译开关和启动调用，事件回调里调 `selAddSystemRecord`。
- **SEL 记录内容缺失**：确认 journal 字段（RECORD_TYPE/DATA）编码是否与
  ipmi-daemon 的解析约定一致 —— 两仓库是配对的。
