/*
// Copyright (c) 2018 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
*/
/* // 版权所有 (c) 2018 Intel Corporation
 * //
 * // 本文件受 Apache License, Version 2.0（以下简称 "License"）许可；
 * // 除非符合该 License，否则你不得使用本文件。
 * // 你可以在以下地址获取该 License 的副本：
 * //
 * //      http://www.apache.org/licenses/LICENSE-2.0
 * //
 * // 除非适用法律要求或书面同意，否则软件
 * // 均按 "AS IS"（原样）基础分发，
 * // 不附带任何明示或暗示的保证或条件。
 * // 有关该 License 下的具体权限和限制，
 * // 请参阅该 License。 */
#include <systemd/sd-journal.h>

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/container/flat_map.hpp>
#include <boost/container/flat_set.hpp>
#include <pulse_event_monitor.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sel_logger.hpp>
#include <threshold_event_monitor.hpp>
#include <watchdog_event_monitor.hpp>
#ifdef SEL_LOGGER_ENABLE_SEL_DELETE
#include <xyz/openbmc_project/Common/error.hpp>
#endif
#ifdef SEL_LOGGER_MONITOR_THRESHOLD_ALARM_EVENTS
#include <threshold_alarm_event_monitor.hpp>
#endif
#ifdef SEL_LOGGER_MONITOR_HOST_ERROR_EVENTS
#include <host_error_event_monitor.hpp>
#endif

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

#ifndef SEL_LOGGER_SEND_TO_LOGGING_SERVICE
static bool getSELLogFiles(std::vector<std::filesystem::path>& selLogFiles)
{
    // Loop through the directory looking for ipmi_sel log files
    // 遍历目录，查找 ipmi_sel 日志文件
    for (const std::filesystem::directory_entry& dirEnt :
         std::filesystem::directory_iterator(selLogDir))
    {
        std::string filename = dirEnt.path().filename();
        if (filename.starts_with(selLogFilename))
        {
            // If we find an ipmi_sel log file, save the path
            // 如果找到 ipmi_sel 日志文件，保存其路径
            selLogFiles.emplace_back(selLogDir / filename);
        }
    }
    // As the log files rotate, they are appended with a ".#" that is higher for
    // the older logs. Since we don't expect more than 10 log files, we
    // can just sort the list to get them in order from newest to oldest
    // 日志文件轮转时会被追加一个 ".#"，日志越旧该数字越大。
    // 由于预计日志文件不会超过 10 个，
    // 我们只需对列表排序即可得到从新到旧的顺序。
    std::sort(selLogFiles.begin(), selLogFiles.end());

    return !selLogFiles.empty();
}

static void saveClearSelTimestamp()
{
    int fd = open("/var/lib/ipmi/sel_erase_time",
                  O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0)
    {
        std::cerr << "Failed to open file\n";
        return;
    }

    if (futimens(fd, NULL) < 0)
    {
        std::cerr << "Failed to update SEL cleared timestamp: "
                  << std::string(strerror(errno));
    }
    close(fd);
}

#ifdef SEL_LOGGER_ENABLE_SEL_DELETE
std::vector<uint16_t> nextRecordsCache;

static void backupCacheToFile()
{
    std::ofstream nextRecordStream(selLogDir / nextRecordFilename);
    for (const auto& recordIds : nextRecordsCache)
    {
        nextRecordStream << recordIds << '\n';
    }
}

uint16_t getNewRecordId()
{
    uint16_t nextRecordId = nextRecordsCache.back();
    // Check if SEL is full
    // 检查 SEL 是否已满
    if (nextRecordId == selInvalidRecID)
    {
        return nextRecordId;
    }
    nextRecordsCache.pop_back();
    if (nextRecordsCache.empty())
    {
        nextRecordsCache.push_back(nextRecordId + 1);
    }
    backupCacheToFile();
    return nextRecordId;
}

static void initializeRecordId()
{
    std::ifstream nextRecordStream(selLogDir / nextRecordFilename);
    if (!nextRecordStream.is_open())
    {
        std::ofstream newStream(selLogDir / nextRecordFilename);
        newStream << '1' << '\n';
        newStream.close();
        nextRecordStream.open(selLogDir / nextRecordFilename);
    }
    std::string line;
    while (std::getline(nextRecordStream, line))
    {
        nextRecordsCache.push_back(std::stoi(line));
    }
}

void clearSelLogFiles()
{
    saveClearSelTimestamp();

    // Clear the SEL by deleting the log files
    // 通过删除日志文件来清空 SEL
    std::vector<std::filesystem::path> selLogFiles;
    if (getSELLogFiles(selLogFiles))
    {
        for (const std::filesystem::path& file : selLogFiles)
        {
            std::error_code ec;
            std::filesystem::remove(file, ec);
        }
    }
    // Reload rsyslog so it knows to start new log files
    // 重新加载 rsyslog，使其知道要开始新的日志文件
    boost::asio::io_context io;
    auto dbus = std::make_shared<sdbusplus::asio::connection>(io);
    sdbusplus::message_t rsyslogReload = dbus->new_method_call(
        "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
        "org.freedesktop.systemd1.Manager", "ReloadUnit");
    rsyslogReload.append("rsyslog.service", "replace");
    try
    {
        sdbusplus::message_t reloadResponse = dbus->call(rsyslogReload);
    }
    catch (const sdbusplus::exception_t& e)
    {
        std::cerr << e.what() << "\n";
    }
    // Set next record to 1
    // 将下一条记录设为 1
    nextRecordsCache.clear();
    nextRecordsCache.push_back(1);
    // Update backup file as well
    // 同时更新备份文件
    std::ofstream nextRecordStream(selLogDir / nextRecordFilename);
    nextRecordStream << '1' << '\n';
}

static bool selDeleteTargetRecord(const uint16_t& targetId)
{
    bool targetEntryFound = false;
    // Check if the ipmi_sel exist and save the path
    // 检查 ipmi_sel 是否存在并保存路径
    std::vector<std::filesystem::path> selLogFiles;
    if (!getSELLogFiles(selLogFiles))
    {
        return targetEntryFound;
    }

    // Go over all the ipmi_sel files to remove the entry with the target ID
    // 遍历所有 ipmi_sel 文件，删除具有目标 ID 的条目
    for (const std::filesystem::path& file : selLogFiles)
    {
        std::fstream logStream(file, std::ios::in);
        std::fstream tempFile(selLogDir / "temp", std::ios::out);
        if (!logStream.is_open())
        {
            return targetEntryFound;
        }
        std::string line;
        while (std::getline(logStream, line))
        {
            // Get the recordId of the current entry
            // 获取当前条目的 recordId
            int left = line.find(" ");
            int right = line.find(",");
            int recordLen = right - left;
            std::string recordId = line.substr(left, recordLen);
            int newRecordId = std::stoi(recordId);

            if (newRecordId != targetId)
            {
                // Copy the entry from the original ipmi_sel to the temp file
                // 将条目从原始 ipmi_sel 复制到临时文件
                tempFile << line << '\n';
            }
            else
            {
                // Skip copying the target entry
                // 跳过复制目标条目
                targetEntryFound = true;
            }
        }
        logStream.close();
        tempFile.close();
        if (targetEntryFound)
        {
            std::fstream logStream(file, std::ios::out);
            std::fstream tempFile(selLogDir / "temp", std::ios::in);
            while (std::getline(tempFile, line))
            {
                logStream << line << '\n';
            }
            logStream.close();
            tempFile.close();
            std::error_code ec;
            if (!std::filesystem::remove(selLogDir / "temp", ec))
            {
                std::cerr << ec.message() << std::endl;
            }
            break;
        }
    }
    return targetEntryFound;
}

static void selDeleteRecord(const uint16_t& recordId)
{
    std::filesystem::file_time_type prevAddTime =
        std::filesystem::last_write_time(selLogDir / selLogFilename);
    bool targetEntryFound = selDeleteTargetRecord(recordId);

    // Check if the Record Id was found
    // 检查是否找到了 Record Id
    if (!targetEntryFound)
    {
        throw sdbusplus::xyz::openbmc_project::Common::Error::
            ResourceNotFound();
    }
    // Add to next record cache for reuse
    // 加入下一条记录缓存以供复用
    nextRecordsCache.push_back(recordId);
    // Add to backup file
    // 加入备份文件
    std::ofstream nextRecordStream(selLogDir / nextRecordFilename,
                                   std::ios::app);
    nextRecordStream << recordId << '\n';
    // Keep Last Add Time the same
    // 保持 Last Add Time（上次添加时间）不变
    std::filesystem::last_write_time(selLogDir / selLogFilename, prevAddTime);
    // Update Last Del Time
    // 更新 Last Del Time（上次删除时间）
    saveClearSelTimestamp();
}
#else
static unsigned int initializeRecordId()
{
    std::vector<std::filesystem::path> selLogFiles;
    if (!getSELLogFiles(selLogFiles))
    {
        return 0;
    }
    std::ifstream logStream(selLogFiles.front());
    if (!logStream.is_open())
    {
        return 0;
    }
    std::string line;
    std::string newestEntry;
    while (std::getline(logStream, line))
    {
        newestEntry = line;
    }

    std::vector<std::string> newestEntryFields;
    boost::split(newestEntryFields, newestEntry, boost::is_any_of(" ,"),
                 boost::token_compress_on);
    if (newestEntryFields.size() < 4)
    {
        return 0;
    }

    return std::stoul(newestEntryFields[1]);
}

static unsigned int recordId = initializeRecordId();

unsigned int getNewRecordId()
{
    if (++recordId >= selInvalidRecID)
    {
        recordId = selInvalidRecID;
    }
    return recordId;
}

void clearSelLogFiles()
{
    saveClearSelTimestamp();

    // Clear the SEL by deleting the log files
    // 通过删除日志文件来清空 SEL
    std::vector<std::filesystem::path> selLogFiles;
    if (getSELLogFiles(selLogFiles))
    {
        for (const std::filesystem::path& file : selLogFiles)
        {
            std::error_code ec;
            std::filesystem::remove(file, ec);
        }
    }

    recordId = 0;

    // Reload rsyslog so it knows to start new log files
    // 重新加载 rsyslog，使其知道要开始新的日志文件
    boost::asio::io_context io;
    auto dbus = std::make_shared<sdbusplus::asio::connection>(io);
    sdbusplus::message_t rsyslogReload = dbus->new_method_call(
        "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
        "org.freedesktop.systemd1.Manager", "ReloadUnit");
    rsyslogReload.append("rsyslog.service", "replace");
    try
    {
        sdbusplus::message_t reloadResponse = dbus->call(rsyslogReload);
    }
    catch (const sdbusplus::exception_t& e)
    {
        std::cerr << e.what() << "\n";
    }
}
#endif
#endif

void toHexStr(const std::vector<uint8_t>& data, std::string& hexStr)
{
    std::stringstream stream;
    stream << std::hex << std::uppercase << std::setfill('0');
    for (int v : data)
    {
        stream << std::setw(2) << v;
    }
    hexStr = stream.str();
}

static uint16_t selAddOemRecord(
    [[maybe_unused]] std::shared_ptr<sdbusplus::asio::connection> conn,
    [[maybe_unused]] const std::string& message,
    const std::vector<uint8_t>& selData, const uint8_t& recordType)
{
    // A maximum of 13 bytes of SEL event data are allowed in an OEM record
    // OEM 记录中最多允许 13 字节的 SEL 事件数据
    if (selData.size() > selOemDataMaxSize)
    {
        throw std::invalid_argument("Event data too large");
    }
    std::string selDataStr;
    toHexStr(selData, selDataStr);

#ifdef SEL_LOGGER_SEND_TO_LOGGING_SERVICE
    sdbusplus::message_t AddToLog = conn->new_method_call(
        "xyz.openbmc_project.Logging", "/xyz/openbmc_project/logging",
        "xyz.openbmc_project.Logging.Create", "Create");

    std::string journalMsg(
        message + ": " + " RecordType=" + std::to_string(recordType) +
        ", GeneratorID=" + std::to_string(0) +
        ", EventDir=" + std::to_string(0) + ", EventData=" + selDataStr);

    try
    {
        AddToLog.append(
            journalMsg, "xyz.openbmc_project.Logging.Entry.Level.Informational",
            std::map<std::string, std::string>(
                {{"SENSOR_PATH", ""},
                 {"GENERATOR_ID", std::to_string(0)},
                 {"RECORD_TYPE", std::to_string(recordType)},
                 {"EVENT_DIR", std::to_string(0)},
                 {"SENSOR_DATA", selDataStr}}));
        conn->call(AddToLog);
    }
    catch (const std::exception& e)
    {
        std::cerr << "Failed adding this event: " << e.what() << "\n";
    }
    return 0;
#else
    unsigned int recordId = getNewRecordId();
    if (recordId < selInvalidRecID)
    {
        sd_journal_send("MESSAGE=%s", message.c_str(), "PRIORITY=%i",
                        selPriority, "MESSAGE_ID=%s", selMessageId,
                        "IPMI_SEL_RECORD_ID=%d", recordId,
                        "IPMI_SEL_RECORD_TYPE=%x", recordType,
                        "IPMI_SEL_DATA=%s", selDataStr.c_str(), NULL);
    }
    return recordId;
#endif
}

int main(int, char*[])
{
#ifndef SEL_LOGGER_SEND_TO_LOGGING_SERVICE
#ifdef SEL_LOGGER_ENABLE_SEL_DELETE
    initializeRecordId();
#endif
#endif
    // setup connection to dbus
    // 建立到 dbus 的连接
    boost::asio::io_context io;
    auto conn = std::make_shared<sdbusplus::asio::connection>(io);

    // IPMI SEL Object
    // IPMI SEL 对象
    conn->request_name(ipmiSelObject);
    auto server = sdbusplus::asio::object_server(conn);

    // Add SEL Interface
    // 添加 SEL 接口
    std::shared_ptr<sdbusplus::asio::dbus_interface> ifaceAddSel =
        server.add_interface(ipmiSelPath, ipmiSelAddInterface);

    // Add a new SEL entry
    // 添加一条新的 SEL 条目
    ifaceAddSel->register_method(
        "IpmiSelAdd",
        [conn](const std::string& message, const std::string& path,
               const std::vector<uint8_t>& selData, const bool& assert,
               const uint16_t& genId) {
            return selAddSystemRecord(conn, message, path, selData, assert,
                                      genId);
        });
    // Add a new OEM SEL entry
    // 添加一条新的 OEM SEL 条目
    ifaceAddSel->register_method(
        "IpmiSelAddOem",
        [conn](const std::string& message, const std::vector<uint8_t>& selData,
               const uint8_t& recordType) {
            return selAddOemRecord(conn, message, selData, recordType);
        });

#ifndef SEL_LOGGER_SEND_TO_LOGGING_SERVICE
    // Clear SEL entries
    // 清空 SEL 条目
    ifaceAddSel->register_method("Clear", []() { clearSelLogFiles(); });
#ifdef SEL_LOGGER_ENABLE_SEL_DELETE
    // Delete a SEL entry
    // 删除一条 SEL 条目
    ifaceAddSel->register_method("SELDelete", [](const uint16_t& recordId) {
        return selDeleteRecord(recordId);
    });
#endif
#endif
    ifaceAddSel->initialize();

#ifdef SEL_LOGGER_MONITOR_THRESHOLD_EVENTS
    sdbusplus::match thresholdAssertMonitor = startThresholdAssertMonitor(conn);
#endif

#ifdef REDFISH_LOG_MONITOR_PULSE_EVENTS
    sdbusplus::match pulseEventMonitor = startPulseEventMonitor(conn);
#endif

#ifdef SEL_LOGGER_MONITOR_WATCHDOG_EVENTS
    sdbusplus::match watchdogEventMonitor = startWatchdogEventMonitor(conn);
#endif

#ifdef SEL_LOGGER_MONITOR_THRESHOLD_ALARM_EVENTS
    startThresholdAlarmMonitor(conn);
#endif

#ifdef SEL_LOGGER_MONITOR_HOST_ERROR_EVENTS
    startHostErrorEventMonitor(conn);
#endif
    io.run();

    return 0;
}
