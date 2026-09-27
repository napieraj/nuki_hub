#pragma once

// UniFi Protect Alarm Manager -> Nuki lock action, handled directly on the Nuki Hub.
//
// Opt-in: compiled only when NUKI_HUB_PROTECT_WEBHOOK is defined. All policy is
// fixed at build time in ProtectWebhookConfig.h (copy ProtectWebhookConfig.h.example),
// so nothing about who may open the door can be changed over the network.
//
// Route:  POST /protect?r=<rule token>   (body: Protect Alarm Manager JSON)
// Auth:   "Authorization: Bearer <secret>" (Protect's Bearer option, preferred),
//         or &k=<secret> in the URL. The source IP must be PROTECT_WEBHOOK_SOURCE_IP.
// One Protect alarm per fob button + gesture, each with its own ?r= token; the
// rule also checks key, fob MAC, button and gesture. Checks run in this order:
// source IP, secret, size, clock, JSON, rule match, freshness, BLE alive, not
// busy, replay, per-rule cooldown, then the action is queued (with a deadline)
// through the same ACL as MQTT.

#ifdef NUKI_HUB_PROTECT_WEBHOOK

#include "PsychicHttp.h"
#include "ArduinoJson.h"

class NukiWrapper;

class ProtectWebhook
{
public:
    static void registerRoute(PsychicHttpServer* server, NukiWrapper* nuki);

    // BLE liveness: called each nuki task loop and before every BLE attempt in
    // NukiRetryHandler::retryComm. If a matching press finds it older than
    // PROTECT_WEBHOOK_BLE_STALL_MS, the webhook replies 503 ble_stalled and
    // reboots the board (the task watchdog would otherwise take 300 s).
    static void bleHeartbeat();

private:
    static esp_err_t handle(PsychicRequest* request, PsychicResponse* resp);
    static bool secretMatches(const String& provided);
    static bool macMatches(const char* a, const char* b);
    static bool fieldEquals(JsonVariantConst v, const char* want);
    static bool eventSeen(const char* eventId);
    static void rememberEvent(const char* eventId);

    static NukiWrapper* _nuki;
};

#endif
