/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "resolv"

#include "PrivateDnsConfiguration.h"

#include <android-base/format.h>
#include <android-base/logging.h>
#include <android-base/stringprintf.h>
#include <netdutils/Stopwatch.h>
#include <netdutils/ThreadUtil.h>
#include <sys/socket.h>

#include "DnsTlsTransport.h"
#include "Experiments.h"
#include "ResolverEventReporter.h"
#include "netd_resolv/resolv.h"
#include "resolv_cache.h"
#include "resolv_private.h"
#include "util.h"

using aidl::android::net::resolv::aidl::IDnsResolverUnsolicitedEventListener;
using aidl::android::net::resolv::aidl::PrivateDnsValidationEventParcel;
using android::base::StringPrintf;
using android::netdutils::setThreadName;
using android::netdutils::Stopwatch;
using std::chrono::milliseconds;

namespace android {
namespace net {

bool parseServer(const char* server, sockaddr_storage* parsed) {
    addrinfo hints = {
            .ai_flags = AI_NUMERICHOST | AI_NUMERICSERV,
            .ai_family = AF_UNSPEC,
    };
    addrinfo* res;

    int err = getaddrinfo(server, "853", &hints, &res);
    if (err != 0) {
        LOG(WARNING) << "Failed to parse server address (" << server << "): " << gai_strerror(err);
        return false;
    }

    memcpy(parsed, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    return true;
}

// Returns true if the IPrivateDnsServer was created when the mode was opportunistic.
bool isForOpportunisticMode(const PrivateDnsConfiguration::ServerIdentity& identity) {
    return identity.provider.empty();
}

int PrivateDnsConfiguration::set(int32_t netId, uint32_t mark,
                                 const std::vector<std::string>& servers, const std::string& name,
                                 const std::string& caCert) {
    LOG(DEBUG) << "PrivateDnsConfiguration::set(" << netId << ", 0x" << std::hex << mark << std::dec
               << ", " << servers.size() << ", " << name << ")";

    // Parse the list of servers that has been passed in
    PrivateDnsTracker tmp;
    for (const auto& s : servers) {
        sockaddr_storage parsed;
        if (!parseServer(s.c_str(), &parsed)) {
            return -EINVAL;
        }
        auto server = std::make_unique<DnsTlsServer>(parsed);
        server->name = name;
        server->certificate = caCert;
        server->mark = mark;
        tmp[ServerIdentity(*server)] = std::move(server);
    }

    std::lock_guard guard(mPrivateDnsLock);
    if (!name.empty()) {
        mPrivateDnsModes[netId] = PrivateDnsMode::STRICT;
    } else if (!tmp.empty()) {
        mPrivateDnsModes[netId] = PrivateDnsMode::OPPORTUNISTIC;
    } else {
        mPrivateDnsModes[netId] = PrivateDnsMode::OFF;
        mPrivateDnsTransports.erase(netId);
        // TODO: signal validation threads to stop.
        return 0;
    }

    // Create the tracker if it was not present
    auto& tracker = mPrivateDnsTransports[netId];

    // Add the servers if not contained in tracker.
    for (auto& [identity, server] : tmp) {
        if (tracker.find(identity) == tracker.end()) {
            tracker[identity] = std::move(server);
        }
    }

    for (auto& [identity, server] : tracker) {
        const bool active = tmp.find(identity) != tmp.end();
        server->setActive(active);

        // For simplicity, deem the validation result of inactive servers as unreliable.
        if (!server->active() && server->validationState() == Validation::success) {
            updateServerState(identity, Validation::success_but_expired, netId);
        }

        if (needsValidation(*server)) {
            updateServerState(identity, Validation::in_process, netId);
            startValidation(identity, netId, false);
        }
    }

    return 0;
}

PrivateDnsStatus PrivateDnsConfiguration::getStatus(unsigned netId) const {
    PrivateDnsStatus status{PrivateDnsMode::OFF, {}};
    std::lock_guard guard(mPrivateDnsLock);

    const auto mode = mPrivateDnsModes.find(netId);
    if (mode == mPrivateDnsModes.end()) return status;
    status.mode = mode->second;

    const auto netPair = mPrivateDnsTransports.find(netId);
    if (netPair != mPrivateDnsTransports.end()) {
        for (const auto& [_, server] : netPair->second) {
            if (server->isDot() && server->active()) {
                DnsTlsServer& dotServer = *static_cast<DnsTlsServer*>(server.get());
                status.serversMap.emplace(dotServer, server->validationState());
            }
            // TODO: also add DoH server to the map.
        }
    }

    return status;
}

void PrivateDnsConfiguration::clear(unsigned netId) {
    LOG(DEBUG) << "PrivateDnsConfiguration::clear(" << netId << ")";
    std::lock_guard guard(mPrivateDnsLock);
    mPrivateDnsModes.erase(netId);
    mPrivateDnsTransports.erase(netId);
}

base::Result<void> PrivateDnsConfiguration::requestValidation(unsigned netId,
                                                              const ServerIdentity& identity,
                                                              uint32_t mark) {
    std::lock_guard guard(mPrivateDnsLock);

    // Running revalidation requires to mark the server as in_process, which means the server
    // won't be used until the validation passes. It's necessary and safe to run revalidation
    // when in private DNS opportunistic mode, because there's a fallback mechanics even if
    // all of the private DNS servers are in in_process state.
    if (auto it = mPrivateDnsModes.find(netId); it == mPrivateDnsModes.end()) {
        return Errorf("NetId not found in mPrivateDnsModes");
    } else if (it->second != PrivateDnsMode::OPPORTUNISTIC) {
        return Errorf("Private DNS setting is not opportunistic mode");
    }

    auto result = getPrivateDnsLocked(identity, netId);
    if (!result.ok()) {
        return result.error();
    }

    const IPrivateDnsServer* server = result.value();

    if (!server->active()) return Errorf("Server is not active");

    if (server->validationState() != Validation::success) {
        return Errorf("Server validation state mismatched");
    }

    // Don't run the validation if |mark| (from android_net_context.dns_mark) is different.
    // This is to protect validation from running on unexpected marks.
    // Validation should be associated with a mark gotten by system permission.
    if (server->validationMark() != mark) return Errorf("Socket mark mismatched");

    updateServerState(identity, Validation::in_process, netId);
    startValidation(identity, netId, true);
    return {};
}

void PrivateDnsConfiguration::startValidation(const ServerIdentity& identity, unsigned netId,
                                              bool isRevalidation) {
    // This ensures that the thread sends probe at least once in case
    // the server is removed before the thread starts running.
    // TODO: consider moving these code to the thread.
    const auto result = getPrivateDnsLocked(identity, netId);
    if (!result.ok()) return;
    DnsTlsServer server = *static_cast<const DnsTlsServer*>(result.value());

    std::thread validate_thread([this, identity, server, netId, isRevalidation] {
        setThreadName(StringPrintf("TlsVerify_%u", netId).c_str());

        const bool avoidBadPrivateDns =
                Experiments::getInstance()->getFlag("avoid_bad_private_dns", 0);
        const int maxLatency = Experiments::getInstance()->getFlag(
                "max_private_dns_latency_threshold_ms", kMaxPrivateDnsLatencyThresholdMs);
        const int minLatency = Experiments::getInstance()->getFlag(
                "min_private_dns_latency_threshold_ms", kMinPrivateDnsLatencyThresholdMs);
        std::optional<int64_t> latencyThreshold;

        // cat /proc/sys/net/ipv4/tcp_syn_retries yields "6".
        //
        // Start with a 1 minute delay and backoff to once per hour.
        //
        // Assumptions:
        //     [1] Each TLS validation is ~10KB of certs+handshake+payload.
        //     [2] Network typically provision clients with <=4 nameservers.
        //     [3] Average month has 30 days.
        //
        // Each validation pass in a given hour is ~1.2MB of data. And 24
        // such validation passes per day is about ~30MB per month, in the
        // worst case. Otherwise, this will cost ~600 SYNs per month
        // (6 SYNs per ip, 4 ips per validation pass, 24 passes per day).
        auto backoff = mBackoffBuilder.build();

        for (int attempt = 1; /**/; ++attempt) {
            // Because the time between two probes is at least one minute, there might already be
            // some traffic sent to Do53 servers during the time. Update latencyThreshold every
            // time before the probe.
            if (avoidBadPrivateDns && isForOpportunisticMode(identity)) {
                const auto do53Latency = resolv_stats_get_average_response_time(netId, PROTO_UDP);
                const int target = do53Latency.has_value()
                                           ? (3 * do53Latency.value().count() / 1000)
                                           : minLatency;

                // The threshold is limited to the range [minLatency, maxLatency].
                latencyThreshold = std::clamp(target, minLatency, maxLatency);
            }

            // ::validate() is a blocking call that performs network operations.
            // It can take milliseconds to minutes, up to the SYN retry limit.
            LOG(WARNING) << "Validating DnsTlsServer " << server.toIpString() << " with mark 0x"
                         << std::hex << server.validationMark();

            Stopwatch stopwatch;
            const bool gotAnswer = DnsTlsTransport::validate(server, server.validationMark());
            const int32_t timeTaken = saturate_cast<int32_t>(stopwatch.timeTakenUs() / 1000);
            LOG(WARNING) << fmt::format(
                    "validateDnsTlsServer returned {} for {}, took {}ms, attempt {}", gotAnswer,
                    server.toIpString(), timeTaken, attempt);

            // Prevent from endlessly sending traffic on the network in opportunistic mode.
            bool maxAttemptsReached = false;
            if (avoidBadPrivateDns && attempt >= kOpportunisticModeMaxAttempts &&
                isForOpportunisticMode(identity)) {
                maxAttemptsReached = true;
                LOG(WARNING) << "Max attempts reached: " << kOpportunisticModeMaxAttempts;
            }

            const int64_t targetTime = latencyThreshold.value_or(INT64_MAX);
            const bool latencyTooHigh = timeTaken > targetTime;
            if (latencyTooHigh) {
                LOG(WARNING) << "validateDnsTlsServer took too long: threshold is " << targetTime
                             << "ms";
            }

            // TODO: combine these boolean variables into a bitwise variable.
            const bool needs_reeval = this->recordPrivateDnsValidation(
                    identity, netId, gotAnswer, isRevalidation, latencyTooHigh, maxAttemptsReached);

            if (!needs_reeval) {
                break;
            }

            if (backoff.hasNextTimeout()) {
                // TODO: make the thread able to receive signals to shutdown early.
                std::this_thread::sleep_for(backoff.getNextTimeout());
            } else {
                break;
            }
        }

        this->updateServerLatencyThreshold(identity, latencyThreshold, netId);
    });
    validate_thread.detach();
}

void PrivateDnsConfiguration::sendPrivateDnsValidationEvent(const ServerIdentity& identity,
                                                            unsigned netId, bool success) {
    LOG(DEBUG) << "Sending validation " << (success ? "success" : "failure") << " event on netId "
               << netId << " for " << identity.sockaddr.ip().toString() << " with hostname {"
               << identity.provider << "}";
    // Send a validation event to NetdEventListenerService.
    const auto& listeners = ResolverEventReporter::getInstance().getListeners();
    if (listeners.empty()) {
        LOG(ERROR)
                << "Validation event not sent since no INetdEventListener receiver is available.";
    }
    for (const auto& it : listeners) {
        it->onPrivateDnsValidationEvent(netId, identity.sockaddr.ip().toString(), identity.provider,
                                        success);
    }

    // Send a validation event to unsolicited event listeners.
    const auto& unsolEventListeners = ResolverEventReporter::getInstance().getUnsolEventListeners();
    const PrivateDnsValidationEventParcel validationEvent = {
            .netId = static_cast<int32_t>(netId),
            .ipAddress = identity.sockaddr.ip().toString(),
            .hostname = identity.provider,
            .validation = success ? IDnsResolverUnsolicitedEventListener::VALIDATION_RESULT_SUCCESS
                                  : IDnsResolverUnsolicitedEventListener::VALIDATION_RESULT_FAILURE,
    };
    for (const auto& it : unsolEventListeners) {
        it->onPrivateDnsValidationEvent(validationEvent);
    }
}

bool PrivateDnsConfiguration::recordPrivateDnsValidation(const ServerIdentity& identity,
                                                         unsigned netId, bool gotAnswer,
                                                         bool isRevalidation, bool latencyTooHigh,
                                                         bool maxAttemptsReached) {
    constexpr bool NEEDS_REEVALUATION = true;
    constexpr bool DONT_REEVALUATE = false;

    std::lock_guard guard(mPrivateDnsLock);

    auto netPair = mPrivateDnsTransports.find(netId);
    if (netPair == mPrivateDnsTransports.end()) {
        LOG(WARNING) << "netId " << netId << " was erased during private DNS validation";
        notifyValidationStateUpdate(identity.sockaddr, Validation::fail, netId);
        return DONT_REEVALUATE;
    }

    const auto mode = mPrivateDnsModes.find(netId);
    if (mode == mPrivateDnsModes.end()) {
        LOG(WARNING) << "netId " << netId << " has no private DNS validation mode";
        notifyValidationStateUpdate(identity.sockaddr, Validation::fail, netId);
        return DONT_REEVALUATE;
    }

    bool reevaluationStatus = NEEDS_REEVALUATION;
    if (gotAnswer) {
        if (!latencyTooHigh) {
            reevaluationStatus = DONT_REEVALUATE;
        }
    } else if (mode->second == PrivateDnsMode::OFF) {
        reevaluationStatus = DONT_REEVALUATE;
    } else if (mode->second == PrivateDnsMode::OPPORTUNISTIC && !isRevalidation) {
        reevaluationStatus = DONT_REEVALUATE;
    }

    if (maxAttemptsReached) {
        reevaluationStatus = DONT_REEVALUATE;
    }

    bool success = gotAnswer;
    auto& tracker = netPair->second;
    auto serverPair = tracker.find(identity);
    if (serverPair == tracker.end()) {
        LOG(WARNING) << "Server " << identity.sockaddr.ip().toString()
                     << " was removed during private DNS validation";
        success = false;
        reevaluationStatus = DONT_REEVALUATE;
    } else if (!serverPair->second->active()) {
        LOG(WARNING) << "Server " << identity.sockaddr.ip().toString()
                     << " was removed from the configuration";
        success = false;
        reevaluationStatus = DONT_REEVALUATE;
    }

    const bool succeededQuickly = success && !latencyTooHigh;

    // Send private dns validation result to listeners.
    sendPrivateDnsValidationEvent(identity, netId, succeededQuickly);

    if (succeededQuickly) {
        updateServerState(identity, Validation::success, netId);
    } else {
        // Validation failure is expected if a user is on a captive portal.
        // TODO: Trigger a second validation attempt after captive portal login
        // succeeds.
        const auto result = (reevaluationStatus == NEEDS_REEVALUATION) ? Validation::in_process
                                                                       : Validation::fail;
        updateServerState(identity, result, netId);
    }
    LOG(WARNING) << "Validation " << (succeededQuickly ? "success" : "failed");

    return reevaluationStatus;
}

void PrivateDnsConfiguration::updateServerState(const ServerIdentity& identity, Validation state,
                                                uint32_t netId) {
    const auto result = getPrivateDnsLocked(identity, netId);
    if (!result.ok()) {
        notifyValidationStateUpdate(identity.sockaddr, Validation::fail, netId);
        return;
    }

    auto* server = result.value();

    server->setValidationState(state);
    notifyValidationStateUpdate(identity.sockaddr, state, netId);

    RecordEntry record(netId, identity, state);
    mPrivateDnsLog.push(std::move(record));
}

bool PrivateDnsConfiguration::needsValidation(const IPrivateDnsServer& server) const {
    // The server is not expected to be used on the network.
    if (!server.active()) return false;

    // The server is newly added.
    if (server.validationState() == Validation::unknown_server) return true;

    // The server has failed at least one validation attempt. Give it another try.
    if (server.validationState() == Validation::fail) return true;

    // The previous validation result might be unreliable.
    if (server.validationState() == Validation::success_but_expired) return true;

    return false;
}

base::Result<IPrivateDnsServer*> PrivateDnsConfiguration::getPrivateDns(
        const ServerIdentity& identity, unsigned netId) {
    std::lock_guard guard(mPrivateDnsLock);
    return getPrivateDnsLocked(identity, netId);
}

base::Result<IPrivateDnsServer*> PrivateDnsConfiguration::getPrivateDnsLocked(
        const ServerIdentity& identity, unsigned netId) {
    auto netPair = mPrivateDnsTransports.find(netId);
    if (netPair == mPrivateDnsTransports.end()) {
        return Errorf("Failed to get private DNS: netId {} not found", netId);
    }

    auto iter = netPair->second.find(identity);
    if (iter == netPair->second.end()) {
        return Errorf("Failed to get private DNS: server {{{}/{}}} not found", identity.sockaddr,
                      identity.provider);
    }

    return iter->second.get();
}

void PrivateDnsConfiguration::updateServerLatencyThreshold(const ServerIdentity& identity,
                                                           std::optional<int64_t> latencyThreshold,
                                                           uint32_t netId) {
    std::lock_guard guard(mPrivateDnsLock);

    const auto result = getPrivateDnsLocked(identity, netId);
    if (!result.ok()) return;

    if (result.value()->isDot()) {
        DnsTlsServer& server = *static_cast<DnsTlsServer*>(result.value());
        server.setLatencyThreshold(latencyThreshold);
        LOG(INFO) << "Set latencyThreshold "
                  << (latencyThreshold ? std::to_string(latencyThreshold.value()) + "ms"
                                       : "nullopt")
                  << " to " << server.toIpString();
    }
}

void PrivateDnsConfiguration::setObserver(PrivateDnsValidationObserver* observer) {
    std::lock_guard guard(mPrivateDnsLock);
    mObserver = observer;
}

void PrivateDnsConfiguration::notifyValidationStateUpdate(const netdutils::IPSockAddr& sockaddr,
                                                          Validation validation,
                                                          uint32_t netId) const {
    if (mObserver) {
        mObserver->onValidationStateUpdate(sockaddr.ip().toString(), validation, netId);
    }
}

void PrivateDnsConfiguration::dump(netdutils::DumpWriter& dw) const {
    dw.println("PrivateDnsLog:");
    netdutils::ScopedIndent indentStats(dw);

    for (const auto& record : mPrivateDnsLog.copy()) {
        dw.println(fmt::format(
                "{} - netId={} PrivateDns={{{}/{}}} state={}", timestampToString(record.timestamp),
                record.netId, record.serverIdentity.sockaddr.toString(),
                record.serverIdentity.provider, validationStatusToString(record.state)));
    }
    dw.blankline();
}

}  // namespace net
}  // namespace android
