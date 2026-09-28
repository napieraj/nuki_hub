#pragma once

// Fork settings (UniFi Protect webhook + relays), pure part: the settings
// struct, defaults, validation, NVS serialization and form-field helpers.
// No Arduino, NVS or FreeRTOS, so it is shared by ForkSettings.cpp,
// ForkSettingsWeb.cpp and the host tests (pio test -e native).

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <strings.h>

namespace ForkSettingsLogic
{
    constexpr size_t MAX_RULES = 12;
    constexpr uint8_t MAX_RELAYS = 8;
    constexpr uint8_t MAX_DIGITAL_INPUTS = 8;

    constexpr size_t LEN_SECRET_MIN = 32;
    constexpr size_t LEN_SECRET = 128;
    constexpr size_t LEN_TOKEN_MIN = 16;
    constexpr size_t LEN_TOKEN = 64;
    constexpr size_t LEN_NAME = 32;
    constexpr size_t LEN_TEXT = 40;   // key, field, value, field2, value2
    constexpr size_t LEN_DEVICE = 23; // "AA:BB:CC:DD:EE:FF" is 17
    constexpr size_t LEN_ACTION = 15;
    constexpr size_t LEN_IP = 15;
    // Embedded lock MQTT server: the Nuki app allows up to 32 characters.
    constexpr size_t LEN_MQTT_USER = 32;
    constexpr size_t LEN_MQTT_PASS = 32;
    constexpr size_t LEN_MQTT_CLIENT_ID = 32;

    struct Range
    {
        uint32_t min;
        uint32_t max;
        uint32_t def;
    };

    constexpr Range MAX_SKEW_MS = { 1000, 120000, 15000 };
    constexpr Range COOLDOWN_MS = { 1000, 600000, 10000 };
    constexpr Range ACTION_DEADLINE_MS = { 1000, 60000, 8000 };
    constexpr Range BLE_STALL_MS = { 5000, 600000, 30000 };
    // A relay closed for minutes (typo, wrong unit) would hold a door open.
    constexpr Range RELAY_PULSE_MS = { 100, 30000, 3000 };
    // Redundant-action skip: the lock must have been heard from (any MQTT
    // packet) within this. The Nuki lock pings every 300 s (keepalive k300).
    constexpr Range LOCK_SILENCE_MS = { 10000, 900000, 330000 };
    // ... but never within this many seconds of the previous accepted webhook
    // lock action (the reported state can lag or be mid-motion). 0 = off.
    constexpr Range SKIP_GRACE_S = { 0, 600, 60 };

    // Nuki Hub lock actions (NukiHelper::lockActionToEnum), canonical spelling.
    constexpr const char* LOCK_ACTIONS[] = {
        "unlock", "lock", "unlatch", "lockNgo", "lockNgoUnlatch", "fullLock",
        "fobAction1", "fobAction2", "fobAction3"
    };
    constexpr size_t LOCK_ACTION_COUNT = sizeof(LOCK_ACTIONS) / sizeof(LOCK_ACTIONS[0]);

    struct Rule
    {
        bool enabled;
        char name[LEN_NAME + 1];     // label only
        char token[LEN_TOKEN + 1];   // ?r= token, "" = not checked
        char key[LEN_TEXT + 1];      // triggers[].key, "" = not checked
        char device[LEN_DEVICE + 1]; // fob MAC, required
        char field[LEN_TEXT + 1];
        char value[LEN_TEXT + 1];    // "" = not checked
        char field2[LEN_TEXT + 1];
        char value2[LEN_TEXT + 1];   // "" = not checked
        char action[LEN_ACTION + 1];
    };

    struct Settings
    {
        bool enabled;                 // master switch
        char secret[LEN_SECRET + 1];
        char sourceIp[LEN_IP + 1];    // "" = any source
        uint32_t maxSkewMs;
        uint32_t cooldownMs;
        uint32_t actionDeadlineMs;
        uint32_t bleStallMs;
        uint32_t relayPulseMs;
        uint8_t relayCount;           // relayN actions allowed for N <= relayCount

        // Hardening options (all off by default)
        bool bearerOnly;              // refuse ?k=<secret> in the URL
        bool requireSourceIp;         // refuse to enable without a source IP
        bool allowBroadRules;         // allow rules with neither token nor key+value
        bool requireTotp;             // saving needs a TOTP code
        uint8_t lockDi;               // 0 = off; 1..8: saving needs DIn active

        Rule rules[MAX_RULES];

        // Embedded MQTT server for the Nuki lock (NUKI_HUB_EMBEDDED_LOCK_MQTT;
        // stored in every build so the blob is the same everywhere).
        bool lockMqttEnabled;                           // off by default
        char lockMqttUser[LEN_MQTT_USER + 1];
        char lockMqttPass[LEN_MQTT_PASS + 1];           // write-only on the page
        char lockMqttClientId[LEN_MQTT_CLIENT_ID + 1];  // "" = any client ID
        bool skipRedundant;                             // skip lock/unlock the lock already is in
        uint32_t lockSilenceMs;                         // skip only if the lock was heard from within this
        uint32_t skipGraceS;                            // never skip within this after a webhook lock action (0 = off)
    };

    // What the board offers; validation rejects relayN / DIn beyond it.
    struct Caps
    {
        uint8_t relays;
        uint8_t digitalInputs;
    };

    inline bool isSet(const char* s)
    {
        return s != nullptr && s[0] != 0;
    }

    inline void setDefaults(Settings& s)
    {
        memset(&s, 0, sizeof(s));
        s.maxSkewMs = MAX_SKEW_MS.def;
        s.cooldownMs = COOLDOWN_MS.def;
        s.actionDeadlineMs = ACTION_DEADLINE_MS.def;
        s.bleStallMs = BLE_STALL_MS.def;
        s.relayPulseMs = RELAY_PULSE_MS.def;
        s.relayCount = MAX_RELAYS;
        s.skipRedundant = true;
        s.lockSilenceMs = LOCK_SILENCE_MS.def;
        s.skipGraceS = SKIP_GRACE_S.def;
    }

    // Values left over from ProtectWebhookConfig.h.example ("replace-with-...").
    inline bool isPlaceholder(const char* s)
    {
        return s != nullptr && strncmp(s, "replace", 7) == 0;
    }

    // Copy with a length check (never truncates). nullptr copies as "".
    inline bool copyText(char* dst, size_t cap, const char* src)
    {
        if(src == nullptr)
        {
            dst[0] = 0;
            return true;
        }
        const size_t n = strlen(src);
        if(n >= cap)
        {
            return false;
        }
        memcpy(dst, src, n + 1);
        return true;
    }

    // Form input: strip leading/trailing whitespace, then copyText.
    inline bool setTrimmed(char* dst, size_t cap, const char* src)
    {
        if(src == nullptr)
        {
            dst[0] = 0;
            return true;
        }
        while(*src && isspace((unsigned char)*src)) src++;
        size_t n = strlen(src);
        while(n > 0 && isspace((unsigned char)src[n - 1])) n--;
        if(n >= cap)
        {
            return false;
        }
        memcpy(dst, src, n);
        dst[n] = 0;
        return true;
    }

    // Write-only field (secret, token): "clear" empties it, a non-empty
    // submitted value replaces it, an empty one keeps the stored value.
    inline bool applyWriteOnly(char* dst, size_t cap, const char* submitted, bool clear)
    {
        if(clear)
        {
            dst[0] = 0;
            return true;
        }
        char tmp[LEN_SECRET + 2];
        if(cap > sizeof(tmp) || !setTrimmed(tmp, cap, submitted))
        {
            return false;
        }
        if(tmp[0] != 0)
        {
            memcpy(dst, tmp, strlen(tmp) + 1);
        }
        return true;
    }

    // Decimal digits only, no sign, no overflow.
    inline bool parseUint(const char* s, uint32_t& out)
    {
        if(!isSet(s))
        {
            return false;
        }
        uint64_t v = 0;
        for(const char* p = s; *p; p++)
        {
            if(*p < '0' || *p > '9')
            {
                return false;
            }
            v = v * 10 + (uint64_t)(*p - '0');
            if(v > 0xffffffffULL)
            {
                return false;
            }
        }
        out = (uint32_t)v;
        return true;
    }

    // Dotted-quad IPv4, not 0.0.0.0.
    inline bool isValidIpv4(const char* s)
    {
        if(!isSet(s))
        {
            return false;
        }
        int parts = 0;
        bool nonZero = false;
        const char* p = s;
        while(true)
        {
            if(*p < '0' || *p > '9')
            {
                return false;
            }
            int v = 0;
            int digits = 0;
            while(*p >= '0' && *p <= '9')
            {
                if(digits > 0 && v == 0)
                {
                    return false; // leading zero
                }
                v = v * 10 + (*p - '0');
                digits++;
                p++;
                if(digits > 3 || v > 255)
                {
                    return false;
                }
            }
            nonZero = nonZero || v != 0;
            parts++;
            if(*p == 0)
            {
                break;
            }
            if(*p != '.' || parts == 4)
            {
                return false;
            }
            p++;
        }
        return parts == 4 && nonZero;
    }

    // 12 hex digits; ':', '-' and '.' allowed as separators.
    inline bool isMac(const char* s)
    {
        if(!isSet(s))
        {
            return false;
        }
        int hex = 0;
        for(const char* p = s; *p; p++)
        {
            if(isxdigit((unsigned char)*p))
            {
                hex++;
            }
            else if(*p != ':' && *p != '-' && *p != '.')
            {
                return false;
            }
        }
        return hex == 12;
    }

    // URL-safe without encoding (the token travels as ?r=...).
    inline bool isUrlSafe(const char* s)
    {
        for(const char* p = s; *p; p++)
        {
            const char c = *p;
            if(!isalnum((unsigned char)c) && c != '-' && c != '_' && c != '.' && c != '~')
            {
                return false;
            }
        }
        return true;
    }

    // Bearer token68 characters (RFC 7235): hex and base64 secrets both fit.
    inline bool isToken68(const char* s)
    {
        for(const char* p = s; *p; p++)
        {
            const char c = *p;
            if(!isalnum((unsigned char)c) && strchr("-._~+/=", c) == nullptr)
            {
                return false;
            }
        }
        return true;
    }

    // Protect trigger keys, field names and values are identifiers or numbers
    // (sensor_button_pressed, button, right, longPress, 2): A-Z a-z 0-9 _ . -
    inline bool isMatchText(const char* s)
    {
        for(const char* p = s; *p; p++)
        {
            const char c = *p;
            if(!(c >= 'A' && c <= 'Z') && !(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9') &&
               c != '_' && c != '.' && c != '-')
            {
                return false;
            }
        }
        return true;
    }

    // Printable ASCII (MQTT user name, password, client ID as typed in the Nuki app).
    inline bool isPrintableAscii(const char* s)
    {
        for(const char* p = s; *p; p++)
        {
            if(*p < 0x20 || *p > 0x7e)
            {
                return false;
            }
        }
        return true;
    }

    // Canonical action name for a lock action (case-insensitive) or
    // "relay1".."relayN" with N <= relayCount; nullptr if unknown.
    inline const char* canonicalAction(const char* action, uint8_t relayCount)
    {
        if(!isSet(action))
        {
            return nullptr;
        }
        for(const char* a : LOCK_ACTIONS)
        {
            if(strcasecmp(action, a) == 0)
            {
                return a;
            }
        }
        static const char* const relays[MAX_RELAYS] = {
            "relay1", "relay2", "relay3", "relay4", "relay5", "relay6", "relay7", "relay8"
        };
        for(uint8_t i = 0; i < relayCount && i < MAX_RELAYS; i++)
        {
            if(strcasecmp(action, relays[i]) == 0)
            {
                return relays[i];
            }
        }
        return nullptr;
    }

    inline bool inRange(uint32_t v, const Range& r)
    {
        return v >= r.min && v <= r.max;
    }

    // An empty slot: nothing typed in it at all.
    inline bool ruleEmpty(const Rule& r)
    {
        return !r.enabled && !isSet(r.name) && !isSet(r.token) && !isSet(r.key) && !isSet(r.device) &&
               !isSet(r.field) && !isSet(r.value) && !isSet(r.field2) && !isSet(r.value2) && !isSet(r.action);
    }

    // Did anything that decides what the rule matches or does change? Then
    // its cooldown restarts from scratch.
    inline bool ruleMatchChanged(const Rule& a, const Rule& b)
    {
        return a.enabled != b.enabled || strcmp(a.token, b.token) != 0 || strcmp(a.key, b.key) != 0 ||
               strcmp(a.device, b.device) != 0 || strcmp(a.field, b.field) != 0 || strcmp(a.value, b.value) != 0 ||
               strcmp(a.field2, b.field2) != 0 || strcmp(a.value2, b.value2) != 0 || strcmp(a.action, b.action) != 0;
    }

    inline size_t enabledRuleCount(const Settings& s)
    {
        size_t n = 0;
        for(const Rule& r : s.rules)
        {
            n += r.enabled ? 1 : 0;
        }
        return n;
    }

    // Validate one enabled rule. i is the 0-based slot (messages say "Rule i+1").
    inline bool validateRule(const Settings& s, size_t i, char* err, size_t errLen)
    {
        const Rule& r = s.rules[i];
        const int n = (int)i + 1;
        if(!isMac(r.device))
        {
            snprintf(err, errLen, "Rule %d: the device must be the fob's MAC (12 hex digits)", n);
            return false;
        }
        if(canonicalAction(r.action, s.relayCount) == nullptr)
        {
            snprintf(err, errLen, "Rule %d: unknown action (relay actions: relay1..relay%u)", n, (unsigned)s.relayCount);
            return false;
        }
        if(isSet(r.value) && !isSet(r.field))
        {
            snprintf(err, errLen, "Rule %d: 'value' needs 'field'", n);
            return false;
        }
        if(isSet(r.value2) && !isSet(r.field2))
        {
            snprintf(err, errLen, "Rule %d: 'value2' needs 'field2'", n);
            return false;
        }
        if(isSet(r.token))
        {
            if(strlen(r.token) < LEN_TOKEN_MIN)
            {
                snprintf(err, errLen, "Rule %d: the token must be at least %u characters", n, (unsigned)LEN_TOKEN_MIN);
                return false;
            }
            if(!isUrlSafe(r.token))
            {
                snprintf(err, errLen, "Rule %d: the token may only contain A-Z a-z 0-9 - _ . ~", n);
                return false;
            }
            if(isPlaceholder(r.token))
            {
                snprintf(err, errLen, "Rule %d: the token is still the example placeholder", n);
                return false;
            }
        }
        if(!s.allowBroadRules && !isSet(r.token) && !(isSet(r.key) && isSet(r.value)))
        {
            snprintf(err, errLen, "Rule %d: needs a token, or key + value; otherwise it matches ANY event "
                     "from that fob (see 'Allow broad rules')", n);
            return false;
        }
        return true;
    }

    // Save-time check of what a rule compares against the trigger (key,
    // field, value, field2, value2): a label typed there would save fine and
    // then never match. Only on save, so settings stored before this check
    // keep loading. i is the 0-based slot.
    inline bool validateRuleText(const Settings& s, size_t i, char* err, size_t errLen)
    {
        const Rule& r = s.rules[i];
        const int n = (int)i + 1;
        if(!isMatchText(r.key))
        {
            snprintf(err, errLen, "Rule %d: the key may only contain A-Z a-z 0-9 _ . - "
                     "(Protect sends e.g. sensor_button_pressed; labels go in Name)", n);
            return false;
        }
        const struct
        {
            const char* label;
            const char* text;
        } texts[] = {
            { "field", r.field }, { "value", r.value }, { "field 2", r.field2 }, { "value 2", r.value2 },
        };
        for(const auto& t : texts)
        {
            if(!isMatchText(t.text))
            {
                snprintf(err, errLen, "Rule %d: %s may only contain A-Z a-z 0-9 _ . - "
                         "(e.g. button, right, value, longPress)", n, t.label);
                return false;
            }
        }
        return true;
    }

    inline bool validateLockMqtt(const Settings& s, char* err, size_t errLen)
    {
        if(!inRange(s.lockSilenceMs, LOCK_SILENCE_MS))
        {
            snprintf(err, errLen, "Lock MQTT: 'heard from within' must be %u-%u ms", (unsigned)LOCK_SILENCE_MS.min,
                     (unsigned)LOCK_SILENCE_MS.max);
            return false;
        }
        if(!inRange(s.skipGraceS, SKIP_GRACE_S))
        {
            snprintf(err, errLen, "Lock MQTT: 'skip grace' must be %u-%u s", (unsigned)SKIP_GRACE_S.min,
                     (unsigned)SKIP_GRACE_S.max);
            return false;
        }
        if(!isPrintableAscii(s.lockMqttUser) || !isPrintableAscii(s.lockMqttPass) || !isPrintableAscii(s.lockMqttClientId))
        {
            snprintf(err, errLen, "Lock MQTT: user name, password and client ID may only contain printable ASCII");
            return false;
        }
        if(strchr(s.lockMqttClientId, ' ') != nullptr)
        {
            snprintf(err, errLen, "Lock MQTT: the client ID can't contain spaces");
            return false;
        }
        if(s.lockMqttEnabled && (!isSet(s.lockMqttUser) || !isSet(s.lockMqttPass)))
        {
            snprintf(err, errLen, "Lock MQTT: set a user name and a password (the same as in the Nuki app) to enable it");
            return false;
        }
        return true;
    }

    // The same checks as the old build-time checks of ProtectWebhookConfig.h,
    // plus ranges. Disabled rules are drafts and not checked.
    inline bool validate(const Settings& s, const Caps& caps, char* err, size_t errLen)
    {
        if(!inRange(s.maxSkewMs, MAX_SKEW_MS) || !inRange(s.cooldownMs, COOLDOWN_MS) ||
           !inRange(s.actionDeadlineMs, ACTION_DEADLINE_MS) || !inRange(s.bleStallMs, BLE_STALL_MS))
        {
            snprintf(err, errLen, "A time value is out of range (skew %u-%u, cooldown %u-%u, deadline %u-%u, "
                     "BLE stall %u-%u ms)",
                     (unsigned)MAX_SKEW_MS.min, (unsigned)MAX_SKEW_MS.max, (unsigned)COOLDOWN_MS.min,
                     (unsigned)COOLDOWN_MS.max, (unsigned)ACTION_DEADLINE_MS.min, (unsigned)ACTION_DEADLINE_MS.max,
                     (unsigned)BLE_STALL_MS.min, (unsigned)BLE_STALL_MS.max);
            return false;
        }
        if(!inRange(s.relayPulseMs, RELAY_PULSE_MS))
        {
            snprintf(err, errLen, "The relay pulse must be %u-%u ms", (unsigned)RELAY_PULSE_MS.min,
                     (unsigned)RELAY_PULSE_MS.max);
            return false;
        }
        if(s.relayCount > caps.relays || (caps.relays > 0 && s.relayCount < 1))
        {
            snprintf(err, errLen, "The relay count must be 1-%u", (unsigned)caps.relays);
            return false;
        }
        if(s.lockDi > caps.digitalInputs)
        {
            snprintf(err, errLen, "The settings lock input must be DI1-DI%u", (unsigned)caps.digitalInputs);
            return false;
        }
        if(isSet(s.secret))
        {
            if(strlen(s.secret) < LEN_SECRET_MIN)
            {
                snprintf(err, errLen, "The secret must be at least %u characters", (unsigned)LEN_SECRET_MIN);
                return false;
            }
            if(!isToken68(s.secret))
            {
                snprintf(err, errLen, "The secret may only contain A-Z a-z 0-9 - . _ ~ + / =");
                return false;
            }
            if(isPlaceholder(s.secret))
            {
                snprintf(err, errLen, "The secret is still the example placeholder");
                return false;
            }
        }
        if(isSet(s.sourceIp) && !isValidIpv4(s.sourceIp))
        {
            snprintf(err, errLen, "The source IP must be an IPv4 address like 192.0.2.10");
            return false;
        }
        if(s.enabled && !isSet(s.secret))
        {
            snprintf(err, errLen, "The webhook can't be enabled without a secret");
            return false;
        }
        if(s.enabled && s.requireSourceIp && !isSet(s.sourceIp))
        {
            snprintf(err, errLen, "'Require source IP' is on: set the source IP or switch the webhook off");
            return false;
        }
        for(size_t i = 0; i < MAX_RULES; i++)
        {
            if(s.rules[i].enabled && !validateRule(s, i, err, errLen))
            {
                return false;
            }
        }
        return validateLockMqtt(s, err, errLen);
    }

    // validate() plus the checks that only apply to a new save (enabled
    // rules only; disabled rules are drafts).
    inline bool validateForSave(const Settings& s, const Caps& caps, char* err, size_t errLen)
    {
        if(!validate(s, caps, err, errLen))
        {
            return false;
        }
        for(size_t i = 0; i < MAX_RULES; i++)
        {
            if(s.rules[i].enabled && !validateRuleText(s, i, err, errLen))
            {
                return false;
            }
        }
        return true;
    }

    // --- NVS format: one blob, so a save is all-or-nothing -----------------
    // "FS", version, then the fields in order; strings are u8 length + bytes.

    // Version 2 appends the lock MQTT section; version 1 blobs still load
    // (lock MQTT off, defaults). Version 3 appends the skip grace (u32 s);
    // version 2 blobs load with the default grace.
    constexpr uint8_t BLOB_VERSION = 3;

    constexpr size_t MAX_BLOB_SIZE =
        3 + 1 + (1 + LEN_SECRET) + (1 + LEN_IP) + 5 * 4 + 1 + 1 + 1 + 1 +
        MAX_RULES * (1 + (1 + LEN_NAME) + (1 + LEN_TOKEN) + 5 * (1 + LEN_TEXT) + (1 + LEN_DEVICE) + (1 + LEN_ACTION)) +
        1 + (1 + LEN_MQTT_USER) + (1 + LEN_MQTT_PASS) + (1 + LEN_MQTT_CLIENT_ID) + 4 + 4;

    class Writer
    {
    public:
        Writer(uint8_t* buf, size_t cap) : _buf(buf), _cap(cap) {}
        void u8(uint8_t v)
        {
            if(_len < _cap) _buf[_len] = v;
            _len++;
        }
        void u32(uint32_t v)
        {
            for(int i = 0; i < 4; i++) u8((uint8_t)(v >> (8 * i)));
        }
        void str(const char* s)
        {
            const size_t n = strlen(s);
            u8((uint8_t)n);
            for(size_t i = 0; i < n; i++) u8((uint8_t)s[i]);
        }
        size_t length() const { return _len <= _cap ? _len : 0; }

    private:
        uint8_t* _buf;
        size_t _cap;
        size_t _len = 0;
    };

    class Reader
    {
    public:
        Reader(const uint8_t* buf, size_t len) : _buf(buf), _len(len) {}
        bool u8(uint8_t& v)
        {
            if(_pos >= _len) return false;
            v = _buf[_pos++];
            return true;
        }
        bool flag(bool& v)
        {
            uint8_t b;
            if(!u8(b) || b > 1) return false;
            v = b == 1;
            return true;
        }
        bool u32(uint32_t& v)
        {
            v = 0;
            for(int i = 0; i < 4; i++)
            {
                uint8_t b;
                if(!u8(b)) return false;
                v |= (uint32_t)b << (8 * i);
            }
            return true;
        }
        bool str(char* dst, size_t cap)
        {
            uint8_t n;
            if(!u8(n) || (size_t)n >= cap || _len - _pos < n) return false;
            for(size_t i = 0; i < n; i++)
            {
                if(_buf[_pos + i] == 0) return false;
            }
            memcpy(dst, _buf + _pos, n);
            dst[n] = 0;
            _pos += n;
            return true;
        }
        bool atEnd() const { return _pos == _len; }

    private:
        const uint8_t* _buf;
        size_t _len;
        size_t _pos = 0;
    };

    // Returns the blob length, 0 if buf is too small.
    inline size_t serialize(const Settings& s, uint8_t* buf, size_t cap)
    {
        Writer w(buf, cap);
        w.u8('F');
        w.u8('S');
        w.u8(BLOB_VERSION);
        w.u8(s.enabled ? 1 : 0);
        w.str(s.secret);
        w.str(s.sourceIp);
        w.u32(s.maxSkewMs);
        w.u32(s.cooldownMs);
        w.u32(s.actionDeadlineMs);
        w.u32(s.bleStallMs);
        w.u32(s.relayPulseMs);
        w.u8(s.relayCount);
        w.u8((uint8_t)((s.bearerOnly ? 1 : 0) | (s.requireSourceIp ? 2 : 0) | (s.allowBroadRules ? 4 : 0) |
                       (s.requireTotp ? 8 : 0)));
        w.u8(s.lockDi);
        w.u8((uint8_t)MAX_RULES);
        for(const Rule& r : s.rules)
        {
            w.u8(r.enabled ? 1 : 0);
            w.str(r.name);
            w.str(r.token);
            w.str(r.key);
            w.str(r.device);
            w.str(r.field);
            w.str(r.value);
            w.str(r.field2);
            w.str(r.value2);
            w.str(r.action);
        }
        w.u8((uint8_t)((s.lockMqttEnabled ? 1 : 0) | (s.skipRedundant ? 2 : 0)));
        w.str(s.lockMqttUser);
        w.str(s.lockMqttPass);
        w.str(s.lockMqttClientId);
        w.u32(s.lockSilenceMs);
        w.u32(s.skipGraceS);
        return w.length();
    }

    // All-or-nothing: out is only written if the whole blob is well-formed.
    inline bool deserialize(const uint8_t* buf, size_t len, Settings& out)
    {
        Settings s;
        setDefaults(s);
        Reader r(buf, len);
        uint8_t m1, m2, ver, flags, slots;
        if(!r.u8(m1) || !r.u8(m2) || !r.u8(ver) || m1 != 'F' || m2 != 'S' || ver < 1 || ver > BLOB_VERSION)
        {
            return false;
        }
        if(!r.flag(s.enabled) || !r.str(s.secret, sizeof(s.secret)) || !r.str(s.sourceIp, sizeof(s.sourceIp)) ||
           !r.u32(s.maxSkewMs) || !r.u32(s.cooldownMs) || !r.u32(s.actionDeadlineMs) || !r.u32(s.bleStallMs) ||
           !r.u32(s.relayPulseMs) || !r.u8(s.relayCount) || !r.u8(flags) || !r.u8(s.lockDi) || !r.u8(slots))
        {
            return false;
        }
        if(flags > 15 || slots > MAX_RULES)
        {
            return false;
        }
        s.bearerOnly = flags & 1;
        s.requireSourceIp = flags & 2;
        s.allowBroadRules = flags & 4;
        s.requireTotp = flags & 8;
        for(size_t i = 0; i < slots; i++)
        {
            Rule& x = s.rules[i];
            if(!r.flag(x.enabled) || !r.str(x.name, sizeof(x.name)) || !r.str(x.token, sizeof(x.token)) ||
               !r.str(x.key, sizeof(x.key)) || !r.str(x.device, sizeof(x.device)) ||
               !r.str(x.field, sizeof(x.field)) || !r.str(x.value, sizeof(x.value)) ||
               !r.str(x.field2, sizeof(x.field2)) || !r.str(x.value2, sizeof(x.value2)) ||
               !r.str(x.action, sizeof(x.action)))
            {
                return false;
            }
        }
        if(ver >= 2)
        {
            uint8_t lockFlags;
            if(!r.u8(lockFlags) || lockFlags > 3 || !r.str(s.lockMqttUser, sizeof(s.lockMqttUser)) ||
               !r.str(s.lockMqttPass, sizeof(s.lockMqttPass)) || !r.str(s.lockMqttClientId, sizeof(s.lockMqttClientId)) ||
               !r.u32(s.lockSilenceMs))
            {
                return false;
            }
            s.lockMqttEnabled = lockFlags & 1;
            s.skipRedundant = lockFlags & 2;
        }
        if(ver >= 3 && !r.u32(s.skipGraceS))
        {
            return false;
        }
        if(!r.atEnd())
        {
            return false;
        }
        out = s;
        return true;
    }

    // --- HTML -------------------------------------------------------------

    // Escape for text and attribute values. Returns false (out = "") if it
    // doesn't fit.
    inline bool htmlEscape(const char* in, char* out, size_t cap)
    {
        size_t o = 0;
        for(const char* p = in; *p; p++)
        {
            const char* rep = nullptr;
            switch(*p)
            {
            case '&': rep = "&amp;"; break;
            case '<': rep = "&lt;"; break;
            case '>': rep = "&gt;"; break;
            case '"': rep = "&quot;"; break;
            case '\'': rep = "&#39;"; break;
            default: break;
            }
            const size_t n = rep ? strlen(rep) : 1;
            if(o + n >= cap)
            {
                if(cap > 0) out[0] = 0;
                return false;
            }
            if(rep)
            {
                memcpy(out + o, rep, n);
            }
            else
            {
                out[o] = *p;
            }
            o += n;
        }
        if(cap > 0) out[o] = 0;
        return true;
    }
}
