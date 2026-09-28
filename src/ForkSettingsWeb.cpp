// Web page "Protect Webhook & Relays" (/get?page=forkcfg, /post?page=forkcfg):
// all fork settings, stored in NVS by ForkSettings. Members of WebCfgServer so
// it reuses its page helpers; reached only through WebCfgServer's /get and
// /post handlers, which run Nuki Hub's authentication (login, Duo/TOTP, and
// "Require MFA ... for all sensitive operations") before dispatching here.
//
// Rules for this page:
//  - The secret and tokens are write-only: never rendered, never logged.
//    The page shows "set, N characters"; an empty field keeps the value.
//  - POST needs this page's own CSRF token (128 bit, per boot, rotated after
//    each save) and, if the browser sends one, a same-host Origin header.
//  - Optional hardening (off by default): settings lock via a digital input,
//    TOTP code on every save, require source IP, Bearer only, broad rules.

#if defined(NUKI_HUB_PROTECT_WEBHOOK) && !defined(NUKI_HUB_UPDATER)

#include "WebCfgServer.h"
#include "ForkSettings.h"
#include "ProtectWebhook.h"
#include "ProtectWebhookLogic.h"
#include "PreferencesKeys.h"
#include "EspMillis.h"
#include "Logger.h"
#include "Gpio.h"
#include "WaveshareBoard.h"
#ifdef NUKI_HUB_EMBEDDED_LOCK_MQTT
#include "LockMqttServer.h"
#include "LockMqttLogic.h"
#include "../lib/nuki_ble/src/NukiLockUtils.h"
#endif
#include <memory>
#include <time.h>

using namespace ForkSettingsLogic;

extern bool timeSynced;

namespace
{
    // CSRF token for this page's POST (32 hex chars).
    char csrfToken[33] = "";

    void rotateCsrfToken()
    {
        for(int i = 0; i < 4; i++)
        {
            snprintf(csrfToken + i * 8, 9, "%08lx", (unsigned long)esp_random());
        }
    }

    const char* csrf()
    {
        if(csrfToken[0] == 0)
        {
            rotateCsrfToken();
        }
        return csrfToken;
    }

    bool csrfMatches(const String& provided)
    {
        return provided.length() == 32 && ProtectWebhookLogic::ctEquals(provided.c_str(), csrf());
    }

    // If the browser says where the form came from, it must be this host.
    bool originAllowed(PsychicRequest* request)
    {
        if(!request->hasHeader("Origin"))
        {
            return true;
        }
        String origin = request->header("Origin");
        const int scheme = origin.indexOf("://");
        if(scheme < 0)
        {
            return false;
        }
        origin = origin.substring(scheme + 3);
        return origin.equalsIgnoreCase(request->header("Host"));
    }

    String esc(const char* s)
    {
        const size_t cap = strlen(s) * 6 + 1;
        std::unique_ptr<char[]> buf(new (std::nothrow) char[cap]);
        if(!buf || !htmlEscape(s, buf.get(), cap))
        {
            return String();
        }
        return String(buf.get());
    }

    std::unique_ptr<Settings> allocSettings()
    {
        std::unique_ptr<Settings> s(new (std::nothrow) Settings);
        if(s)
        {
            setDefaults(*s);
        }
        return s;
    }

    // Settings lock: saving needs digital input DIn active (closed to DGND).
    // Only a DI without a Nuki Hub GPIO role counts: a pin with a role could
    // be driven (output role) or would also trigger a lock action.
    enum class DiState { Off, Active, Inactive, HasRole };

    DiState lockInputState(uint8_t di, Gpio* gpio)
    {
        if(di == 0)
        {
            return DiState::Off;
        }
#ifdef NUKI_HUB_WAVESHARE_8DI8RO
        const int pin = WaveshareBoard::digitalInputGpio(di);
        if(gpio == nullptr || gpio->getPinRole(pin) != PinRole::Disabled)
        {
            return DiState::HasRole;
        }
        pinMode(pin, INPUT_PULLUP);
        return digitalRead(pin) == LOW ? DiState::Active : DiState::Inactive;
#else
        (void)gpio;
        return DiState::HasRole;
#endif
    }

    const char* resultTime(const ProtectWebhook::Result& r, char* buf, size_t len)
    {
        if(r.epoch > 0)
        {
            const time_t t = (time_t)r.epoch;
            struct tm utc;
            gmtime_r(&t, &utc);
            strftime(buf, len, "%Y-%m-%d %H:%M:%S UTC", &utc);
        }
        else
        {
            snprintf(buf, len, "uptime %lld s", (long long)(r.uptimeMs / 1000));
        }
        return buf;
    }

    void printBanner(PsychicStreamResponse* response, const String& text, bool error)
    {
        response->print("<table><tbody><tr><td colspan=\"2\" style=\"border: 0; color: ");
        response->print(error ? "red" : "green");
        response->print("; font-size: 20px; font-weight: bold; text-align: center;\">");
        response->print(text);
        response->print("</td></tr></tbody></table>");
    }

    void printGenerateRow(PsychicStreamResponse* response, const char* field, int bytes)
    {
        response->print("<tr><td></td><td><input type=\"button\" value=\"Generate random\" onclick=\"fsGen('");
        response->print(field);
        response->print("',");
        response->print(bytes);
        response->print(")\"/> copy it before saving: it is not shown again</td></tr>");
    }

    String writeOnlyStatus(const char* value)
    {
        if(!isSet(value))
        {
            return "not set";
        }
        return "set, " + String((unsigned)strlen(value)) + " characters";
    }

    String param(PsychicRequest* request, const char* name)
    {
        if(!request->hasParam(name))
        {
            return String();
        }
        return request->getParam(name)->value();
    }

    String duration(int64_t ms)
    {
        const long s = (long)(ms / 1000);
        if(s < 120)
        {
            return String(s) + " s";
        }
        if(s < 7200)
        {
            return String(s / 60) + " min";
        }
        return String(s / 3600) + " h " + String((s % 3600) / 60) + " min";
    }
}

bool WebCfgServer::forkSettingsParse(PsychicRequest* request, Settings& s, char* err, size_t errLen)
{
    s.enabled = isParameterTrue(request, "FS_EN");
    if(!applyWriteOnly(s.secret, sizeof(s.secret), param(request, "FS_SECRET").c_str(), isParameterTrue(request, "FS_SECRET_CLR")))
    {
        snprintf(err, errLen, "The secret is too long (max %u characters)", (unsigned)LEN_SECRET);
        return false;
    }
    if(!setTrimmed(s.sourceIp, sizeof(s.sourceIp), param(request, "FS_SRCIP").c_str()))
    {
        snprintf(err, errLen, "The source IP is too long");
        return false;
    }

    struct Num
    {
        const char* name;
        const char* label;
        uint32_t* dst;
    };
    const Num nums[] = {
        { "FS_SKEW", "Max. clock skew", &s.maxSkewMs },
        { "FS_COOL", "Cooldown", &s.cooldownMs },
        { "FS_DEADLINE", "Action deadline", &s.actionDeadlineMs },
        { "FS_BLESTALL", "BLE stall", &s.bleStallMs },
        { "FS_PULSE", "Relay pulse", &s.relayPulseMs },
    };
    for(const Num& n : nums)
    {
        if(!request->hasParam(n.name))
        {
            continue; // not on this board's form (relays)
        }
        if(!parseUint(param(request, n.name).c_str(), *n.dst))
        {
            snprintf(err, errLen, "%s must be a whole number of milliseconds", n.label);
            return false;
        }
    }
    uint32_t v;
    if(request->hasParam("FS_RELAYS"))
    {
        if(!parseUint(param(request, "FS_RELAYS").c_str(), v) || v > 255)
        {
            snprintf(err, errLen, "Invalid relay count");
            return false;
        }
        s.relayCount = (uint8_t)v;
    }
    if(request->hasParam("FS_LOCKDI"))
    {
        if(!parseUint(param(request, "FS_LOCKDI").c_str(), v) || v > 255)
        {
            snprintf(err, errLen, "Invalid settings lock input");
            return false;
        }
        s.lockDi = (uint8_t)v;
    }
#ifdef NUKI_HUB_EMBEDDED_LOCK_MQTT
    if(request->hasParam("FS_LM_SEEN"))
    {
        s.lockMqttEnabled = isParameterTrue(request, "FS_LM_EN");
        s.skipRedundant = isParameterTrue(request, "FS_LM_SKIP");
        if(!setTrimmed(s.lockMqttUser, sizeof(s.lockMqttUser), param(request, "FS_LM_USER").c_str()) ||
           !setTrimmed(s.lockMqttClientId, sizeof(s.lockMqttClientId), param(request, "FS_LM_CID").c_str()) ||
           !applyWriteOnly(s.lockMqttPass, sizeof(s.lockMqttPass), param(request, "FS_LM_PASS").c_str(), isParameterTrue(request, "FS_LM_PASS_CLR")))
        {
            snprintf(err, errLen, "Lock MQTT: user name, password and client ID are at most %u characters", (unsigned)LEN_MQTT_USER);
            return false;
        }
        if(!parseUint(param(request, "FS_LM_SILENCE").c_str(), s.lockSilenceMs))
        {
            snprintf(err, errLen, "Lock MQTT: 'heard from within' must be a whole number of milliseconds");
            return false;
        }
    }
#endif
    s.bearerOnly = isParameterTrue(request, "FS_BEARER");
    s.requireSourceIp = isParameterTrue(request, "FS_REQIP");
    s.allowBroadRules = isParameterTrue(request, "FS_BROAD");
    s.requireTotp = isParameterTrue(request, "FS_REQTOTP");

    char name[16];
    for(size_t i = 0; i < MAX_RULES; i++)
    {
        Rule& r = s.rules[i];
        const unsigned n = (unsigned)i;
        snprintf(name, sizeof(name), "R%u_SEEN", n);
        if(!request->hasParam(name))
        {
            continue; // slot not on the form: keep it
        }
        snprintf(name, sizeof(name), "R%u_EN", n);
        r.enabled = isParameterTrue(request, name);

        snprintf(name, sizeof(name), "R%u_TOK", n);
        const String token = param(request, name);
        snprintf(name, sizeof(name), "R%u_TOKCLR", n);
        if(!applyWriteOnly(r.token, sizeof(r.token), token.c_str(), isParameterTrue(request, name)))
        {
            snprintf(err, errLen, "Rule %u: the token is too long (max %u characters)", n + 1, (unsigned)LEN_TOKEN);
            return false;
        }

        struct Text
        {
            const char* suffix;
            const char* label;
            char* dst;
            size_t cap;
        };
        const Text texts[] = {
            { "NAME", "name", r.name, sizeof(r.name) },
            { "KEY", "key", r.key, sizeof(r.key) },
            { "DEV", "device", r.device, sizeof(r.device) },
            { "F1", "field", r.field, sizeof(r.field) },
            { "V1", "value", r.value, sizeof(r.value) },
            { "F2", "field2", r.field2, sizeof(r.field2) },
            { "V2", "value2", r.value2, sizeof(r.value2) },
            { "ACT", "action", r.action, sizeof(r.action) },
        };
        for(const Text& t : texts)
        {
            snprintf(name, sizeof(name), "R%u_%s", n, t.suffix);
            if(!setTrimmed(t.dst, t.cap, param(request, name).c_str()))
            {
                snprintf(err, errLen, "Rule %u: %s is too long (max %u characters)", n + 1, t.label, (unsigned)(t.cap - 1));
                return false;
            }
        }
        const char* canonical = canonicalAction(r.action, MAX_RELAYS);
        if(canonical != nullptr)
        {
            copyText(r.action, sizeof(r.action), canonical);
        }
    }
    return true;
}

esp_err_t WebCfgServer::processForkSettings(PsychicRequest* request, PsychicResponse* resp)
{
    const String ip = request->client()->localIP().toString();
    const bool mfaApproval = _preferences->getBool(preference_cred_duo_approval, false) &&
                             (_importExport->getTOTPEnabled() || _duoEnabled);

    if(!request->hasParam("FORKCFG"))
    {
        // Nuki Hub's MFA approval page re-posts without the form data. The
        // approval it just used is granted again, once, so the next save of
        // the real form from this client passes.
        if(mfaApproval)
        {
            _importExport->_sessionsOpts[ip + "approve"] = true;
        }
        return buildForkSettingsHtml(request, resp, nullptr,
                                     "Approved, but the form data was not re-sent. Enter your changes again and save.", true);
    }
    if(!csrfMatches(param(request, "FSCSRF")) || !originAllowed(request))
    {
        Log->println("Fork settings: save refused (stale page or cross-site request)");
        return buildForkSettingsHtml(request, resp, nullptr, "Refused: the page was stale or the request came from another site. Reload and try again.", true);
    }

    auto stored = allocSettings();
    auto next = allocSettings();
    if(!stored || !next || !ForkSettings::snapshot(*stored))
    {
        return buildConfirmHtml(request, resp, "Settings busy or out of memory, try again.", 3, true, "/get?page=forkcfg");
    }

    // TOTP on every save (option). Checked before anything else changes.
    if(stored->requireTotp)
    {
        if(!_importExport->getTOTPEnabled())
        {
            return buildForkSettingsHtml(request, resp, nullptr,
                                         "'Require TOTP to save' is on, but TOTP is not enabled under Credentials. "
                                         "Enable it there, or run 'forkcfg unlock' on the USB console.", true);
        }
        String code = param(request, "totpkey");
        if(!timeSynced || code.length() == 0 || !_importExport->checkTOTP(&code))
        {
            return buildForkSettingsHtml(request, resp, nullptr, "Wrong or missing TOTP code. Nothing was saved.", true);
        }
    }

    if(param(request, "FS_ACTION") == "off")
    {
        // Allowed even with the settings lock on: it only makes things safer.
        const bool ok = ForkSettings::switchOff();
        if(ok)
        {
            rotateCsrfToken();
        }
        return buildForkSettingsHtml(request, resp, nullptr, ok ? "Webhook switched off." : "Switching off failed.", !ok);
    }

    const DiState lock = lockInputState(stored->lockDi, _gpio);
    if(lock == DiState::Inactive || lock == DiState::HasRole)
    {
        return buildForkSettingsHtml(request, resp, nullptr, "The settings are locked. Nothing was saved.", true);
    }

    *next = *stored;
    char err[200] = "";
    bool ok = forkSettingsParse(request, *next, err, sizeof(err));

    if(ok && next->requireTotp && !stored->requireTotp && !_importExport->getTOTPEnabled())
    {
        snprintf(err, sizeof(err), "'Require TOTP to save' needs TOTP enabled under Credentials first");
        ok = false;
    }
    if(ok && next->lockDi != 0 && next->lockDi != stored->lockDi)
    {
        // Turning the lock on (or moving it) only works with the input
        // active right now: proves the wiring before it can lock you out.
        const DiState state = lockInputState(next->lockDi, _gpio);
        if(state == DiState::HasRole)
        {
            snprintf(err, sizeof(err), "DI%u has a role in GPIO Configuration; pick a DI without one", (unsigned)next->lockDi);
            ok = false;
        }
        else if(state != DiState::Active)
        {
            snprintf(err, sizeof(err), "To turn on the settings lock, hold DI%u active while saving", (unsigned)next->lockDi);
            ok = false;
        }
    }
    if(ok)
    {
        ok = ForkSettings::save(*next, err, sizeof(err));
    }
    if(!ok)
    {
        // Show what was submitted so it can be fixed; new secret/token values
        // are write-only and must be entered again.
        return buildForkSettingsHtml(request, resp, next.get(),
                                     "Not saved: " + esc(err) + ". New secret/token values were not kept; enter them again.", true);
    }
    rotateCsrfToken();
    return buildForkSettingsHtml(request, resp, nullptr, "Saved and applied.", false);
}

#ifdef NUKI_HUB_EMBEDDED_LOCK_MQTT
void WebCfgServer::buildLockMqttSection(PsychicStreamResponse* response, const Settings& s, const Settings& stored)
{
    LockMqttServer::Status st;
    LockMqttServer::status(st);

    response->print("<h3>Nuki lock MQTT (official MQTT API, built-in server)</h3>");
    response->print("<p>In the Nuki app (lock &rarr; Settings &rarr; Features &amp; Configuration &rarr; MQTT): "
                    "host = this board's IP, the user name and password below, <b>Allow locking</b> on, "
                    "<b>Home Assistant discovery</b> off. Only the lock can log in; Nuki Hub talks to it in-process. "
                    "Lock actions go over MQTT while the lock is connected, otherwise over BLE.</p><table>");
    String server = !st.enabled ? String("off") :
                    st.listening ? "listening on port " + String((unsigned)LockMqttServer::PORT) :
                    String("<span class=\"warning\">not listening (network not up yet?)</span>");
    printParameter(response, "Server", server.c_str());
    if(st.connected)
    {
        String conn = "<span style=\"color: green\">yes</span>, for " + duration(espMillis() - st.connectedSinceMs) +
                      ", client '" + esc(st.clientId) + "' from " + String(st.peer) + ", keepalive " + String((unsigned)st.keepAliveS) + " s";
        printParameter(response, "Lock connected", conn.c_str());
        printParameter(response, "Last message from the lock", (duration(st.lastRxAgeMs) + " ago").c_str());
        if(st.lockState >= 0)
        {
            char state[30] = {0};
            NukiLock::lockstateToString((NukiLock::LockState)st.lockState, state);
            String text = String(state) + " (reported " + duration(st.stateAgeMs) + " ago)";
            if(st.doorState >= 0)
            {
                char door[30] = {0};
                NukiLock::doorSensorStateToString((NukiLock::DoorSensorState)st.doorState, door);
                text += ", door sensor: " + String(door);
            }
            printParameter(response, "Last state", text.c_str());
        }
        else
        {
            printParameter(response, "Last state", "none yet in this session");
        }
    }
    else
    {
        printParameter(response, "Lock connected", st.enabled ? "no" : "no (server off)");
    }
    const char* actions = !st.connected ? "BLE (lock not connected)" :
                          !st.lockActionSubscribed ? "BLE (the lock doesn't listen to lockAction: turn on 'Allow locking' in the Nuki app)" :
                          !st.hybridReady ? "BLE until the next reboot (hybrid mode was off at boot)" :
                          "MQTT, BLE if the lock doesn't confirm within 2 s";
    printParameter(response, "Lock actions go over", actions);
    String counts = String((unsigned long)st.sessions) + " sessions, " + String((unsigned long)st.takeovers) + " takeovers, " +
                    String((unsigned long)st.refused) + " refused since boot";
    printParameter(response, "Connections", counts.c_str());
    if(st.lastRefusal[0] != 0)
    {
        printParameter(response, "Last refused", esc(st.lastRefusal).c_str());
    }
    if(st.stackFreeBytes > 0)
    {
        printParameter(response, "Server task stack unused", (String((unsigned long)st.stackFreeBytes) + " bytes").c_str());
    }
    response->print("</table><table>");
    response->print("<input type=\"hidden\" name=\"FS_LM_SEEN\" value=\"1\">");
    printCheckBox(response, "FS_LM_EN", "Enable the lock MQTT server (port 1883)", s.lockMqttEnabled, "");
    printInputField(response, "FS_LM_USER", "User name (as in the Nuki app, max. 32)", esc(s.lockMqttUser).c_str(), LEN_MQTT_USER, "autocomplete=\"off\"");
    printParameter(response, "Password", writeOnlyStatus(stored.lockMqttPass).c_str());
    printInputField(response, "FS_LM_PASS", "New password (empty = keep; max. 32)", "", LEN_MQTT_PASS, "autocomplete=\"off\"");
    printGenerateRow(response, "FS_LM_PASS", 12);
    printCheckBox(response, "FS_LM_PASS_CLR", "Clear the password", false, "");
    char hint[96];
    const uint32_t nukiId = _preferences->getUInt(preference_nuki_id_lock, 0);
    if(nukiId != 0)
    {
        char p[24];
        LockMqttLogic::officialPath(nukiId, p, sizeof(p));
        snprintf(hint, sizeof(hint), "Expected client ID (optional; the Nuki lock uses Nuki_&lt;ID&gt;, here Nuki_%s)", p + 5);
    }
    else
    {
        snprintf(hint, sizeof(hint), "Expected client ID (optional; the Nuki lock uses Nuki_&lt;Nuki ID in hex&gt;)");
    }
    printInputField(response, "FS_LM_CID", hint, esc(s.lockMqttClientId).c_str(), LEN_MQTT_CLIENT_ID, "");
    printCheckBox(response, "FS_LM_SKIP", "Webhook: skip lock/unlock if the lock already is locked/unlocked (only while connected; never unlatch)", s.skipRedundant, "");
    printInputField(response, "FS_LM_SILENCE", "Skip only if the lock was heard from within (ms, 10000-900000)", (int)s.lockSilenceMs, 6, "");
    response->print("</table>");
}
#endif

esp_err_t WebCfgServer::buildForkSettingsHtml(PsychicRequest* request, PsychicResponse* resp, const Settings* shown,
                                              const String& message, bool error)
{
    auto stored = allocSettings();
    if(!stored || !ForkSettings::snapshot(*stored))
    {
        return buildConfirmHtml(request, resp, "Settings busy or out of memory, try again.", 3, true, "/");
    }
    const Settings& s = shown != nullptr ? *shown : *stored;
    ForkSettings::State state;
    ForkSettings::state(state);
    const Caps caps = ForkSettings::caps();
    const DiState lock = lockInputState(stored->lockDi, _gpio);
    const bool editable = lock == DiState::Off || lock == DiState::Active;
    const bool authConfigured = strlen(_credUser) > 0 && strlen(_credPassword) > 0;
    const bool totpAvailable = _importExport->getTOTPEnabled();
    const bool mfaApproval = _preferences->getBool(preference_cred_duo_approval, false) && (totpAvailable || _duoEnabled);

    const char* script =
        "<script>function fsGen(n,b){var a=new Uint8Array(b);crypto.getRandomValues(a);var h='';"
        "for(var i=0;i<a.length;i++){h+=('0'+a[i].toString(16)).slice(-2);}"
        "var e=document.getElementsByName(n)[0];e.value=h;e.focus();e.select();}</script>";

    PsychicStreamResponse response(resp, "text/html");
    response.beginSend();
    buildHtmlHeader(&response, script);

    if(!authConfigured)
    {
        response.print("<table><tbody><tr><td colspan=\"2\" style=\"border: 0; color: red; font-size: 28px; font-weight: bold; text-align: center;\">"
                       "WEB UI HAS NO PASSWORD: anyone on the network can change who opens the door. "
                       "Set a user and password under Credentials (and TOTP or Duo).</td></tr></tbody></table>");
    }
    if(_preferences->getString(preference_bypass_proxy, "") != "")
    {
        printBanner(&response, "Requests from the reverse proxy IP set under Credentials skip the web login, including this page.", true);
    }
    if(message.length() > 0)
    {
        printBanner(&response, message, error);
    }
    if(!editable)
    {
        String text = "Settings locked. ";
        if(lock == DiState::HasRole)
        {
            text += "DI" + String(stored->lockDi) + " now has a GPIO role, so it can't unlock; ";
        }
        else
        {
            text += "Hold DI" + String(stored->lockDi) + " active and reload this page, or ";
        }
        text += "run <b>forkcfg unlock</b> on the USB serial console. \"Switch the webhook off\" below still works.";
        printBanner(&response, text, true);
    }

    response.print("<h3>Protect Webhook &amp; Relays</h3>");
    response.print("<table>");
    {
        String active = state.active ? "<span style=\"color: green\">Active</span>" : "<span style=\"color: red\">Off</span>";
        if(state.reason[0] != 0)
        {
            active += ": " + esc(state.reason);
        }
        printParameter(&response, "Webhook", active.c_str());
    }
    printParameter(&response, "Clock", timeSynced ? "Synced" : "<span class=\"warning\">Not synced: presses get 503 no_time</span>");
    {
        const int64_t age = ProtectWebhook::bleHeartbeatAgeMs();
        String ble = age < 0 ? String("not started (lock actions get 503 ble_stalled)")
                             : "last heartbeat " + String((long)(age / 1000)) + " s ago";
        printParameter(&response, "Nuki task (BLE)", ble.c_str());
        printParameter(&response, "Nuki lock", _nuki != nullptr ? "enabled" : "not enabled: only relay rules can work");
    }
    {
        String rules = String((unsigned)enabledRuleCount(*stored)) + " of " + String((unsigned)MAX_RULES) + " enabled";
        printParameter(&response, "Rules", rules.c_str());
    }
    const char* source = ForkSettings::source() == ForkSettings::Source::Nvs ? "saved settings (NVS)" :
                         ForkSettings::source() == ForkSettings::Source::Header ? "seeded from ProtectWebhookConfig.h at this boot, stored in NVS" :
                         "defaults, nothing saved yet";
    printParameter(&response, "Settings source", source);
    printParameter(&response, "ProtectWebhookConfig.h in this build",
                   ForkSettings::headerPresent() ? "yes (only seeds an empty NVS)" : "no");
    {
        String lockText = lock == DiState::Off ? String("off") :
                          "DI" + String(stored->lockDi) + (lock == DiState::Active ? ": active, editing allowed" :
                                                           lock == DiState::Inactive ? ": inactive, locked" : ": has a GPIO role, locked");
        printParameter(&response, "Settings lock", lockText.c_str());
    }
    {
        String url = "http://" + request->client()->localIP().toString() + "/protect?r=&lt;rule token&gt;";
        printParameter(&response, "Webhook URL", url.c_str());
    }
    response.print("</table><br>");

    ProtectWebhook::Result results[ProtectWebhook::RESULT_LOG_SIZE];
    const size_t resultCount = ProtectWebhook::recentResults(results, ProtectWebhook::RESULT_LOG_SIZE);
    response.print("<h3>Recent webhook requests</h3>");
    if(resultCount == 0)
    {
        response.print("<p>None since boot.</p>");
    }
    else
    {
        response.print("<table><tr><th>Time</th><th>Result</th><th>Rule</th></tr>");
        for(size_t i = 0; i < resultCount; i++)
        {
            const ProtectWebhook::Result& r = results[i];
            char when[32];
            response.print("<tr><td>");
            response.print(resultTime(r, when, sizeof(when)));
            response.print("</td><td>");
            response.print((unsigned)r.code);
            response.print(" ");
            response.print(r.result);
            response.print("</td><td>");
            if(r.rule >= 0)
            {
                response.print("Rule ");
                response.print((int)r.rule + 1);
                response.print(": ");
                response.print(esc(r.action));
            }
            response.print("</td></tr>");
        }
        response.print("</table>");
    }
    response.print("<br>");

    response.print("<form method=\"post\" action=\"/post?page=forkcfg\" autocomplete=\"off\">");
    response.print("<input type=\"hidden\" name=\"FORKCFG\" value=\"1\">");
    response.print("<input type=\"hidden\" name=\"FSCSRF\" value=\"");
    response.print(csrf());
    response.print("\">");
    response.print(editable ? "<fieldset style=\"border: 0; padding: 0\">" : "<fieldset disabled style=\"border: 0; padding: 0\">");

    response.print("<h3>Webhook</h3><table>");
    printCheckBox(&response, "FS_EN", "Webhook enabled (POST /protect)", s.enabled, "");
    printParameter(&response, "Secret (Protect: Bearer token)", writeOnlyStatus(stored->secret).c_str());
    printInputField(&response, "FS_SECRET", "New secret (empty = keep; min. 32 characters)", "", LEN_SECRET, "autocomplete=\"off\"");
    printGenerateRow(&response, "FS_SECRET", 32);
    printCheckBox(&response, "FS_SECRET_CLR", "Clear the secret", false, "");
    printInputField(&response, "FS_SRCIP", "Source IP (the UniFi console; empty = any)", esc(s.sourceIp).c_str(), LEN_IP, "");
    response.print("</table>");

    response.print("<h3>Timing (milliseconds)</h3><table>");
    printInputField(&response, "FS_SKEW", "Max. clock skew of an event (1000-120000)", (int)s.maxSkewMs, 6, "");
    printInputField(&response, "FS_COOL", "Cooldown per rule (1000-600000)", (int)s.cooldownMs, 6, "");
    printInputField(&response, "FS_DEADLINE", "Lock action deadline (1000-60000)", (int)s.actionDeadlineMs, 5, "");
    printInputField(&response, "FS_BLESTALL", "Reboot if the BLE task stalls for (5000-600000)", (int)s.bleStallMs, 6, "");
    response.print("</table>");

    if(caps.relays > 0)
    {
        response.print("<h3>Relays</h3><table>");
        std::vector<std::pair<String, String>> counts;
        for(unsigned i = 1; i <= caps.relays; i++)
        {
            counts.push_back(std::make_pair(String(i), String(i)));
        }
        printDropDown(&response, "FS_RELAYS", "Relays available to rules (relay1..relayN)", String((unsigned)s.relayCount), counts, "");
        printInputField(&response, "FS_PULSE", "Relay pulse (100-30000)", (int)s.relayPulseMs, 5, "");
        response.print("</table>");
    }

#ifdef NUKI_HUB_EMBEDDED_LOCK_MQTT
    buildLockMqttSection(&response, s, *stored);
#endif

    response.print("<h3>Hardening options</h3><table>");
    printCheckBox(&response, "FS_BEARER", "Bearer only: refuse the secret as ?k= in the URL (Protect: Auth = Bearer)", s.bearerOnly, "");
    printCheckBox(&response, "FS_REQIP", "Require source IP: the webhook can't be enabled while the source IP is empty", s.requireSourceIp, "");
    printCheckBox(&response, "FS_BROAD", "Allow broad rules: a rule without token and without key + value (matches ANY event of that fob)", s.allowBroadRules, "");
    if(totpAvailable || s.requireTotp)
    {
        printCheckBox(&response, "FS_REQTOTP", "Require a TOTP code for every save on this page", s.requireTotp, "");
    }
    else
    {
        printParameter(&response, "Require a TOTP code for every save on this page", "needs TOTP enabled under Credentials");
    }
    if(caps.digitalInputs > 0)
    {
        std::vector<std::pair<String, String>> dis;
        dis.push_back(std::make_pair("0", "Off"));
        for(unsigned i = 1; i <= caps.digitalInputs; i++)
        {
            dis.push_back(std::make_pair(String(i), "DI" + String(i)));
        }
        printDropDown(&response, "FS_LOCKDI", "Settings lock: saving needs this input active (DI without a GPIO role; hold it while turning the lock on)", String((unsigned)s.lockDi), dis, "");
    }
    response.print("</table>");
    response.print("<p>Also in Nuki Hub: Credentials &rarr; \"Require MFA (Duo/TOTP) for all sensitive operations\" asks for Duo/TOTP on every save, including this page. "
                   "USB console: <b>forkcfg unlock</b> (settings lock and TOTP requirement off), <b>forkcfg off</b> (webhook off).</p>");

    // Rules
    std::vector<std::pair<String, String>> actions;
    actions.push_back(std::make_pair("", "(none)"));
    for(const char* a : LOCK_ACTIONS)
    {
        actions.push_back(std::make_pair(String(a), String("Lock: ") + a));
    }
    for(unsigned i = 1; i <= caps.relays && i <= s.relayCount; i++)
    {
        actions.push_back(std::make_pair("relay" + String(i), "Relay " + String(i) + " pulse"));
    }
    response.print("<h3>Rules</h3><p>One Protect alarm per fob button + gesture, each with its own token. "
                   "Empty key/field/value = not checked. Disabled rules are drafts and not checked.</p>");
    bool shownEmpty = false;
    char name[16];
    for(size_t i = 0; i < MAX_RULES; i++)
    {
        const Rule& r = s.rules[i];
        const bool empty = ruleEmpty(r);
        const bool open = !empty ? r.enabled : !shownEmpty;
        shownEmpty = shownEmpty || empty;
        const unsigned n = (unsigned)i;

        response.print(open ? "<details open>" : "<details>");
        response.print("<summary>Rule ");
        response.print(n + 1);
        if(isSet(r.name))
        {
            response.print(": ");
            response.print(esc(r.name));
        }
        response.print(empty ? " (empty)" : r.enabled ? " (enabled" : " (disabled");
        if(!empty)
        {
            if(isSet(r.action))
            {
                response.print(", ");
                response.print(esc(r.action));
            }
            response.print(")");
        }
        response.print("</summary>");
        snprintf(name, sizeof(name), "R%u_SEEN", n);
        response.print("<input type=\"hidden\" name=\"");
        response.print(name);
        response.print("\" value=\"1\"><table>");

        snprintf(name, sizeof(name), "R%u_EN", n);
        printCheckBox(&response, name, "Enabled", r.enabled, "");
        snprintf(name, sizeof(name), "R%u_NAME", n);
        printInputField(&response, name, "Name (label only)", esc(r.name).c_str(), LEN_NAME, "");
        const Rule& storedRule = stored->rules[i];
        printParameter(&response, "Token (?r=)", writeOnlyStatus(storedRule.token).c_str());
        snprintf(name, sizeof(name), "R%u_TOK", n);
        printInputField(&response, name, "New token (empty = keep; min. 16 characters)", "", LEN_TOKEN, "autocomplete=\"off\"");
        printGenerateRow(&response, name, 16);
        snprintf(name, sizeof(name), "R%u_TOKCLR", n);
        printCheckBox(&response, name, "Clear the token", false, "");
        snprintf(name, sizeof(name), "R%u_KEY", n);
        printInputField(&response, name, "Key (e.g. sensor_button_pressed)", esc(r.key).c_str(), LEN_TEXT, "");
        snprintf(name, sizeof(name), "R%u_DEV", n);
        printInputField(&response, name, "Device: fob MAC (required)", esc(r.device).c_str(), LEN_DEVICE, "");
        snprintf(name, sizeof(name), "R%u_F1", n);
        printInputField(&response, name, "Field (e.g. button)", esc(r.field).c_str(), LEN_TEXT, "");
        snprintf(name, sizeof(name), "R%u_V1", n);
        printInputField(&response, name, "Value (e.g. right)", esc(r.value).c_str(), LEN_TEXT, "");
        snprintf(name, sizeof(name), "R%u_F2", n);
        printInputField(&response, name, "Field 2 (e.g. value)", esc(r.field2).c_str(), LEN_TEXT, "");
        snprintf(name, sizeof(name), "R%u_V2", n);
        printInputField(&response, name, "Value 2 (e.g. longPress, press, doublePress)", esc(r.value2).c_str(), LEN_TEXT, "");
        snprintf(name, sizeof(name), "R%u_ACT", n);
        std::vector<std::pair<String, String>> ruleActions = actions;
        if(isSet(r.action) && canonicalAction(r.action, s.relayCount) == nullptr)
        {
            ruleActions.push_back(std::make_pair(esc(r.action), esc(r.action) + " (not available)"));
        }
        printDropDown(&response, name, "Action", esc(r.action), ruleActions, "");
        response.print("</table></details>");
    }

    if(totpAvailable && (stored->requireTotp || mfaApproval))
    {
        response.print("<table>");
        printInputField(&response, "totpkey", "TOTP code", "", 6, "autocomplete=\"one-time-code\" inputmode=\"numeric\"");
        response.print("</table>");
    }
    if(editable)
    {
        response.print("<br><input type=\"submit\" name=\"submit\" value=\"Save\">");
    }
    response.print("</fieldset></form>");

    // Always available, also when locked: switching off only makes it safer.
    response.print("<br><form method=\"post\" action=\"/post?page=forkcfg\" onsubmit=\"return confirm('Switch the Protect webhook off?');\">");
    response.print("<input type=\"hidden\" name=\"FORKCFG\" value=\"1\"><input type=\"hidden\" name=\"FS_ACTION\" value=\"off\">");
    response.print("<input type=\"hidden\" name=\"FSCSRF\" value=\"");
    response.print(csrf());
    response.print("\">");
    if(totpAvailable && (stored->requireTotp || mfaApproval))
    {
        response.print("<table>");
        printInputField(&response, "totpkey", "TOTP code", "", 6, "autocomplete=\"one-time-code\" inputmode=\"numeric\"");
        response.print("</table>");
    }
    response.print("<input type=\"submit\" value=\"Switch the webhook off now\" style=\"background: red\"></form>");
    response.print("</body></html>");
    return response.endSend();
}

#endif
