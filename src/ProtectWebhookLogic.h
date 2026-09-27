#pragma once

// Pure logic of the UniFi Protect webhook: no Arduino, JSON or FreeRTOS, so it
// is shared by ProtectWebhook.cpp and the host tests (pio test -e native).

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <strings.h>

namespace ProtectWebhookLogic
{
    // MAC addresses compare equal ignoring case and any separators.
    inline bool macMatches(const char* a, const char* b)
    {
        auto next = [](const char*& p) -> int
        {
            while(*p && !isxdigit((unsigned char)*p)) p++;
            return *p ? tolower((unsigned char)*p++) : 0;
        };
        int ca, cb;
        do
        {
            ca = next(a);
            cb = next(b);
            if(ca != cb) return false;
        } while(ca != 0);
        return true;
    }

    // Compare without an early exit on the first differing byte.
    inline bool ctEquals(const char* a, const char* b)
    {
        const size_t la = strlen(a), lb = strlen(b);
        uint8_t diff = (uint8_t)(la != lb);
        for(size_t i = 0; i < la && i < lb; i++) diff |= (uint8_t)a[i] ^ (uint8_t)b[i];
        return diff == 0;
    }

    // Protect sends milliseconds. A value below 1e11 can only be seconds
    // (1e11 ms is 1973). Zero, negative or after 2100 is treated as missing (0),
    // which keeps the skew arithmetic in isFresh far from overflow.
    inline int64_t normalizeTimestampMs(int64_t ts)
    {
        if(ts <= 0)
        {
            return 0;
        }
        if(ts < 100000000000LL)
        {
            ts *= 1000;
        }
        return ts > 4102444800000LL ? 0 : ts;
    }

    // ts must come from normalizeTimestampMs; 0 (missing) is never fresh.
    inline bool isFresh(int64_t nowMs, int64_t ts, int64_t maxSkewMs)
    {
        return ts > 0 && nowMs - ts <= maxSkewMs && ts - nowMs <= maxSkewMs;
    }

    // A button/gesture value given as text (case-insensitive) or as a number.
    inline bool textEquals(const char* got, const char* want)
    {
        return got != nullptr && strcasecmp(got, want) == 0;
    }

    inline bool numberEquals(long long got, const char* want)
    {
        char buf[24];
        snprintf(buf, sizeof(buf), "%lld", got);
        return strcmp(buf, want) == 0;
    }

    // Rule action "relay1".."relay8" -> relay 1..8 on the board; anything else 0
    // (a Nuki lock action).
    inline int relayChannel(const char* action)
    {
        if(action == nullptr || strncmp(action, "relay", 5) != 0)
        {
            return 0;
        }
        const char* d = action + 5;
        return (d[0] >= '1' && d[0] <= '8' && d[1] == 0) ? d[0] - '0' : 0;
    }

    // Without an eventId, the (timestamp, key, device) tuple identifies the event.
    inline const char* eventIdOrSynth(const char* eventId, int64_t ts, const char* key, const char* device,
                                      char* buf, size_t bufSize)
    {
        if(eventId != nullptr && eventId[0] != 0)
        {
            return eventId;
        }
        snprintf(buf, bufSize, "%lld|%s|%s", (long long)ts, key, device);
        return buf;
    }

    // Index of the first rule matching the ?r= token, trigger key, fob and
    // fields, or -1. fieldEquals(fieldName, wanted) looks the field up in the
    // trigger. Rule is any struct with the ProtectRule members.
    template<typename Rule, size_t N, typename FieldEquals>
    int findRule(const Rule (&rules)[N], const char* token, const char* key, const char* device,
                 FieldEquals fieldEquals)
    {
        for(size_t i = 0; i < N; i++)
        {
            const Rule& r = rules[i];
            if(r.token != nullptr && !ctEquals(token, r.token)) continue;
            if(r.key != nullptr && strcmp(key, r.key) != 0) continue;
            if(!macMatches(device, r.device)) continue;
            if(r.value != nullptr && !fieldEquals(r.field, r.value)) continue;
            if(r.value2 != nullptr && !fieldEquals(r.field2, r.value2)) continue;
            return (int)i;
        }
        return -1;
    }

    // Replay cache (last Seen event IDs, whatever rule) plus a cooldown per
    // rule. Not thread-safe: the caller serializes calls.
    template<size_t Rules, size_t Seen = 16, size_t IdLen = 64>
    class ReplayGuard
    {
    public:
        // True if the event is new and the rule is out of cooldown; then both
        // are recorded. False (nothing recorded) for a replay or cooldown.
        bool accept(const char* eventId, size_t rule, int64_t nowMs, int64_t cooldownMs)
        {
            if(rule >= Rules || seen(eventId))
            {
                return false;
            }
            if(_accepted[rule] && nowMs - _lastAcceptedMs[rule] < cooldownMs)
            {
                return false;
            }
            remember(eventId);
            _accepted[rule] = true;
            _lastAcceptedMs[rule] = nowMs;
            return true;
        }

    private:
        bool seen(const char* eventId) const
        {
            for(size_t i = 0; i < Seen; i++)
            {
                // Stored IDs are truncated to IdLen - 1 chars; compare the same way.
                if(_seen[i][0] != 0 && strncmp(_seen[i], eventId, IdLen - 1) == 0) return true;
            }
            return false;
        }

        void remember(const char* eventId)
        {
            strncpy(_seen[_next], eventId, IdLen - 1);
            _seen[_next][IdLen - 1] = 0;
            _next = (_next + 1) % Seen;
        }

        char _seen[Seen][IdLen] = {};
        size_t _next = 0;
        bool _accepted[Rules] = {};
        int64_t _lastAcceptedMs[Rules] = {};
    };
}
