/*
// Copyright (c) 2019 Intel Corporation
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
/* // 版权所有 (c) 2019 Intel Corporation
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
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/bus/match.hpp>
#include <sel_logger.hpp>
#include <sensorutils.hpp>

inline static sdbusplus::match startPulseEventMonitor(
    std::shared_ptr<sdbusplus::asio::connection> conn)
{
    auto pulseEventMatcherCallback = [conn](sdbusplus::message_t& msg) {
        std::string thresholdInterface;
        boost::container::flat_map<std::string, std::variant<std::string>>
            propertiesChanged;
        msg.read(thresholdInterface, propertiesChanged);
        std::string objPath = msg.get_path();

        if (propertiesChanged.empty())
        {
            return;
        }

        std::string event = propertiesChanged.begin()->first;

        auto variant =
            std::get_if<std::string>(&propertiesChanged.begin()->second);

        if (event.empty() || nullptr == variant)
        {
            return;
        }

        if (event == "CurrentHostState")
        {
            std::string journalMsg = "Host";
            std::string redfishMsgId;
            std::string_view hostObjPathPrefix =
                "/xyz/openbmc_project/state/host";

            if (objPath.starts_with(hostObjPathPrefix))
            {
                journalMsg += objPath.erase(0, hostObjPathPrefix.size());
            }

            if (*variant == "xyz.openbmc_project.State.Host.HostState.Off")
            {
                journalMsg += " state is off";
                redfishMsgId = "OpenBMC.0.1.DCPowerOff";
            }
            else if (*variant ==
                     "xyz.openbmc_project.State.Host.HostState.Running")
            {
                journalMsg += " state is on";
                redfishMsgId = "OpenBMC.0.1.DCPowerOn";
            }
            else
            {
                return;
            }
#ifdef SEL_LOGGER_SEND_TO_LOGGING_SERVICE
            sdbusplus::message_t newLogEntry = conn->new_method_call(
                "xyz.openbmc_project.Logging", "/xyz/openbmc_project/logging",
                "xyz.openbmc_project.Logging.Create", "Create");
            const std::string logLevel =
                "xyz.openbmc_project.Logging.Entry.Level.Informational";
            const std::string hostPathName = "HOST_PATH";
            const std::string hostPath = msg.get_path();
            newLogEntry.append(
                std::move(journalMsg), std::move(logLevel),
                std::map<std::string, std::string>(
                    {{std::move(hostPathName), std::move(hostPath)}}));
            conn->call(newLogEntry);
#else
            sd_journal_send("MESSAGE=%s", journalMsg.c_str(),
                            "REDFISH_MESSAGE_ID=%s", redfishMsgId.c_str(),
                            NULL);
#endif
        }
    };

    sdbusplus::match pulseEventMatcher(
        static_cast<sdbusplus::bus_t&>(*conn),
        "type='signal',interface='org.freedesktop.DBus.Properties',member='"
        "PropertiesChanged',arg0namespace='xyz.openbmc_project.State.Host'",
        std::move(pulseEventMatcherCallback));

    return pulseEventMatcher;
}
