/*
 * Copyright (C) 2019 The Android Open Source Project
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
 *
 */

#ifndef DNS_RESPONDER_CLIENT_H
#define DNS_RESPONDER_CLIENT_H

#include <memory>
#include <string>
#include <vector>

#include "android/net/IDnsResolver.h"
#include "android/net/INetd.h"
#include "dns_responder.h"
#include "dns_tls_certificate.h"

inline const std::vector<std::string> kDefaultServers = {"127.0.0.3"};
inline const std::vector<std::string> kDefaultSearchDomains = {"example.com"};
inline const std::vector<int> kDefaultParams = {
        300,      // sample validity in seconds
        25,       // success threshod in percent
        8,    8,  // {MIN,MAX}_SAMPLES
        1000,     // BASE_TIMEOUT_MSEC
        2,        // retry count
};

// TODO: Replace binder service related code to ndk version after finishing all tests migration
// from libbinder to libbinder_ndk.
class DnsResponderClient {
  public:
    struct Mapping {
        std::string host;
        std::string entry;
        std::string ip4;
        std::string ip6;
    };

    virtual ~DnsResponderClient() = default;

    static void SetupMappings(unsigned num_hosts, const std::vector<std::string>& domains,
                              std::vector<Mapping>* mappings);

    // This function is deprecated. Please use SetResolversFromParcel() instead.
    bool SetResolversForNetwork(const std::vector<std::string>& servers = kDefaultServers,
                                const std::vector<std::string>& domains = kDefaultSearchDomains,
                                const std::vector<int>& params = kDefaultParams);

    // This function is deprecated. Please use SetResolversFromParcel() instead.
    bool SetResolversWithTls(const std::vector<std::string>& servers,
                             const std::vector<std::string>& searchDomains,
                             const std::vector<int>& params, const std::string& name) {
        // Pass servers as both network-assigned and TLS servers.  Tests can
        // determine on which server and by which protocol queries arrived.
        return SetResolversWithTls(servers, searchDomains, params, servers, name);
    }

    // This function is deprecated. Please use SetResolversFromParcel() instead.
    bool SetResolversWithTls(const std::vector<std::string>& servers,
                             const std::vector<std::string>& searchDomains,
                             const std::vector<int>& params,
                             const std::vector<std::string>& tlsServers, const std::string& name);

    bool SetResolversFromParcel(const android::net::ResolverParamsParcel& resolverParams);

    // Return a default resolver configuration for opportunistic mode.
    static android::net::ResolverParamsParcel GetDefaultResolverParamsParcel();

    static void SetupDNSServers(unsigned numServers, const std::vector<Mapping>& mappings,
                                std::vector<std::unique_ptr<test::DNSResponder>>* dns,
                                std::vector<std::string>* servers);

    int SetupOemNetwork();

    void TearDownOemNetwork(int oemNetId);

    virtual void SetUp();
    virtual void TearDown();

    android::net::IDnsResolver* resolvService() const { return mDnsResolvSrv.get(); }
    android::net::INetd* netdService() const { return mNetdSrv.get(); }

  private:
    android::sp<android::net::INetd> mNetdSrv = nullptr;
    android::sp<android::net::IDnsResolver> mDnsResolvSrv = nullptr;
    int mOemNetId = -1;
};

#endif  // DNS_RESPONDER_CLIENT_H