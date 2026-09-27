#pragma once

// UniFi Protect Alarm Manager -> Nuki lock action, handled directly on the Nuki Hub.
//
// Opt-in: compiled only when NUKI_HUB_PROTECT_WEBHOOK is defined. The policy
// (secret, source IP, rules, timings) lives in NVS and is edited on the web
// page "Protect Webhook & Relays" (ForkSettings.h, ForkSettingsWeb.cpp), which
// needs Nuki Hub's web login. src/ProtectWebhookConfig.h is optional and only
// seeds NVS on the first boot.
//
// Route:  POST /protect?r=<rule token>   (body: Protect Alarm Manager JSON)
// Auth:   "Authorization: Bearer <secret>" (Protect's Bearer option, preferred),
//         or &k=<secret> in the URL (unless "Bearer only" is set). The source
//         IP must match the configured one, if set.
// One Protect alarm per fob button + gesture, each with its own ?r= token; the
// rule also checks key, fob MAC, button and gesture. Checks run in this order:
// enabled, source IP, secret, size, clock, JSON, rule match, freshness, BLE
// alive, not busy, replay, per-rule cooldown, then the action is queued (with
// a deadline) through the same ACL as MQTT.

#ifdef NUKI_HUB_PROTECT_WEBHOOK

#include "PsychicHttp.h"
#include "ArduinoJson.h"

class NukiWrapper;

class ProtectWebhook
{
public:
    static void registerRoute(PsychicHttpServer* server, NukiWrapper* nuki);

    // BLE liveness: called each nuki task loop and before every BLE attempt in
    // NukiRetryHandler::retryComm. If a matching press finds it older than the
    // configured BLE stall time, the webhook replies 503 ble_stalled and
    // reboots the board (the task watchdog would otherwise take 300 s).
    static void bleHeartbeat();
    // Milliseconds since the last heartbeat, -1 if the nuki task hasn't run yet.
    static int64_t bleHeartbeatAgeMs();

    // ForkSettings calls this (holding the settings lock) after a save:
    // rules whose match/action changed start with a fresh cooldown.
    static void onRulesChanged(const bool* changed, size_t count);

    // Recent requests, for the settings page (newest first). No secrets or
    // tokens are kept.
    struct Result
    {
        int64_t epoch;     // UTC seconds, 0 if the clock wasn't set
        int64_t uptimeMs;
        uint16_t code;
        const char* result; // string literal
        int8_t rule;        // 0-based slot, -1 = none
        char action[16];
    };
    static constexpr size_t RESULT_LOG_SIZE = 10;
    static size_t recentResults(Result* out, size_t max);

private:
    static esp_err_t handle(PsychicRequest* request, PsychicResponse* resp);
    static bool secretMatches(const String& provided, const char* expected);
    static bool fieldEquals(JsonVariantConst v, const char* want);

    static NukiWrapper* _nuki;
};

#endif
