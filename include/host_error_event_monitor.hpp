/*
// Copyright (c) 2022 Intel Corporation
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
/* // 版权所有 (c) 2022 Intel Corporation
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

#pragma once
#include <boost/container/flat_map.hpp>
#include <boost/container/flat_set.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sel_logger.hpp>
#include <sensorutils.hpp>

using sdbusMatch = std::shared_ptr<sdbusplus::match>;
static sdbusMatch thermTripEventMatcher;
static sdbusMatch ierrEventMatcher;

static boost::container::flat_map<std::string, sdbusMatch> hostErrorMatches = {
    {"ThermalTrip", thermTripEventMatcher}, {"IERR", ierrEventMatcher}};
static boost::container::flat_set<std::string> hostErrorEvents;

void hostErrorEventMonitor(std::shared_ptr<sdbusplus::asio::connection> conn,
                           sdbusplus::message_t& msg)
{
    std::string msgInterface;
    boost::container::flat_map<std::string, std::variant<bool>> values;
    try
    {
        msg.read(msgInterface, values);
    }
    catch (const sdbusplus::exception_t& ec)
    {
        std::cerr << "error getting asserted value from " << msg.get_path()
                  << " ec= " << ec.what() << "\n";
        return;
    }
    std::string objectPath = msg.get_path();
    auto findState = values.find("Asserted");
    if (values.empty() || findState == values.end())
    {
        return;
    }
    bool assert = std::get<bool>(findState->second);
    // Check if the log should be recorded.
    // 检查是否应记录该日志。
    if (assert)
    {
        if (hostErrorEvents.insert(objectPath).second == false)
        {
            return;
        }
    }
    else
    {
        if (hostErrorEvents.erase(objectPath) == 0)
        {
            return;
        }
    }
    std::string eventName = objectPath.substr(objectPath.find_last_of('/') + 1,
                                              objectPath.length());
    std::string message =
        (assert) ? eventName + " Asserted" : eventName + " De-Asserted";
    uint8_t selType = (msgInterface.ends_with("ThermalTrip")) ? 0x01 : 0x00;

    std::vector<uint8_t> selData{selType, 0xff, 0xff};
    selAddSystemRecord(conn, message, objectPath, selData, assert, selBMCGenID);
}

inline static void startHostErrorEventMonitor(
    std::shared_ptr<sdbusplus::asio::connection> conn)
{
    for (auto iter = hostErrorMatches.begin(); iter != hostErrorMatches.end();
         iter++)
    {
        iter->second = std::make_shared<sdbusplus::match>(
            static_cast<sdbusplus::bus_t&>(*conn),
            "type='signal',interface='org.freedesktop.DBus.Properties',member='"
            "PropertiesChanged',arg0namespace='xyz.openbmc_project."
            "HostErrorMonitor.Processor." +
                iter->first + "'",
            [conn, iter](sdbusplus::message_t& msg) {
                hostErrorEventMonitor(conn, msg);
            });
    }
}
