#pragma once

// Fork settings (UniFi Protect webhook, relays, hardening options), stored in
// NVS namespace "forkcfg" as one versioned blob (key "cfg"), edited on the web
// page "Protect Webhook & Relays" (ForkSettingsWeb.cpp).
//
// Precedence: NVS wins. If src/ProtectWebhookConfig.h exists at build time,
// its values seed NVS once, on the first boot that finds no "cfg" key. After
// that the header is ignored. Without the header and without saved settings,
// the webhook is off until configured on the web page.
//
// The settings are kept in RAM and guarded by a FreeRTOS mutex. The webhook
// holds it (ReadLock) for the whole request, so a save waits for a running
// request and a request never sees a half-written rule table. Saving stores
// the blob first, then swaps the RAM copy under the mutex (live, no reboot).
//
// Not part of Nuki Hub's export/import (different namespace, not in
// PreferencesKeys.h): the secret and tokens never leave the board that way.
// Nuki Hub's factory reset erases the namespace too.

#ifdef NUKI_HUB_PROTECT_WEBHOOK

#include "ForkSettingsLogic.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace ForkSettings
{
    using Settings = ForkSettingsLogic::Settings;
    using Rule = ForkSettingsLogic::Rule;

    enum class Source : uint8_t
    {
        Defaults, // nothing stored yet, no header: webhook off
        Header,   // seeded from ProtectWebhookConfig.h at this boot
        Nvs       // loaded from NVS
    };

    // Webhook state after the last load or save.
    struct State
    {
        bool active;       // requests are processed
        char reason[200];  // why not active, or a note ("no enabled rules")
    };

    // At boot, after Preferences are up and before the web server starts.
    void begin();

    ForkSettingsLogic::Caps caps();
    bool headerPresent();
    Source source();

    // A consistent copy (for the web page). Settings is ~4 KB: pass a heap object.
    bool snapshot(Settings& out, TickType_t wait = pdMS_TO_TICKS(3000));
    void state(State& out);

    // Validate, store in NVS, then apply to the running webhook. On failure
    // nothing changes and err says why.
    bool save(const Settings& s, char* err, size_t errLen);

    // Serial console "forkcfg unlock": switches off the settings lock and
    // "require TOTP". Physical access (USB) is the recovery path.
    bool serialUnlock();

    // Master switch off, nothing else changed and no validation, so it works
    // even with the settings lock on or a stored config that no longer
    // validates (web page button, serial "forkcfg off").
    bool switchOff();

    // Nuki Hub factory reset: erase the namespace (the header, if any, seeds
    // again at the next boot).
    void factoryReset();

    // Incremented by every load or save (cheap change check for other tasks).
    uint32_t generation();

    // The webhook holds this for one request.
    class ReadLock
    {
    public:
        explicit ReadLock(TickType_t wait);
        ~ReadLock();
        ReadLock(const ReadLock&) = delete;
        ReadLock& operator=(const ReadLock&) = delete;

        bool locked() const { return _locked; }
        const Settings& settings() const;
        bool active() const;

    private:
        bool _locked = false;
    };
}

#endif
