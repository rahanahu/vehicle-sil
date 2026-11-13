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
    return static_cast<int>(who.role) >= static_cast<int>(need);
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
    return Who{found->name, found->role, false};
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
    }
    return "?";
}

const std::vector<RpcRule>& rpc_rules()
{
    // Viewer reads, Operator changes the simulation's inputs and time, Admin ends it. The effect
    // says the same of the call itself, whoever calls: an observe-only server refuses each Change.
    static const std::vector<RpcRule> rules = {
        {kControlService, "Ping", Role::Viewer, Effect::Read, false, "ping"},
        {kControlService, "ListPeers", Role::Viewer, Effect::Read, false, "list peers"},
        {kControlService, "GetValues", Role::Viewer, Effect::Read, false, "read values"},
        {kControlService, "Write", Role::Operator, Effect::Change, true, "write"},
        {kControlService, "GetOperation", Role::Viewer, Effect::Read, false, "read operations"},
        {kControlService, "WaitOperation", Role::Viewer, Effect::Read, false, "wait for operations"},
        {kControlService, "Watch", Role::Viewer, Effect::Read, false, "watch"},
        {kControlService, "ForgetPeer", Role::Operator, Effect::Change, true, "forget a peer"},
        {kControlService, "GetTime", Role::Viewer, Effect::Read, false, "read the time"},
        {kControlService, "GetSnapshot", Role::Viewer, Effect::Read, false, "read a snapshot"},
        {kControlService, "Pause", Role::Operator, Effect::Change, true, "pause"},
        {kControlService, "Resume", Role::Operator, Effect::Change, true, "resume"},
        {kControlService, "RunUntil", Role::Operator, Effect::Change, true, "run until"},
        {kControlService, "Step", Role::Operator, Effect::Change, true, "step"},
        {kControlService, "SetSpeed", Role::Operator, Effect::Change, true, "set the speed"},
        {kControlService, "WaitPaused", Role::Viewer, Effect::Read, false, "wait for a pause"},
        {kControlService, "Stop", Role::Admin, Effect::End, true, "stop"},
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
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        throw std::runtime_error("cannot read the credentials file " + path);
    }
    std::ostringstream text;
    text << in.rdbuf();
    return parse_credentials(text.str(), path);
}

std::vector<Credential> parse_credentials(const std::string& text, const std::string& source)
{
    std::istringstream in(text);
    std::vector<Credential> out;
    std::string line;
    int number = 0;
    // The words of a line are never repeated in a message: one of them is a token (also when
    // they are in the wrong order).
    const auto fail = [&](const std::string& why) {
        return std::runtime_error(source + ":" + std::to_string(number) + ": " + why);
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
        const auto& role = fields[0];
        if (role == "agent")
        {
            throw fail("the role agent is no longer used: the ECUs' control agents reach the server over SIL Kit, "
                       "without a token; delete this line");
        }
        if (fields.size() != 3)
        {
            throw fail("expected <role> <name> <token> (for example: viewer alice <token>)");
        }
        Credential c;
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
        else
        {
            throw fail("the first word is no role: the roles are viewer, operator and admin (<role> <name> <token>)");
        }
        c.name = fields[1];
        c.token = fields[2];
        if (c.token.size() < 16)
        {
            throw fail("a token needs at least 16 characters");
        }
        for (const auto& other : out)
        {
            if (other.token == c.token)
            {
                throw fail("the token is used by an earlier line already: each credential needs a token of its own");
            }
        }
        out.push_back(std::move(c));
    }
    return out;
}

} // namespace vehicle_sil::control_grpc
