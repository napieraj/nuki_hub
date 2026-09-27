#ifdef NUKI_HUB_PROTECT_WEBHOOK

#include "ProtectWebhook.h"
#include "ProtectWebhookConfig.h"
#include "NukiWrapper.h"
#include "LockActionResult.h"
#include "EspMillis.h"
#include "Logger.h"
#include "RestartReason.h"
#include "WaveshareBoard.h"
#include "util/NukiHelper.h"
#include "ArduinoJson.h"
#include <time.h>
#include "lwip/sockets.h"
#include "lwip/inet.h"

extern bool timeSynced;

// Older local ProtectWebhookConfig.h files don't define it.
#ifndef PROTECT_WEBHOOK_ACTION_DEADLINE_MS
#define PROTECT_WEBHOOK_ACTION_DEADLINE_MS 8000
#endif
#ifndef PROTECT_WEBHOOK_BLE_STALL_MS
#define PROTECT_WEBHOOK_BLE_STALL_MS 30000
#endif

// Build-time checks of ProtectWebhookConfig.h. The rules must be
// "static constexpr" for these to see them.
namespace ProtectWebhookCheck
{
    constexpr size_t len(const char* s)
    {
        size_t n = 0;
        while(s[n] != 0) n++;
        return n;
    }

    constexpr bool set(const char* s)
    {
        return s != nullptr && s[0] != 0;
    }

    constexpr bool isPlaceholder(const char* s)
    {
        const char* p = "replace";
        if(s == nullptr) return false;
        for(size_t i = 0; p[i] != 0; i++)
        {
            if(s[i] != p[i]) return false;
        }
        return true;
    }

    template<typename F> constexpr bool allRules(F f)
    {
        for(const ProtectRule& r : PROTECT_WEBHOOK_RULES)
        {
            if(!f(r)) return false;
        }
        return true;
    }
}

static_assert(ProtectWebhookCheck::len(PROTECT_WEBHOOK_SECRET) >= 32,
              "PROTECT_WEBHOOK_SECRET must be at least 32 characters (openssl rand -hex 32)");
static_assert(ProtectWebhookCheck::allRules([](const ProtectRule& r) { return ProtectWebhookCheck::set(r.device); }),
              "every Protect rule needs a device (the fob's MAC)");
static_assert(ProtectWebhookCheck::allRules([](const ProtectRule& r) { return ProtectWebhookCheck::set(r.action); }),
              "every Protect rule needs an action");
static_assert(ProtectWebhookCheck::allRules([](const ProtectRule& r) { return r.value == nullptr || ProtectWebhookCheck::set(r.field); }),
              "a Protect rule with 'value' needs 'field'");
static_assert(ProtectWebhookCheck::allRules([](const ProtectRule& r) { return r.value2 == nullptr || ProtectWebhookCheck::set(r.field2); }),
              "a Protect rule with 'value2' needs 'field2'");
static_assert(ProtectWebhookCheck::allRules([](const ProtectRule& r) { return r.token == nullptr || ProtectWebhookCheck::len(r.token) >= 16; }),
              "Protect rule tokens must be at least 16 characters (openssl rand -hex 16)");
#ifndef PROTECT_WEBHOOK_ALLOW_BROAD_RULES
static_assert(ProtectWebhookCheck::allRules([](const ProtectRule& r) { return r.token != nullptr || (ProtectWebhookCheck::set(r.key) && r.value != nullptr); }),
              "a Protect rule without a token needs key + value, or it matches ANY event from that fob "
              "(define PROTECT_WEBHOOK_ALLOW_BROAD_RULES to allow that)");
#endif
#ifndef PROTECT_WEBHOOK_ALLOW_PLACEHOLDERS
static_assert(!ProtectWebhookCheck::isPlaceholder(PROTECT_WEBHOOK_SECRET),
              "PROTECT_WEBHOOK_SECRET is still the placeholder from the example");
static_assert(ProtectWebhookCheck::allRules([](const ProtectRule& r) { return !ProtectWebhookCheck::isPlaceholder(r.token); }),
              "a Protect rule token is still the placeholder from the example");
#endif

NukiWrapper* ProtectWebhook::_nuki = nullptr;

namespace
{
    constexpr size_t kMaxBody = 4096;
    constexpr size_t kSeenEvents = 16;
    char seen[kSeenEvents][64] = {};
    size_t seenNext = 0;
    // Cooldown per rule (index into PROTECT_WEBHOOK_RULES), so "hold to unlock"
    // followed by "press to lock" isn't blocked. 0 = never accepted.
    constexpr size_t kRules = sizeof(PROTECT_WEBHOOK_RULES) / sizeof(PROTECT_WEBHOOK_RULES[0]);
    int64_t lastAcceptedMs[kRules] = {};
    portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;

    // Last BLE heartbeat (espMillis), 0 = nuki task not running yet.
    int64_t bleHeartbeatTs = 0;
    portMUX_TYPE bleHeartbeatMux = portMUX_INITIALIZER_UNLOCKED;

    // PROTECT_WEBHOOK_SOURCE_IP in network byte order; 0 = check disabled.
    uint32_t allowedSourceIp = 0;

    // Accept the peer only if it is exactly allowedSourceIp: plain IPv4, or
    // IPv6 when IPv4-mapped (::ffff:a.b.c.d). Any other IPv6 peer is refused
    // (PsychicClient::remoteIP() would compare only its low 32 bits).
    bool sourceIpAllowed(int sock)
    {
        struct sockaddr_storage peer;
        socklen_t peerLen = sizeof(peer);
        if(getpeername(sock, (struct sockaddr*)&peer, &peerLen) != 0)
        {
            return false;
        }
        uint32_t ip;
        if(peer.ss_family == AF_INET)
        {
            ip = ((struct sockaddr_in*)&peer)->sin_addr.s_addr;
        }
#if LWIP_IPV6
        else if(peer.ss_family == AF_INET6)
        {
            const struct sockaddr_in6* p6 = (const struct sockaddr_in6*)&peer;
            const uint32_t* w = p6->sin6_addr.un.u32_addr;
            if(w[0] != 0 || w[1] != 0 || w[2] != PP_HTONL(0x0000ffffUL))
            {
                return false;
            }
            ip = w[3];
        }
#endif
        else
        {
            return false;
        }
        return ip == allowedSourceIp;
    }

    esp_err_t reply(PsychicResponse* resp, int code, const char* result)
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "{\"result\":\"%s\"}", result);
        return resp->send(code, "application/json", buf);
    }
}

void ProtectWebhook::registerRoute(PsychicHttpServer* server, NukiWrapper* nuki)
{
    if(nuki == nullptr)
    {
        return;
    }
    // Fail closed: one rule with an unknown action disables the whole route.
    bool configValid = true;
    for(const ProtectRule& r : PROTECT_WEBHOOK_RULES)
    {
        if((int)NukiHelper::lockActionToEnum(r.action) == 0xff)
        {
            Log->printf("Protect webhook: unknown action '%s' in ProtectWebhookConfig.h\n", r.action);
            configValid = false;
        }
    }
    if(strlen(PROTECT_WEBHOOK_SOURCE_IP) > 0)
    {
        struct in_addr parsed;
        if(inet_pton(AF_INET, PROTECT_WEBHOOK_SOURCE_IP, &parsed) != 1 || parsed.s_addr == 0)
        {
            Log->printf("Protect webhook: invalid PROTECT_WEBHOOK_SOURCE_IP '%s'\n", PROTECT_WEBHOOK_SOURCE_IP);
            configValid = false;
        }
        else
        {
            allowedSourceIp = parsed.s_addr;
        }
    }
    if(!configValid)
    {
        Log->println("Protect webhook disabled: fix ProtectWebhookConfig.h");
        return;
    }

    _nuki = nuki;
    server->on("/protect", HTTP_POST, [](PsychicRequest* request, PsychicResponse* resp)
    {
        return handle(request, resp);
    });
    Log->println("Protect webhook enabled on POST /protect");
}

void ProtectWebhook::bleHeartbeat()
{
    const int64_t now = espMillis();
    taskENTER_CRITICAL(&bleHeartbeatMux);
    bleHeartbeatTs = now;
    taskEXIT_CRITICAL(&bleHeartbeatMux);
}

bool ProtectWebhook::secretMatches(const String& provided)
{
    const char* expected = PROTECT_WEBHOOK_SECRET;
    const size_t n = strlen(expected);
    if(n < 32 || provided.length() != n)
    {
        return false;
    }
    uint8_t diff = 0;
    for(size_t i = 0; i < n; i++)
    {
        diff |= (uint8_t)provided[i] ^ (uint8_t)expected[i];
    }
    return diff == 0;
}

bool ProtectWebhook::macMatches(const char* a, const char* b)
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

namespace
{
    // A string field, or the Integration API style {"text": "..."} wrapper.
    const char* fieldText(JsonVariantConst v)
    {
        if(v.is<const char*>())
        {
            return v.as<const char*>();
        }
        if(v["text"].is<const char*>())
        {
            return v["text"].as<const char*>();
        }
        return nullptr;
    }

    // Protect sends milliseconds. A value below 1e11 can only be seconds
    // (1e11 ms is 1973). Negative, zero or after 2100 is treated as missing,
    // which keeps the skew arithmetic below far from overflow.
    int64_t normalizeTimestampMs(int64_t ts)
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
}

bool ProtectWebhook::fieldEquals(JsonVariantConst v, const char* want)
{
    // Protect may send the discriminator as a string, a {"text": ...} wrapper
    // or a number.
    const char* text = fieldText(v);
    if(text != nullptr)
    {
        return strcasecmp(text, want) == 0;
    }
    if(v.is<long long>())
    {
        char buf[24];
        snprintf(buf, sizeof(buf), "%lld", v.as<long long>());
        return strcmp(buf, want) == 0;
    }
    return false;
}

bool ProtectWebhook::eventSeen(const char* eventId)
{
    for(size_t i = 0; i < kSeenEvents; i++)
    {
        // Stored IDs are truncated to 63 chars; compare the same way.
        if(seen[i][0] != 0 && strncmp(seen[i], eventId, sizeof(seen[0]) - 1) == 0) return true;
    }
    return false;
}

void ProtectWebhook::rememberEvent(const char* eventId)
{
    strlcpy(seen[seenNext], eventId, sizeof(seen[0]));
    seenNext = (seenNext + 1) % kSeenEvents;
}

namespace
{
    bool ctEquals(const char* a, const char* b)
    {
        const size_t la = strlen(a), lb = strlen(b);
        uint8_t diff = (uint8_t)(la != lb);
        for(size_t i = 0; i < la && i < lb; i++) diff |= (uint8_t)a[i] ^ (uint8_t)b[i];
        return diff == 0;
    }
}

esp_err_t ProtectWebhook::handle(PsychicRequest* request, PsychicResponse* resp)
{
    // 1. Transport checks: source IP, secret, size.
    if(strlen(PROTECT_WEBHOOK_SOURCE_IP) > 0 && !sourceIpAllowed(request->client()->socket()))
    {
        Log->println("Protect webhook: rejected source IP");
        return reply(resp, 403, "denied");
    }
    // Secret: "?k=<secret>" in the Delivery URL, or Protect's Bearer auth option
    // ("Authorization: Bearer <secret>"), which keeps it out of URLs and logs.
    String provided;
    if(request->hasParam("k"))
    {
        provided = request->getParam("k")->value();
    }
    else if(request->hasHeader("Authorization"))
    {
        const String auth = request->header("Authorization");
        if(auth.startsWith("Bearer "))
        {
            provided = auth.substring(7);
            provided.trim();
        }
    }
    if(provided.length() == 0 || !secretMatches(provided))
    {
        Log->println("Protect webhook: bad secret");
        return reply(resp, 403, "denied");
    }
    if(request->contentLength() == 0 || request->contentLength() > kMaxBody)
    {
        return reply(resp, 413, "bad_size");
    }

    // 2. Freshness needs a real clock.
    if(!timeSynced)
    {
        Log->println("Protect webhook: time not synced, refusing");
        return reply(resp, 503, "no_time");
    }

    JsonDocument doc;
    if(deserializeJson(doc, request->body()))
    {
        return reply(resp, 400, "bad_json");
    }

    JsonArrayConst triggers = doc["alarm"]["triggers"].as<JsonArrayConst>();
    if(triggers.isNull())
    {
        return reply(resp, 400, "no_triggers");
    }

    const int64_t nowMs = (int64_t)time(nullptr) * 1000;

    // Optional per-alarm token (?r=...): one Protect alarm per button+gesture,
    // each with its own Delivery URL, lets Protect do the button filtering.
    String ruleToken = request->hasParam("r") ? request->getParam("r")->value() : String();

    // Older Alarm Manager payloads carry only key/device per trigger; fall back
    // to the envelope timestamp.
    const int64_t envelopeTs = normalizeTimestampMs(doc["timestamp"] | (int64_t)0);

    // 3. Find one trigger that matches a rule (key + device + discriminator).
    for(JsonObjectConst t : triggers)
    {
        const char* key = t["key"] | "";
        const char* device = t["device"] | "";
        int64_t ts = normalizeTimestampMs(t["timestamp"] | (int64_t)0);
        if(ts == 0) ts = envelopeTs;

        // Without an eventId, the (timestamp, key, device) tuple identifies the event.
        char synthId[64];
        const char* eventId = t["eventId"] | "";
        if(eventId[0] == 0)
        {
            snprintf(synthId, sizeof(synthId), "%lld|%s|%s", (long long)ts, key, device);
            eventId = synthId;
        }

        const ProtectRule* rule = nullptr;
        for(const ProtectRule& r : PROTECT_WEBHOOK_RULES)
        {
            if(r.token != nullptr && !ctEquals(ruleToken.c_str(), r.token)) continue;
            if(r.key != nullptr && strcmp(key, r.key) != 0) continue;
            if(!macMatches(device, r.device)) continue;
            if(r.value != nullptr && !fieldEquals(t[r.field], r.value)) continue;
            if(r.value2 != nullptr && !fieldEquals(t[r.field2], r.value2)) continue;
            rule = &r;
            break;
        }
        if(rule == nullptr)
        {
            // Authenticated but unmatched: log what arrived (never the secret
            // or token) so a changed Protect payload is easy to spot.
            const char* button = fieldText(t["button"]);
            const char* value = fieldText(t["value"]);
            Log->printf("Protect webhook: no rule for key '%s' device '%s' button '%s' value '%s'\n",
                        key, device, button ? button : "", value ? value : "");
            continue;
        }
        if(ts == 0 || llabs(nowMs - ts) > PROTECT_WEBHOOK_MAX_SKEW_MS)
        {
            Log->printf("Protect webhook: stale event, skew %lld ms\n", (long long)(nowMs - ts));
            continue;
        }
        // 4. The nuki task must be alive, or nothing would ever be sent.
        taskENTER_CRITICAL(&bleHeartbeatMux);
        const int64_t heartbeat = bleHeartbeatTs;
        taskEXIT_CRITICAL(&bleHeartbeatMux);
        if(heartbeat == 0)
        {
            Log->println("Protect webhook: BLE not started yet");
            return reply(resp, 503, "ble_stalled");
        }
        if(espMillis() - heartbeat > PROTECT_WEBHOOK_BLE_STALL_MS)
        {
            Log->printf("Protect webhook: nuki task stalled for %lld ms, restarting\n",
                        (long long)(espMillis() - heartbeat));
#ifdef NUKI_HUB_WAVESHARE_8DI8RO
            WaveshareBoard::recordBleEvent(WaveshareBoard::BLE_EVENT_STALLED);
#endif
            esp_err_t res = reply(resp, 503, "ble_stalled");
            espDelay(100); // let the reply leave before the reboot
            restartEsp(RestartReason::BLEError);
            return res;
        }

        // 5. One action at a time. The queue is a single slot, so a second
        // action would silently replace the first. Refuse before touching the
        // replay cache and cooldown, so Protect's retry (1 s, 2 s) can still
        // succeed once the lock task is done or the pending action expired.
        if(_nuki->isLockActionPending())
        {
            Log->println("Protect webhook: lock action pending, busy");
            return reply(resp, 503, "busy");
        }

        // 6. Replay + cooldown, then act.
        const size_t ruleIndex = rule - PROTECT_WEBHOOK_RULES;
        bool proceed = false;
        taskENTER_CRITICAL(&lock);
        const int64_t m = espMillis();
        if(!eventSeen(eventId) &&
           (lastAcceptedMs[ruleIndex] == 0 || m - lastAcceptedMs[ruleIndex] >= PROTECT_WEBHOOK_COOLDOWN_MS))
        {
            rememberEvent(eventId);
            lastAcceptedMs[ruleIndex] = m;
            proceed = true;
        }
        taskEXIT_CRITICAL(&lock);
        if(!proceed)
        {
            Log->println("Protect webhook: duplicate or cooldown");
            return reply(resp, 429, "cooldown");
        }

        // Drop the action if the lock can't be reached in time: a press
        // shouldn't unlock the door long after the user gave up.
        LockActionResult r = _nuki->requestLockAction(rule->action, espMillis() + PROTECT_WEBHOOK_ACTION_DEADLINE_MS);
        Log->printf("Protect webhook: %s -> %s\n", rule->action,
                    r == LockActionResult::Success ? "queued" : "refused");
        switch(r)
        {
        case LockActionResult::Success:       return reply(resp, 200, "ack");
        case LockActionResult::AccessDenied:  return reply(resp, 403, "acl_denied");
        default:                              return reply(resp, 500, "error");
        }
    }

    return reply(resp, 403, "no_match");
}

#endif
