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

    // NukiLock::LockAction values (NukiLockConstants.h) that can release the
    // latch: Unlatch, LockNgoUnlatch, and the fob actions (their configured
    // action is unknown here, so they count as unlatching).
    inline bool lockActionUnlatches(uint8_t action)
    {
        return action == 0x03 || action == 0x05 || (action >= 0x81 && action <= 0x83);
    }

    // Nuki::CmdResult::NotPaired (NukiDataTypes.h). The only lockAction result
    // returned before anything is written to the lock (NukiBle.hpp executeAction).
    // TimeOut, Failed and Lock_Busy can each come after the command was sent
    // (a TimeOut waiting for the challenge, before the command, looks the same
    // to the caller, so every TimeOut counts as ambiguous).
    constexpr uint8_t CMD_RESULT_NOT_PAIRED = 5;

    // Actions that must not be sent twice: the unlatching ones (a second unlatch
    // opens the door again) and the ones that drive the bolt towards locked
    // (Lock, LockNgo, FullLock): a re-send while the motor is still running from
    // the first command is the likely cause of "motor jam" notifications.
    // Unlock is not in the list: once unlocked, the lock answers a second unlock
    // with Complete and doesn't move (NukiBle.hpp, cmdChallAccStateMachine).
    inline bool lockActionSentOnce(uint8_t action)
    {
        return lockActionUnlatches(action) || action == 0x02 || action == 0x04 || action == 0x06;
    }

    // May a failed lock action be sent again? Send-once actions only when the
    // result proves the command never reached the lock.
    inline bool lockActionRetryable(uint8_t action, uint8_t cmdResult)
    {
        return !lockActionSentOnce(action) || cmdResult == CMD_RESULT_NOT_PAIRED;
    }

    // Nuki::CmdResult as an upper-case log token (cmdResultToString has no
    // Lock_Busy and prints it as "undefined").
    inline const char* cmdResultLogName(uint8_t cmdResult)
    {
        switch(cmdResult)
        {
            case 1:  return "SUCCESS";
            case 2:  return "FAILED";
            case 3:  return "TIMEOUT";
            case 4:  return "WORKING";
            case 5:  return "NOT_PAIRED";
            case 6:  return "LOCK_BUSY";
            case 99: return "ERROR";
            default: return "UNKNOWN";
        }
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

    // A rule field that is null or "" is not checked.
    inline bool ruleFieldSet(const char* s)
    {
        return s != nullptr && s[0] != 0;
    }

    // Rules with an "enabled" member are skipped when it is false; rules
    // without one (tests, the old compile-time struct) always take part.
    template<typename Rule>
    auto ruleEnabled(const Rule& r, int) -> decltype((bool)r.enabled)
    {
        return r.enabled;
    }

    template<typename Rule>
    bool ruleEnabled(const Rule&, long)
    {
        return true;
    }

    // Does one rule match the ?r= token, trigger key, fob and fields?
    // fieldEquals(fieldName, wanted) looks the field up in the trigger.
    template<typename Rule, typename FieldEquals>
    bool ruleMatches(const Rule& r, const char* token, const char* key, const char* device,
                     FieldEquals& fieldEquals)
    {
        if(!ruleEnabled(r, 0)) return false;
        if(ruleFieldSet(r.token) && !ctEquals(token, r.token)) return false;
        if(ruleFieldSet(r.key) && strcmp(key, r.key) != 0) return false;
        if(!ruleFieldSet(r.device) || !macMatches(device, r.device)) return false;
        if(ruleFieldSet(r.value) && !fieldEquals(r.field, r.value)) return false;
        if(ruleFieldSet(r.value2) && !fieldEquals(r.field2, r.value2)) return false;
        return true;
    }

    // Index of the first matching rule, or -1. Rule is any struct with the
    // ProtectRule members (optionally "enabled").
    template<typename Rule, typename FieldEquals>
    int findRule(const Rule* rules, size_t n, const char* token, const char* key, const char* device,
                 FieldEquals fieldEquals)
    {
        for(size_t i = 0; i < n; i++)
        {
            if(ruleMatches(rules[i], token, key, device, fieldEquals)) return (int)i;
        }
        return -1;
    }

    template<typename Rule, size_t N, typename FieldEquals>
    int findRule(const Rule (&rules)[N], const char* token, const char* key, const char* device,
                 FieldEquals fieldEquals)
    {
        return findRule(&rules[0], N, token, key, device, fieldEquals);
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

        // Forget one rule's cooldown (its settings changed).
        void resetRule(size_t rule)
        {
            if(rule < Rules)
            {
                _accepted[rule] = false;
                _lastAcceptedMs[rule] = 0;
            }
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
