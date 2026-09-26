#pragma once

// UniFi Protect Alarm Manager -> Nuki lock action, handled directly on the Nuki Hub.
//
// Opt-in: compiled only when NUKI_HUB_PROTECT_WEBHOOK is defined. All policy is
// fixed at build time in ProtectWebhookConfig.h (copy ProtectWebhookConfig.h.example),
// so nothing about who may open the door can be changed over the network.
//
// Route:  POST /protect?k=<secret>   (body: Protect Alarm Manager JSON)

#ifdef NUKI_HUB_PROTECT_WEBHOOK

#include "PsychicHttp.h"

class NukiWrapper;

class ProtectWebhook
{
public:
    static void registerRoute(PsychicHttpServer* server, NukiWrapper* nuki);

private:
    static esp_err_t handle(PsychicRequest* request, PsychicResponse* resp);
    static bool secretMatches(const String& provided);
    static bool macMatches(const char* a, const char* b);
    static bool userAllowed(const char* guid);
    static bool eventSeen(const char* eventId);
    static void rememberEvent(const char* eventId);

    static NukiWrapper* _nuki;
};

#endif
