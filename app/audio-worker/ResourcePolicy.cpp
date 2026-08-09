// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * ResourcePolicy.cpp — implementation of ResourcePolicy serialization.
 */

#include "ResourcePolicy.hpp"

#include <sstream>
#include <string>

namespace hathor::audio_worker {

// ---------------------------------------------------------------------------
// Minimal JSON serialization (no external JSON dependency in the worker)
// ---------------------------------------------------------------------------

static std::string escapeString(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

std::string ResourcePolicy::serialize() const
{
    std::ostringstream ss;
    ss << "{";
    ss << "\"maxConcurrentLiveVMs\":" << maxConcurrentLiveVMs << ",";
    ss << "\"maxThreads\":" << maxThreads << ",";
    ss << "\"maxVmMemoryMb\":" << maxVmMemoryMb << ",";
    ss << "\"idleSuspendTimeoutSec\":" << idleSuspendTimeoutSec << ",";
    ss << "\"preferSuspendOverDestroy\":" << (preferSuspendOverDestroy ? "true" : "false") << ",";
    ss << "\"ceilingBehavior\":" << static_cast<int>(ceilingBehavior) << ",";
    ss << "\"vmCost\":{";
    ss << "\"cpuPerBlock\":" << vmCost.cpuPerBlock << ",";
    ss << "\"idleMemoryBytes\":" << vmCost.idleMemoryBytes << ",";
    ss << "\"perShredBytes\":" << vmCost.perShredBytes;
    ss << "}}";
    return ss.str();
}

// ---------------------------------------------------------------------------
// Minimal JSON parser (only handles the flat structure we emit)
// ---------------------------------------------------------------------------

namespace {

/// Skip whitespace in a JSON string.
static void skipWs(const char*& p)
{
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
        ++p;
}

/// Parse a JSON string value (without quotes in the result).
static bool parseString(const char*& p, std::string& out)
{
    skipWs(p);
    if (*p != '"') return false;
    ++p;
    out.clear();
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
            ++p;
            switch (*p) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                default:   out += *p;   break;
            }
            ++p;
        } else {
            out += *p;
            ++p;
        }
    }
    if (*p != '"') return false;
    ++p;
    return true;
}

/// Parse a JSON number (int or float).
template<typename T>
static bool parseNumber(const char*& p, T& out)
{
    skipWs(p);
    char* end = nullptr;
    if constexpr (std::is_floating_point_v<T>) {
        double d = std::strtod(p, &end);
        if (end == p) return false;
        out = static_cast<T>(d);
    } else {
        long long ll = std::strtoll(p, &end, 0);
        if (end == p) return false;
        out = static_cast<T>(ll);
    }
    p = end;
    return true;
}

/// Parse a JSON boolean.
static bool parseBool(const char*& p, bool& out)
{
    skipWs(p);
    if (std::strncmp(p, "true", 4) == 0) {
        out = true;
        p += 4;
        return true;
    }
    if (std::strncmp(p, "false", 5) == 0) {
        out = false;
        p += 5;
        return true;
    }
    return false;
}

/// Parse a JSON key (must be a string followed by ':').
static bool parseKey(const char*& p, std::string& key)
{
    skipWs(p);
    if (*p != '"') return false;
    if (!parseString(p, key)) return false;
    skipWs(p);
    if (*p != ':') return false;
    ++p;
    return true;
}

} // namespace

bool ResourcePolicy::deserialize(const std::string& json)
{
    const char* p = json.c_str();
    skipWs(p);
    if (*p != '{') return false;
    ++p;

    while (true) {
        skipWs(p);
        if (*p == '}') {
            ++p;
            break;
        }
        if (*p == ',') {
            ++p;
            continue;
        }

        std::string key;
        if (!parseKey(p, key)) return false;

        skipWs(p);

        if (key == "maxConcurrentLiveVMs") {
            if (!parseNumber(p, maxConcurrentLiveVMs)) return false;
        } else if (key == "maxThreads") {
            if (!parseNumber(p, maxThreads)) return false;
        } else if (key == "maxVmMemoryMb") {
            if (!parseNumber(p, maxVmMemoryMb)) return false;
        } else if (key == "idleSuspendTimeoutSec") {
            if (!parseNumber(p, idleSuspendTimeoutSec)) return false;
        } else if (key == "preferSuspendOverDestroy") {
            if (!parseBool(p, preferSuspendOverDestroy)) return false;
        } else if (key == "ceilingBehavior") {
            int val = 0;
            if (!parseNumber(p, val)) return false;
            ceilingBehavior = static_cast<CeilingBehavior>(val);
        } else if (key == "vmCost") {
            skipWs(p);
            if (*p != '{') return false;
            ++p;
            while (true) {
                skipWs(p);
                if (*p == '}') {
                    ++p;
                    break;
                }
                if (*p == ',') {
                    ++p;
                    continue;
                }
                std::string subKey;
                if (!parseKey(p, subKey)) return false;
                if (subKey == "cpuPerBlock") {
                    if (!parseNumber(p, vmCost.cpuPerBlock)) return false;
                } else if (subKey == "idleMemoryBytes") {
                    if (!parseNumber(p, vmCost.idleMemoryBytes)) return false;
                } else if (subKey == "perShredBytes") {
                    if (!parseNumber(p, vmCost.perShredBytes)) return false;
                } else {
                    return false;
                }
            }
        } else {
            return false;
        }
    }

    return true;
}

} // namespace hathor::audio_worker
