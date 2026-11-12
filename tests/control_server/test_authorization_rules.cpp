// SPDX-License-Identifier: MIT
//
// The server's authorization table against the services in the proto: an RPC added without a
// rule, or a rule left behind for a removed RPC, fails here.
#include "test_harness.hpp"

#include "auth.hpp"
#include "vehicle_sil/control/v1/control.grpc.pb.h"

#include <cstdio>
#include <set>
#include <string>
#include <utility>

namespace {
namespace v1 = vehicle_sil::control::v1;
using namespace vehicle_sil::control_grpc::detail;
} // namespace

TEST_CASE(control_server_authorization_rules_match_the_proto_services)
{
    std::set<std::pair<std::string, std::string>> in_proto;
    const auto* file = v1::PingRequest::descriptor()->file();
    CHECK_EQ(file->service_count(), 1); // ControlService: another service needs its rules here
    for (const auto service : {kControlService})
    {
        const auto* descriptor = file->FindServiceByName(std::string(service));
        CHECK(descriptor != nullptr);
        for (int i = 0; i < descriptor->method_count(); ++i)
        {
            const std::string method(descriptor->method(i)->name());
            in_proto.emplace(std::string(service), method);
            if (find_rule(service, method) == nullptr)
            {
                std::fprintf(stderr, "no authorization rule for %s/%s\n", std::string(service).c_str(), method.c_str());
            }
            CHECK(find_rule(service, method) != nullptr);
        }
    }
    std::set<std::pair<std::string, std::string>> in_table;
    for (const auto& rule : rpc_rules())
    {
        CHECK(in_table.emplace(std::string(rule.service), std::string(rule.method)).second); // no rule twice
        CHECK(in_proto.count({std::string(rule.service), std::string(rule.method)}) == 1);
        CHECK(!rule.what.empty());
    }
    CHECK(in_table == in_proto);
    CHECK(find_rule(kControlService, "NoSuchMethod") == nullptr); // refused for everyone
}
