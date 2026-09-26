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

bool ProtectWebhook::userAllowed(const char* guid)
{
    for(const char* allowed : PROTECT_WEBHOOK_USERS)
    {
        if(strcasecmp(guid, allowed) == 0) return true;
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

esp_err_t ProtectWebhook::handle(PsychicRequest* request, PsychicResponse* resp)
{
    // 1. Transport checks: source IP, secret, size.
    if(strlen(PROTECT_WEBHOOK_SOURCE_IP) > 0 &&
       request->client()->remoteIP().toString() != PROTECT_WEBHOOK_SOURCE_IP)
    {
        Log->println("Protect webhook: rejected source IP");
        return reply(resp, 403, "denied");
    }
    if(!request->hasParam("k") || !secretMatches(request->getParam("k")->value()))
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

    // 3. Find one trigger that satisfies every policy check.
    for(JsonObjectConst t : triggers)
    {
        const char* key = t["key"] | "";
        const char* device = t["device"] | "";
        const char* value = t["value"] | "";
        const char* eventId = t["eventId"] | "";
        const int64_t ts = t["timestamp"] | (int64_t)0;

        bool keyOk = false;
        for(const char* k : PROTECT_WEBHOOK_KEYS)
        {
            if(strcmp(key, k) == 0) keyOk = true;
        }
        if(!keyOk || !macMatches(device, PROTECT_WEBHOOK_DEVICE_MAC)) continue;
        if(!userAllowed(value))
        {
            Log->printf("Protect webhook: user not allowed (%s)\n", key);
            continue;
        }
        if(ts == 0 || llabs(nowMs - ts) > PROTECT_WEBHOOK_MAX_SKEW_MS)
        {
            Log->printf("Protect webhook: stale event, skew %lld ms\n", (long long)(nowMs - ts));
            continue;
        }
        if(eventId[0] == 0) continue;

        // 4. Replay + cooldown, then act.
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

        LockActionResult r = _nuki->requestLockAction(PROTECT_WEBHOOK_ACTION);
        Log->printf("Protect webhook: %s -> %s\n", PROTECT_WEBHOOK_ACTION,
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
