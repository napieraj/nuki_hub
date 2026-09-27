#ifdef NUKI_HUB_PROTECT_WEBHOOK

#include "ProtectWebhook.h"
#include "ProtectWebhookConfig.h"
#include "NukiWrapper.h"
#include "LockActionResult.h"
#include "EspMillis.h"
#include "Logger.h"
#include "ArduinoJson.h"
#include <time.h>

extern bool timeSynced;

// Older local ProtectWebhookConfig.h files don't define it.
#ifndef PROTECT_WEBHOOK_ACTION_DEADLINE_MS
#define PROTECT_WEBHOOK_ACTION_DEADLINE_MS 8000
#endif

NukiWrapper* ProtectWebhook::_nuki = nullptr;

namespace
{
    constexpr size_t kMaxBody = 4096;
    constexpr size_t kSeenEvents = 16;
    char seen[kSeenEvents][48] = {};
    size_t seenNext = 0;
    int64_t lastAcceptedMs = -PROTECT_WEBHOOK_COOLDOWN_MS;
    portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;

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
    _nuki = nuki;
    server->on("/protect", HTTP_POST, [](PsychicRequest* request, PsychicResponse* resp)
    {
        return handle(request, resp);
    });
    Log->println("Protect webhook enabled on POST /protect");
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

bool ProtectWebhook::fieldEquals(JsonVariantConst v, const char* want)
{
    // Protect may send the discriminator as a string or a number.
    if(v.is<const char*>())
    {
        return strcasecmp(v.as<const char*>(), want) == 0;
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
        if(seen[i][0] != 0 && strcmp(seen[i], eventId) == 0) return true;
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
    if(strlen(PROTECT_WEBHOOK_SOURCE_IP) > 0 &&
       request->client()->remoteIP().toString() != PROTECT_WEBHOOK_SOURCE_IP)
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
    const int64_t envelopeTs = doc["timestamp"] | (int64_t)0;

    // 3. Find one trigger that matches a rule (key + device + discriminator).
    for(JsonObjectConst t : triggers)
    {
        const char* key = t["key"] | "";
        const char* device = t["device"] | "";
        int64_t ts = t["timestamp"] | (int64_t)0;
        if(ts == 0) ts = envelopeTs;

        // Without an eventId, the (timestamp, key, device) tuple identifies the event.
        char synthId[48];
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
            Log->printf("Protect webhook: no rule for key '%s'\n", key);
            continue;
        }
        if(ts == 0 || llabs(nowMs - ts) > PROTECT_WEBHOOK_MAX_SKEW_MS)
        {
            Log->printf("Protect webhook: stale event, skew %lld ms\n", (long long)(nowMs - ts));
            continue;
        }
        // 4. One action at a time. The queue is a single slot, so a second
        // action would silently replace the first. Refuse before touching the
        // replay cache and cooldown, so Protect's retry (1 s, 2 s) can still
        // succeed once the lock task is done or the pending action expired.
        if(_nuki->isLockActionPending())
        {
            Log->println("Protect webhook: lock action pending, busy");
            return reply(resp, 503, "busy");
        }

        // 5. Replay + cooldown, then act.
        bool proceed = false;
        taskENTER_CRITICAL(&lock);
        const int64_t m = espMillis();
        if(!eventSeen(eventId) && m - lastAcceptedMs >= PROTECT_WEBHOOK_COOLDOWN_MS)
        {
            rememberEvent(eventId);
            lastAcceptedMs = m;
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
