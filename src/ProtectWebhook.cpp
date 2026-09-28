#ifdef NUKI_HUB_PROTECT_WEBHOOK

#include "ProtectWebhook.h"
#include "ForkSettings.h"
#include "ProtectWebhookLogic.h"
#include "NukiWrapper.h"
#include "LockActionResult.h"
#include "EspMillis.h"
#include "Logger.h"
#include "RestartReason.h"
#include "WaveshareBoard.h"
#include "util/NukiHelper.h"
#include "ArduinoJson.h"
#ifdef NUKI_HUB_EMBEDDED_LOCK_MQTT
#include "LockMqttServer.h"
#endif
#include <time.h>
#include "lwip/sockets.h"
#include "lwip/inet.h"

extern bool timeSynced;

NukiWrapper* ProtectWebhook::_nuki = nullptr;

namespace
{
    using namespace ProtectWebhookLogic;
    using ForkSettingsLogic::MAX_RULES;

    constexpr size_t kMaxBody = 4096;
    // Replay cache for the last 16 event IDs, plus a cooldown per rule slot,
    // so "hold to unlock" followed by "press to lock" isn't blocked. Guarded
    // by `lock`.
    ReplayGuard<MAX_RULES> replayGuard;
    portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
    // The previous accepted webhook lock action (queued or skipped; any rule,
    // any lock action, not relays), for the redundant-action skip grace.
    // Guarded by `lock`.
    bool havePrevLockAction = false;
    int64_t prevLockActionMs = 0;

    void notePrevLockAction(int64_t ms)
    {
        taskENTER_CRITICAL(&lock);
        havePrevLockAction = true;
        prevLockActionMs = ms;
        taskEXIT_CRITICAL(&lock);
    }

    // Last BLE heartbeat (espMillis), 0 = nuki task not running yet.
    int64_t bleHeartbeatTs = 0;
    portMUX_TYPE bleHeartbeatMux = portMUX_INITIALIZER_UNLOCKED;

    // Recent results for the settings page. Guarded by resultMux.
    ProtectWebhook::Result results[ProtectWebhook::RESULT_LOG_SIZE];
    size_t resultNext = 0;
    size_t resultStored = 0;
    portMUX_TYPE resultMux = portMUX_INITIALIZER_UNLOCKED;

    void recordResult(int code, const char* result, int rule, const char* action)
    {
        ProtectWebhook::Result r = {};
        const time_t now = time(nullptr);
        r.epoch = timeSynced ? (int64_t)now : 0;
        r.uptimeMs = espMillis();
        r.code = (uint16_t)code;
        r.result = result;
        r.rule = (int8_t)rule;
        if(action != nullptr)
        {
            strlcpy(r.action, action, sizeof(r.action));
        }
        taskENTER_CRITICAL(&resultMux);
        results[resultNext] = r;
        resultNext = (resultNext + 1) % ProtectWebhook::RESULT_LOG_SIZE;
        if(resultStored < ProtectWebhook::RESULT_LOG_SIZE)
        {
            resultStored++;
        }
        taskEXIT_CRITICAL(&resultMux);
    }

    // Accept the peer only if it is exactly the allowed IPv4 address: plain
    // IPv4, or IPv6 when IPv4-mapped (::ffff:a.b.c.d). Any other IPv6 peer is
    // refused (PsychicClient::remoteIP() would compare only its low 32 bits).
    bool sourceIpAllowed(int sock, uint32_t allowedSourceIp)
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
        return allowedSourceIp != 0 && ip == allowedSourceIp;
    }

    // Visible feedback after authentication (never before, so an
    // unauthenticated caller learns nothing from the LED).
    void ledFeedback(bool ok)
    {
#ifdef NUKI_HUB_WAVESHARE_8DI8RO
        WaveshareBoard::flashStatusLed(ok);
#else
        (void)ok;
#endif
    }

    esp_err_t send(PsychicResponse* resp, int code, const char* result)
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "{\"result\":\"%s\"}", result);
        return resp->send(code, "application/json", buf);
    }
}

void ProtectWebhook::registerRoute(PsychicHttpServer* server, NukiWrapper* nuki)
{
    // Always registered: whether requests are processed is decided per
    // request from the settings, which can change at runtime.
    _nuki = nuki;
    server->on("/protect", HTTP_POST, [](PsychicRequest* request, PsychicResponse* resp)
    {
        return handle(request, resp);
    });
}

void ProtectWebhook::bleHeartbeat()
{
    const int64_t now = espMillis();
    taskENTER_CRITICAL(&bleHeartbeatMux);
    bleHeartbeatTs = now;
    taskEXIT_CRITICAL(&bleHeartbeatMux);
}

int64_t ProtectWebhook::bleHeartbeatAgeMs()
{
    taskENTER_CRITICAL(&bleHeartbeatMux);
    const int64_t heartbeat = bleHeartbeatTs;
    taskEXIT_CRITICAL(&bleHeartbeatMux);
    return heartbeat == 0 ? -1 : espMillis() - heartbeat;
}

void ProtectWebhook::onRulesChanged(const bool* changed, size_t count)
{
    taskENTER_CRITICAL(&lock);
    for(size_t i = 0; i < count; i++)
    {
        if(changed[i])
        {
            replayGuard.resetRule(i);
        }
    }
    taskEXIT_CRITICAL(&lock);
}

size_t ProtectWebhook::recentResults(Result* out, size_t max)
{
    taskENTER_CRITICAL(&resultMux);
    size_t n = 0;
    for(; n < resultStored && n < max; n++)
    {
        out[n] = results[(resultNext + RESULT_LOG_SIZE - 1 - n) % RESULT_LOG_SIZE];
    }
    taskEXIT_CRITICAL(&resultMux);
    return n;
}

bool ProtectWebhook::secretMatches(const String& provided, const char* expected)
{
    const size_t n = strlen(expected);
    if(n < ForkSettingsLogic::LEN_SECRET_MIN || provided.length() != n)
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
}

bool ProtectWebhook::fieldEquals(JsonVariantConst v, const char* want)
{
    // Protect may send the discriminator as a string, a {"text": ...} wrapper
    // or a number.
    const char* text = fieldText(v);
    if(text != nullptr)
    {
        return textEquals(text, want);
    }
    if(v.is<long long>())
    {
        return numberEquals(v.as<long long>(), want);
    }
    return false;
}

esp_err_t ProtectWebhook::handle(PsychicRequest* request, PsychicResponse* resp)
{
    int ruleIndex = -1;
    const char* ruleAction = nullptr;
    auto reply = [&](int code, const char* result)
    {
        recordResult(code, result, ruleIndex, ruleAction);
        return send(resp, code, result);
    };

    // The settings can't change while this request runs (a save waits), and
    // it never sees a half-written rule table.
    ForkSettings::ReadLock cfg(pdMS_TO_TICKS(2000));
    if(!cfg.locked())
    {
        return reply(503, "busy");
    }
    if(!cfg.active())
    {
        Log->println("Protect webhook: disabled (see the Protect Webhook & Relays page)");
        return reply(403, "disabled");
    }
    const ForkSettings::Settings& s = cfg.settings();

    // 1. Transport checks: source IP, secret, size.
    if(s.sourceIp[0] != 0)
    {
        struct in_addr parsed = {};
        const bool parsedOk = inet_pton(AF_INET, s.sourceIp, &parsed) == 1;
        if(!parsedOk || !sourceIpAllowed(request->client()->socket(), parsed.s_addr))
        {
            Log->println("Protect webhook: rejected source IP");
            return reply(403, "denied");
        }
    }
    // Secret: Protect's Bearer auth option ("Authorization: Bearer <secret>"),
    // which keeps it out of URLs and logs, or "?k=<secret>" in the Delivery URL
    // unless "Bearer only" is set.
    String provided;
    if(request->hasParam("k"))
    {
        if(s.bearerOnly)
        {
            Log->println("Protect webhook: secret in the URL refused (Bearer only)");
            return reply(403, "denied");
        }
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
    if(provided.length() == 0 || !secretMatches(provided, s.secret))
    {
        Log->println("Protect webhook: bad secret");
        return reply(403, "denied");
    }
    if(request->contentLength() == 0 || request->contentLength() > kMaxBody)
    {
        return reply(413, "bad_size");
    }

    // 2. Freshness needs a real clock.
    if(!timeSynced)
    {
        Log->println("Protect webhook: time not synced, refusing");
        return reply(503, "no_time");
    }

    JsonDocument doc;
    if(deserializeJson(doc, request->body()))
    {
        return reply(400, "bad_json");
    }

    JsonArrayConst triggers = doc["alarm"]["triggers"].as<JsonArrayConst>();
    if(triggers.isNull())
    {
        return reply(400, "no_triggers");
    }

    const int64_t nowMs = (int64_t)time(nullptr) * 1000;

    // Optional per-alarm token (?r=...): one Protect alarm per button+gesture,
    // each with its own Delivery URL, lets Protect do the button filtering.
    String ruleToken = request->hasParam("r") ? request->getParam("r")->value() : String();

    // Older Alarm Manager payloads carry only key/device per trigger; fall back
    // to the envelope timestamp.
    const int64_t envelopeTs = normalizeTimestampMs(doc["timestamp"] | (int64_t)0);

    // 3. Find one trigger that matches an enabled rule (key + device + discriminator).
    for(JsonObjectConst t : triggers)
    {
        const char* key = t["key"] | "";
        const char* device = t["device"] | "";
        int64_t ts = normalizeTimestampMs(t["timestamp"] | (int64_t)0);
        if(ts == 0) ts = envelopeTs;

        char synthId[64];
        const char* eventId = eventIdOrSynth(t["eventId"] | "", ts, key, device, synthId, sizeof(synthId));

        const int matched = findRule(s.rules, MAX_RULES, ruleToken.c_str(), key, device,
                                     [&t](const char* field, const char* want)
        {
            return fieldEquals(t[field], want);
        });
        if(matched < 0)
        {
            // Authenticated but unmatched: log what arrived (never the secret
            // or token) so a changed Protect payload is easy to spot.
            const char* button = fieldText(t["button"]);
            const char* value = fieldText(t["value"]);
            Log->printf("Protect webhook: no rule for key '%s' device '%s' button '%s' value '%s'\n",
                        key, device, button ? button : "", value ? value : "");
            continue;
        }
        const ForkSettings::Rule* rule = &s.rules[matched];
        if(!isFresh(nowMs, ts, s.maxSkewMs))
        {
            Log->printf("Protect webhook: stale event, skew %lld ms\n", (long long)(nowMs - ts));
            continue;
        }
        ruleIndex = matched;
        ruleAction = rule->action;
        // Relay rules pulse a relay on the board; the lock checks below
        // (BLE alive, lock busy) don't apply to them.
        const int relay = relayChannel(rule->action);
        if(relay > (int)s.relayCount)
        {
            // Not reached: validation refuses relayN beyond the relay count.
            return reply(500, "error");
        }
        if(relay == 0)
        {
            if(_nuki == nullptr)
            {
                Log->println("Protect webhook: lock action, but no Nuki lock is enabled");
                return reply(500, "error");
            }
            // 4. The nuki task must be alive, or nothing would ever be sent.
            const int64_t age = bleHeartbeatAgeMs();
            if(age < 0)
            {
                Log->println("Protect webhook: BLE not started yet");
                return reply(503, "ble_stalled");
            }
            if(age > (int64_t)s.bleStallMs)
            {
                Log->printf("Protect webhook: nuki task stalled for %lld ms, restarting\n", (long long)age);
#ifdef NUKI_HUB_WAVESHARE_8DI8RO
                WaveshareBoard::recordBleEvent(WaveshareBoard::BLE_EVENT_STALLED);
#endif
                esp_err_t res = reply(503, "ble_stalled");
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
                return reply(503, "busy");
            }
        }

        // 6. Replay + cooldown, then act.
        const int64_t m = espMillis();
        taskENTER_CRITICAL(&lock);
        const bool proceed = replayGuard.accept(eventId, (size_t)matched, m, s.cooldownMs);
        taskEXIT_CRITICAL(&lock);
        if(!proceed)
        {
            Log->println("Protect webhook: duplicate or cooldown");
            return reply(429, "cooldown");
        }

        if(relay > 0)
        {
#ifdef NUKI_HUB_WAVESHARE_8DI8RO
            const bool pulsed = WaveshareBoard::pulseRelay(relay, s.relayPulseMs);
            Log->printf("Protect webhook: %s -> %s\n", rule->action, pulsed ? "pulsed" : "failed");
            ledFeedback(pulsed);
            return pulsed ? reply(200, "ack") : reply(500, "error");
#else
            return reply(500, "error"); // not reached: no relays, validation refuses relay rules
#endif
        }

#ifdef NUKI_HUB_EMBEDDED_LOCK_MQTT
        // 7. Redundant lock/unlock: only while the lock's MQTT session is live
        // and its state is fresh (never from the BLE cache, which can be
        // 30 min old), and not within the skip grace after the previous
        // accepted webhook lock action (the state can lag or be mid-motion).
        // After replay/cooldown, so a skip counts as a press.
        if(s.lockMqttEnabled && s.skipRedundant && _nuki->lockActionAllowed(rule->action))
        {
            LockMqttLogic::LiveState live;
            int64_t stateAgeMs;
            LockMqttServer::liveState(live, stateAgeMs);
            // The previous accepted lock action, read before this one is noted.
            LockMqttLogic::Grace grace = { false, 0, m, (int64_t)s.skipGraceS * 1000 };
            taskENTER_CRITICAL(&lock);
            grace.havePrevious = havePrevLockAction;
            grace.previousMs = prevLockActionMs;
            taskEXIT_CRITICAL(&lock);
            const LockMqttLogic::Skip skip = LockMqttLogic::redundantAction(rule->action, live, (int64_t)s.lockSilenceMs, grace);
            if(skip == LockMqttLogic::Skip::SendGrace)
            {
                const int64_t ago = grace.nowMs > grace.previousMs ? grace.nowMs - grace.previousMs : 0;
                Log->printf("Protect webhook: %s -> sent, previous fob action %lld s ago (skip grace %u s)\n",
                            rule->action, (long long)(ago / 1000), (unsigned)s.skipGraceS);
            }
            else if(skip != LockMqttLogic::Skip::Send)
            {
                notePrevLockAction(m);
                const bool locked = skip == LockMqttLogic::Skip::AlreadyLocked;
                Log->printf("Protect webhook: %s -> skipped, already %s (MQTT state %lld ms old, lock last heard %lld ms ago)\n",
                            rule->action, locked ? "locked" : "unlocked", (long long)stateAgeMs, (long long)live.lastRxAgeMs);
                ledFeedback(true);
                return reply(200, locked ? "skipped_locked" : "skipped_unlocked");
            }
        }
#endif

        // Drop the action if the lock can't be reached in time: a press
        // shouldn't unlock the door long after the user gave up.
        LockActionResult r = _nuki->requestLockAction(rule->action, espMillis() + s.actionDeadlineMs);
        if(r == LockActionResult::Success)
        {
            notePrevLockAction(m);
        }
        Log->printf("Protect webhook: %s -> %s\n", rule->action,
                    r == LockActionResult::Success ? "queued" : "refused");
        switch(r)
        {
        case LockActionResult::Success:       ledFeedback(true);  return reply(200, "ack");
        case LockActionResult::AccessDenied:  ledFeedback(false); return reply(403, "acl_denied");
        default:                              ledFeedback(false); return reply(500, "error");
        }
    }

    ledFeedback(false);
    return reply(403, "no_match");
}

#endif
