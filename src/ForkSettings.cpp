#ifdef NUKI_HUB_PROTECT_WEBHOOK

#include "ForkSettings.h"
#include "ProtectWebhook.h"
#include "Logger.h"
#include "util/NukiHelper.h"
#include <Preferences.h>
#include <memory>
#ifdef NUKI_HUB_WAVESHARE_8DI8RO
#include "WaveshareBoard.h"
#endif

using namespace ForkSettingsLogic;

// --- Optional compile-time seed ---------------------------------------------
// src/ProtectWebhookConfig.h (git-ignored, template .example) is only read
// here, and only used on the first boot without stored settings. The checks
// below are the same as before it became optional: whatever it holds goes
// live on a fresh board, so a placeholder or broken rule still stops the build.
#if __has_include("ProtectWebhookConfig.h")
#include "ProtectWebhookConfig.h"
#define FORK_SETTINGS_HAVE_HEADER 1

#ifndef PROTECT_WEBHOOK_ACTION_DEADLINE_MS
#define PROTECT_WEBHOOK_ACTION_DEADLINE_MS 8000
#endif
#ifndef PROTECT_WEBHOOK_RELAY_PULSE_MS
#define PROTECT_WEBHOOK_RELAY_PULSE_MS 3000
#endif
#ifndef PROTECT_WEBHOOK_BLE_STALL_MS
#define PROTECT_WEBHOOK_BLE_STALL_MS 30000
#endif
#ifndef PROTECT_WEBHOOK_MAX_SKEW_MS
#define PROTECT_WEBHOOK_MAX_SKEW_MS 15000
#endif
#ifndef PROTECT_WEBHOOK_COOLDOWN_MS
#define PROTECT_WEBHOOK_COOLDOWN_MS 10000
#endif

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

static_assert(sizeof(PROTECT_WEBHOOK_RULES) / sizeof(PROTECT_WEBHOOK_RULES[0]) <= MAX_RULES,
              "ProtectWebhookConfig.h: at most 12 rules (ForkSettingsLogic::MAX_RULES)");
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
static_assert(PROTECT_WEBHOOK_RELAY_PULSE_MS >= 100 && PROTECT_WEBHOOK_RELAY_PULSE_MS <= 30000,
              "PROTECT_WEBHOOK_RELAY_PULSE_MS must be 100..30000 (milliseconds)");
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
#endif // __has_include("ProtectWebhookConfig.h")

namespace
{
    constexpr const char* NVS_NAMESPACE = "forkcfg";
    constexpr const char* NVS_KEY = "cfg";

    SemaphoreHandle_t mutex = nullptr;
    Settings current;                 // guarded by mutex
    ForkSettings::State currentState; // guarded by mutex
    ForkSettings::Source loadedFrom = ForkSettings::Source::Defaults;
    volatile uint32_t settingsGeneration = 0;

    // Settings is ~4 KB: never on a task stack.
    std::unique_ptr<Settings> newSettings()
    {
        std::unique_ptr<Settings> s(new (std::nothrow) Settings);
        if(s)
        {
            setDefaults(*s);
        }
        return s;
    }

    void computeState(const Settings& s, ForkSettings::State& st)
    {
        char err[sizeof(st.reason)];
        st.reason[0] = 0;
        if(!validate(s, ForkSettings::caps(), err, sizeof(err)))
        {
            st.active = false;
            snprintf(st.reason, sizeof(st.reason), "invalid settings: %s", err);
        }
        else if(!s.enabled)
        {
            st.active = false;
            snprintf(st.reason, sizeof(st.reason), "switched off in the settings");
        }
        else
        {
            st.active = true;
            if(enabledRuleCount(s) == 0)
            {
                snprintf(st.reason, sizeof(st.reason), "no enabled rules, every press gets 403 no_match");
            }
        }
    }

    bool store(const Settings& s)
    {
        std::unique_ptr<uint8_t[]> buf(new (std::nothrow) uint8_t[MAX_BLOB_SIZE]);
        if(!buf)
        {
            return false;
        }
        const size_t n = serialize(s, buf.get(), MAX_BLOB_SIZE);
        Preferences p;
        if(n == 0 || !p.begin(NVS_NAMESPACE, false))
        {
            return false;
        }
        const bool ok = p.putBytes(NVS_KEY, buf.get(), n) == n;
        p.end();
        return ok;
    }

    // Returns false if nothing (or nothing usable) is stored.
    bool load(Settings& out)
    {
        Preferences p;
        if(!p.begin(NVS_NAMESPACE, true))
        {
            return false; // namespace doesn't exist yet
        }
        bool ok = false;
        const size_t n = p.isKey(NVS_KEY) ? p.getBytesLength(NVS_KEY) : 0;
        if(n > 0 && n <= MAX_BLOB_SIZE)
        {
            std::unique_ptr<uint8_t[]> buf(new (std::nothrow) uint8_t[n]);
            ok = buf && p.getBytes(NVS_KEY, buf.get(), n) == n && deserialize(buf.get(), n, out);
        }
        else if(n > 0)
        {
            Log->println("Fork settings: stored blob too large, ignored");
        }
        p.end();
        return ok;
    }

    bool storedBlobExists()
    {
        Preferences p;
        if(!p.begin(NVS_NAMESPACE, true))
        {
            return false;
        }
        const bool exists = p.isKey(NVS_KEY);
        p.end();
        return exists;
    }

    // Swap in new settings; caller holds the mutex.
    void applyLocked(const Settings& s)
    {
        bool changed[MAX_RULES];
        for(size_t i = 0; i < MAX_RULES; i++)
        {
            changed[i] = ruleMatchChanged(current.rules[i], s.rules[i]);
        }
        current = s;
        computeState(current, currentState);
        settingsGeneration = settingsGeneration + 1;
        ProtectWebhook::onRulesChanged(changed, MAX_RULES);
    }

#ifdef FORK_SETTINGS_HAVE_HEADER
    void seedFromHeader(Settings& s)
    {
        setDefaults(s);
        s.enabled = true;
        if(!isPlaceholder(PROTECT_WEBHOOK_SECRET))
        {
            copyText(s.secret, sizeof(s.secret), PROTECT_WEBHOOK_SECRET);
        }
        copyText(s.sourceIp, sizeof(s.sourceIp), PROTECT_WEBHOOK_SOURCE_IP);
        s.maxSkewMs = PROTECT_WEBHOOK_MAX_SKEW_MS;
        s.cooldownMs = PROTECT_WEBHOOK_COOLDOWN_MS;
        s.actionDeadlineMs = PROTECT_WEBHOOK_ACTION_DEADLINE_MS;
        s.bleStallMs = PROTECT_WEBHOOK_BLE_STALL_MS;
        s.relayPulseMs = PROTECT_WEBHOOK_RELAY_PULSE_MS;
        s.relayCount = ForkSettings::caps().relays;
#ifdef PROTECT_WEBHOOK_ALLOW_BROAD_RULES
        s.allowBroadRules = true;
#endif
        size_t i = 0;
        for(const ProtectRule& h : PROTECT_WEBHOOK_RULES)
        {
            Rule& r = s.rules[i++];
            // A placeholder token is dropped and its rule seeded disabled.
            const bool placeholder = isPlaceholder(h.token);
            r.enabled = !placeholder;
            bool ok = placeholder || copyText(r.token, sizeof(r.token), h.token);
            ok = copyText(r.key, sizeof(r.key), h.key) && ok;
            ok = copyText(r.device, sizeof(r.device), h.device) && ok;
            ok = copyText(r.field, sizeof(r.field), h.field) && ok;
            ok = copyText(r.value, sizeof(r.value), h.value) && ok;
            ok = copyText(r.field2, sizeof(r.field2), h.field2) && ok;
            ok = copyText(r.value2, sizeof(r.value2), h.value2) && ok;
            const char* action = canonicalAction(h.action, s.relayCount);
            ok = copyText(r.action, sizeof(r.action), action != nullptr ? action : h.action) && ok;
            snprintf(r.name, sizeof(r.name), "Rule %u (from header)", (unsigned)i);
            if(!ok)
            {
                Log->printf("Fork settings: header rule %u has a field that is too long, seeded disabled\n", (unsigned)i);
                r.enabled = false;
            }
        }
    }
#endif
}

ForkSettingsLogic::Caps ForkSettings::caps()
{
#ifdef NUKI_HUB_WAVESHARE_8DI8RO
    return { (uint8_t)WaveshareBoard::RELAY_COUNT, (uint8_t)WaveshareBoard::DIGITAL_INPUT_COUNT };
#else
    return { 0, 0 };
#endif
}

bool ForkSettings::headerPresent()
{
#ifdef FORK_SETTINGS_HAVE_HEADER
    return true;
#else
    return false;
#endif
}

ForkSettings::Source ForkSettings::source()
{
    return loadedFrom;
}

void ForkSettings::begin()
{
    if(mutex == nullptr)
    {
        mutex = xSemaphoreCreateMutex();
    }

    // The action list in ForkSettingsLogic.h must match Nuki Hub's.
    for(const char* a : LOCK_ACTIONS)
    {
        if((int)NukiHelper::lockActionToEnum(a) == 0xff)
        {
            Log->printf("Fork settings: lock action '%s' unknown to Nuki Hub\n", a);
        }
    }

    auto s = newSettings();
    if(!s || mutex == nullptr)
    {
        Log->println("Fork settings: out of memory, webhook off");
        return;
    }

    if(load(*s))
    {
        loadedFrom = Source::Nvs;
        Log->println("Fork settings loaded from NVS");
    }
    else
    {
        if(storedBlobExists())
        {
            // Damaged or from a newer firmware: fail closed, keep the blob
            // until the user saves (which overwrites it).
            Log->println("Fork settings: stored settings unreadable, webhook off until saved again");
            setDefaults(*s);
        }
#ifdef FORK_SETTINGS_HAVE_HEADER
        else
        {
            seedFromHeader(*s);
            loadedFrom = Source::Header;
            Log->println(store(*s) ? "Fork settings seeded from ProtectWebhookConfig.h"
                                   : "Fork settings: seeding NVS failed, using the header values for this boot");
        }
#endif
    }

    xSemaphoreTake(mutex, portMAX_DELAY);
    current = *s;
    computeState(current, currentState);
    settingsGeneration = settingsGeneration + 1;
    const bool on = currentState.active;
    char reason[sizeof(currentState.reason)];
    memcpy(reason, currentState.reason, sizeof(reason));
    xSemaphoreGive(mutex);

    if(on)
    {
        Log->printf("Protect webhook enabled%s%s\n", reason[0] ? ": " : "", reason);
    }
    else
    {
        Log->printf("Protect webhook disabled: %s\n", reason);
    }
}

bool ForkSettings::snapshot(Settings& out, TickType_t wait)
{
    if(mutex == nullptr || xSemaphoreTake(mutex, wait) != pdTRUE)
    {
        return false;
    }
    out = current;
    xSemaphoreGive(mutex);
    return true;
}

void ForkSettings::state(State& out)
{
    if(mutex == nullptr || xSemaphoreTake(mutex, pdMS_TO_TICKS(3000)) != pdTRUE)
    {
        out.active = false;
        snprintf(out.reason, sizeof(out.reason), "settings busy");
        return;
    }
    out = currentState;
    xSemaphoreGive(mutex);
}

bool ForkSettings::save(const Settings& s, char* err, size_t errLen)
{
    if(!validateForSave(s, caps(), err, errLen))
    {
        return false;
    }
    if(mutex == nullptr || xSemaphoreTake(mutex, pdMS_TO_TICKS(5000)) != pdTRUE)
    {
        snprintf(err, errLen, "Settings busy (a webhook request is running), try again");
        return false;
    }
    // Store while holding the mutex, so two saves can't interleave and NVS
    // always matches what runs.
    const bool stored = store(s);
    if(stored)
    {
        applyLocked(s);
        loadedFrom = Source::Nvs;
    }
    xSemaphoreGive(mutex);
    if(!stored)
    {
        snprintf(err, errLen, "Could not write the settings to NVS (full?); nothing changed");
        return false;
    }
    Log->println("Fork settings saved and applied");
    return true;
}

namespace
{
    // Store and apply a modified copy of the current settings, without
    // validation (only for changes that can't make them less safe).
    template<typename F> bool modifyUnchecked(F change)
    {
        if(mutex == nullptr || xSemaphoreTake(mutex, pdMS_TO_TICKS(5000)) != pdTRUE)
        {
            return false;
        }
        auto s = newSettings();
        bool ok = false;
        if(s)
        {
            *s = current;
            change(*s);
            ok = store(*s);
            if(ok)
            {
                applyLocked(*s);
                loadedFrom = ForkSettings::Source::Nvs;
            }
        }
        xSemaphoreGive(mutex);
        return ok;
    }
}

bool ForkSettings::serialUnlock()
{
    // A stored config that fails validation must still be unlockable to be fixed.
    const bool ok = modifyUnchecked([](Settings& s)
    {
        s.lockDi = 0;
        s.requireTotp = false;
    });
    Log->println(ok ? "Fork settings: unlocked from the serial console" : "Fork settings: unlock failed");
    return ok;
}

bool ForkSettings::switchOff()
{
    const bool ok = modifyUnchecked([](Settings& s) { s.enabled = false; });
    Log->println(ok ? "Protect webhook switched off" : "Protect webhook: switching off failed");
    return ok;
}

void ForkSettings::factoryReset()
{
    Preferences p;
    if(p.begin(NVS_NAMESPACE, false))
    {
        p.clear();
        p.end();
    }
}

uint32_t ForkSettings::generation()
{
    return settingsGeneration;
}

ForkSettings::ReadLock::ReadLock(TickType_t wait)
{
    _locked = mutex != nullptr && xSemaphoreTake(mutex, wait) == pdTRUE;
}

ForkSettings::ReadLock::~ReadLock()
{
    if(_locked)
    {
        xSemaphoreGive(mutex);
    }
}

const ForkSettings::Settings& ForkSettings::ReadLock::settings() const
{
    return current;
}

bool ForkSettings::ReadLock::active() const
{
    return _locked && currentState.active;
}

#endif
