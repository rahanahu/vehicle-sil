// SPDX-License-Identifier: MIT
#include "auth.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace vehicle_sil::control_grpc {
namespace detail {

namespace {

/// Compares in time independent of where the strings differ.
bool same_secret(const std::string& a, const std::string& b)
{
    unsigned diff = a.size() == b.size() ? 0U : 1U;
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        diff |= static_cast<unsigned>(static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(i < b.size() ? b[i] : 0));
    }
    return diff == 0;
}

} // namespace

bool allows(const Who& who, Role need)
{
    if (who.anyone)
    {
        return true;
    }
    const Role have = who.role;
    if (need == Role::Agent || have == Role::Agent)
    {
        return have == need;
    }
    return static_cast<int>(have) >= static_cast<int>(need);
}

std::optional<Who> Authorizer::identify(const grpc::ServerContextBase& context) const
{
    if (security_.credentials.empty())
    {
        return Who{};
    }
    const auto& metadata = context.client_metadata();
    const auto it = metadata.find("authorization");
    if (it == metadata.end())
    {
        return std::nullopt;
    }
    const std::string value(it->second.data(), it->second.size());
    const std::string prefix = "bearer ";
    if (value.size() <= prefix.size()
        || !std::equal(prefix.begin(), prefix.end(), value.begin(),
                       [](char a, char b) { return a == std::tolower(static_cast<unsigned char>(b)); }))
    {
        return std::nullopt;
    }
    const std::string token = value.substr(prefix.size());
    const Credential* found = nullptr;
    for (const auto& c : security_.credentials)
    {
        if (same_secret(c.token, token)) // no early exit: same time for every credential
        {
            found = &c;
        }
    }
    if (found == nullptr)
    {
        return std::nullopt;
    }
    return Who{found->name, found->role, found->participants, false};
}

std::string role_name(Role role)
{
    switch (role)
    {
    case Role::Viewer:
        return "viewer";
    case Role::Operator:
        return "operator";
    case Role::Admin:
        return "admin";
    case Role::Agent:
        return "agent";
    }
    return "?";
}

const std::vector<RpcRule>& rpc_rules()
{
    // Viewer reads, Operator changes the simulation's inputs and time, Admin ends it.
    static const std::vector<RpcRule> rules = {
        {kControlService, "Ping", Role::Viewer, false, "ping"},
        {kControlService, "ListPeers", Role::Viewer, false, "list peers"},
        {kControlService, "GetValues", Role::Viewer, false, "read values"},
        {kControlService, "Write", Role::Operator, true, "write"},
        {kControlService, "GetOperation", Role::Viewer, false, "read operations"},
        {kControlService, "WaitOperation", Role::Viewer, false, "wait for operations"},
        {kControlService, "Watch", Role::Viewer, false, "watch"},
        {kControlService, "ForgetPeer", Role::Operator, true, "forget a peer"},
        {kControlService, "GetTime", Role::Viewer, false, "read the time"},
        {kControlService, "Pause", Role::Operator, true, "pause"},
        {kControlService, "Resume", Role::Operator, true, "resume"},
        {kControlService, "RunUntil", Role::Operator, true, "run until"},
        {kControlService, "Step", Role::Operator, true, "step"},
        {kControlService, "SetSpeed", Role::Operator, true, "set the speed"},
        {kControlService, "WaitPaused", Role::Viewer, false, "wait for a pause"},
        {kControlService, "Stop", Role::Admin, true, "stop"},
        {kAgentService, "Connect", Role::Agent, true, "connect an agent"},
    };
    return rules;
}

const RpcRule* find_rule(std::string_view service, std::string_view method)
{
    for (const auto& rule : rpc_rules())
    {
        if (rule.service == service && rule.method == method)
        {
            return &rule;
        }
    }
    return nullptr;
}

} // namespace detail

std::vector<Credential> load_credentials(const std::string& path)
{
    std::ifstream in(path);
    if (!in)
    {
        throw std::runtime_error("cannot read the credentials file " + path);
    }
    std::vector<Credential> out;
    std::string line;
    int number = 0;
    const auto fail = [&](const std::string& why) {
        return std::runtime_error(path + ":" + std::to_string(number) + ": " + why);
    };
    while (std::getline(in, line))
    {
        ++number;
        std::istringstream words(line);
        std::vector<std::string> fields;
        for (std::string w; words >> w;)
        {
            if (w[0] == '#')
            {
                break; // a comment: a word that starts with '#'
            }
            fields.push_back(w);
        }
        if (fields.empty())
        {
            continue;
        }
        if (fields.size() < 3)
        {
            throw fail("expected <role> <name> <token> [participant,...]");
        }
        Credential c;
        const auto& role = fields[0];
        if (role == "viewer")
        {
            c.role = Role::Viewer;
        }
        else if (role == "operator")
        {
            c.role = Role::Operator;
        }
        else if (role == "admin")
        {
            c.role = Role::Admin;
        }
        else if (role == "agent")
        {
            c.role = Role::Agent;
        }
        else
        {
            throw fail("unknown role " + role);
        }
        c.name = fields[1];
        c.token = fields[2];
        if (c.token.size() < 16)
        {
            throw fail("a token needs at least 16 characters");
        }
        // The participants: the rest of the line, separated by commas and/or spaces.
        for (std::size_t i = 3; i < fields.size(); ++i)
        {
            std::istringstream list(fields[i]);
            for (std::string p; std::getline(list, p, ',');)
            {
                if (!p.empty())
                {
                    c.participants.push_back(p);
                }
            }
        }
        if (!c.participants.empty() && c.role != Role::Agent)
        {
            throw fail("only an agent credential names participants");
        }
        for (const auto& other : out)
        {
            if (other.token == c.token)
            {
                throw fail("the token of " + c.name + " is used by " + other.name + " already");
            }
        }
        out.push_back(std::move(c));
    }
    return out;
}

} // namespace vehicle_sil::control_grpc
