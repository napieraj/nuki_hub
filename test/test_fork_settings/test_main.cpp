// Host tests for src/ForkSettingsLogic.h (web UI settings of the Protect
// webhook): pio test -e native

#include <unity.h>
#include <cstring>
#include "ForkSettingsLogic.h"
#include "ProtectWebhookLogic.h"

using namespace ForkSettingsLogic;

namespace
{
    const Caps WAVESHARE = { 8, 8 };
    const Caps NO_RELAYS = { 0, 0 };
    char err[200];

    void fillRule(Rule& r, const char* token, const char* action)
    {
        memset(&r, 0, sizeof(r));
        r.enabled = true;
        strcpy(r.name, "Fob A - Hold Right");
        strcpy(r.token, token);
        strcpy(r.key, "sensor_button_pressed");
        strcpy(r.device, "AA:BB:CC:DD:EE:01");
        strcpy(r.field, "button");
        strcpy(r.value, "right");
        strcpy(r.field2, "value");
        strcpy(r.value2, "longPress");
        strcpy(r.action, action);
    }

    // A complete, valid configuration with two rules.
    Settings valid()
    {
        Settings s;
        setDefaults(s);
        s.enabled = true;
        strcpy(s.secret, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
        strcpy(s.sourceIp, "192.0.2.10");
        fillRule(s.rules[0], "11111111111111111111111111111111", "unlock");
        fillRule(s.rules[1], "22222222222222222222222222222222", "relay2");
        memset(s.rules[1].value2, 0, sizeof(s.rules[1].value2));
        strcpy(s.rules[1].value2, "press");
        return s;
    }

    bool ok(const Settings& s, const Caps& caps = WAVESHARE)
    {
        err[0] = 0;
        return validate(s, caps, err, sizeof(err));
    }
}

void setUp() {}
void tearDown() {}

void test_defaults_are_disabled_and_valid()
{
    Settings s;
    setDefaults(s);
    TEST_ASSERT_FALSE(s.enabled);
    TEST_ASSERT_EQUAL_UINT32(15000, s.maxSkewMs);
    TEST_ASSERT_EQUAL_UINT32(10000, s.cooldownMs);
    TEST_ASSERT_EQUAL_UINT32(8000, s.actionDeadlineMs);
    TEST_ASSERT_EQUAL_UINT32(30000, s.bleStallMs);
    TEST_ASSERT_EQUAL_UINT32(3000, s.relayPulseMs);
    TEST_ASSERT_EQUAL_UINT8(8, s.relayCount);
    TEST_ASSERT_FALSE(s.bearerOnly || s.requireSourceIp || s.allowBroadRules || s.requireTotp);
    TEST_ASSERT_EQUAL_UINT8(0, s.lockDi);
    TEST_ASSERT_TRUE(ok(s));
    TEST_ASSERT_EQUAL(0, (int)enabledRuleCount(s));
}

void test_valid_config()
{
    TEST_ASSERT_TRUE_MESSAGE(ok(valid()), err);
}

void test_secret_rules()
{
    Settings s = valid();
    s.secret[31] = 0; // 31 chars
    TEST_ASSERT_FALSE(ok(s));
    s = valid();
    s.secret[32] = 0; // exactly 32
    TEST_ASSERT_TRUE(ok(s));
    strcpy(s.secret, "replace-with-output-of-openssl-rand-hex-32-000000000000");
    TEST_ASSERT_FALSE(ok(s));
    TEST_ASSERT_NOT_NULL(strstr(err, "placeholder"));
    strcpy(s.secret, "0123456789abcdef0123456789abcdef 123");
    TEST_ASSERT_FALSE(ok(s)); // space
    strcpy(s.secret, "AbCdEfGhIjKlMnOpQrStUvWxYz012345+/==");
    TEST_ASSERT_TRUE(ok(s));  // base64 is fine for a Bearer token
    s.secret[0] = 0;
    TEST_ASSERT_FALSE(ok(s)); // enabled needs a secret
    s.enabled = false;
    TEST_ASSERT_TRUE(ok(s));  // a disabled webhook may be saved without
}

void test_source_ip()
{
    Settings s = valid();
    const char* bad[] = { "192.0.2", "192.0.2.256", "192.0.2.10.1", "0.0.0.0", "192.0.02.1", "a.b.c.d", "1..2.3", " 1.2.3.4" };
    for(const char* ip : bad)
    {
        strcpy(s.sourceIp, ip);
        TEST_ASSERT_FALSE_MESSAGE(ok(s), ip);
    }
    strcpy(s.sourceIp, "10.0.0.1");
    TEST_ASSERT_TRUE(ok(s));
    s.sourceIp[0] = 0;
    TEST_ASSERT_TRUE(ok(s)); // no source check
    s.requireSourceIp = true;
    TEST_ASSERT_FALSE(ok(s));
    s.enabled = false;
    TEST_ASSERT_TRUE(ok(s)); // only enabling needs it
}

void test_rule_checks()
{
    Settings s = valid();
    s.rules[0].device[0] = 0;
    TEST_ASSERT_FALSE(ok(s));
    TEST_ASSERT_NOT_NULL(strstr(err, "Rule 1"));

    s = valid();
    strcpy(s.rules[0].device, "AA:BB:CC:DD:EE");
    TEST_ASSERT_FALSE(ok(s));
    strcpy(s.rules[0].device, "aabbccddee01");
    TEST_ASSERT_TRUE(ok(s));
    strcpy(s.rules[0].device, "AA-BB-CC-DD-EE-0G");
    TEST_ASSERT_FALSE(ok(s));

    s = valid();
    s.rules[0].field[0] = 0;
    TEST_ASSERT_FALSE(ok(s));
    TEST_ASSERT_NOT_NULL(strstr(err, "'value' needs 'field'"));

    s = valid();
    s.rules[1].field2[0] = 0;
    TEST_ASSERT_FALSE(ok(s));
    TEST_ASSERT_NOT_NULL(strstr(err, "Rule 2"));

    s = valid();
    strcpy(s.rules[0].token, "short");
    TEST_ASSERT_FALSE(ok(s));
    strcpy(s.rules[0].token, "0123456789abcdef"); // 16
    TEST_ASSERT_TRUE(ok(s));
    strcpy(s.rules[0].token, "0123456789abcdef&x=1");
    TEST_ASSERT_FALSE(ok(s));
    strcpy(s.rules[0].token, "replace-with-32-hex-token-hold-right");
    TEST_ASSERT_FALSE(ok(s));

    s = valid();
    strcpy(s.rules[0].action, "open");
    TEST_ASSERT_FALSE(ok(s));
    strcpy(s.rules[0].action, "lockngo"); // case-insensitive
    TEST_ASSERT_TRUE(ok(s));
}

void test_broad_rules()
{
    Settings s = valid();
    s.rules[0].token[0] = 0; // key + value still set
    TEST_ASSERT_TRUE(ok(s));
    s.rules[0].value[0] = 0; // neither token nor key+value
    TEST_ASSERT_FALSE(ok(s));
    s.allowBroadRules = true;
    TEST_ASSERT_TRUE(ok(s));
    s.allowBroadRules = false;
    s.rules[0].enabled = false; // drafts are not checked
    TEST_ASSERT_TRUE(ok(s));
}

void test_relay_count()
{
    Settings s = valid(); // rule 2 uses relay2
    s.relayCount = 2;
    TEST_ASSERT_TRUE(ok(s));
    s.relayCount = 1;
    TEST_ASSERT_FALSE(ok(s));
    TEST_ASSERT_NOT_NULL(strstr(err, "relay1..relay1"));
    s.relayCount = 0;
    TEST_ASSERT_FALSE(ok(s)); // 1..8 on this board
    s.relayCount = 9;
    TEST_ASSERT_FALSE(ok(s));

    TEST_ASSERT_EQUAL_STRING("relay8", canonicalAction("relay8", 8));
    TEST_ASSERT_NULL(canonicalAction("relay8", 7));
    TEST_ASSERT_NULL(canonicalAction("relay9", 8));
    TEST_ASSERT_NULL(canonicalAction("relay0", 8));
    TEST_ASSERT_EQUAL_STRING("unlock", canonicalAction("Unlock", 0));
    TEST_ASSERT_EQUAL_STRING("fobAction3", canonicalAction("FOBACTION3", 0));
    TEST_ASSERT_NULL(canonicalAction("", 8));

    // A board without relays: relay rules are refused, lock rules are fine.
    s = valid();
    s.relayCount = 0;
    s.rules[1].enabled = false;
    TEST_ASSERT_TRUE(ok(s, NO_RELAYS));
    s.rules[1].enabled = true;
    TEST_ASSERT_FALSE(ok(s, NO_RELAYS));
}

void test_ranges()
{
    Settings s = valid();
    s.relayPulseMs = 99;
    TEST_ASSERT_FALSE(ok(s));
    s.relayPulseMs = 100;
    TEST_ASSERT_TRUE(ok(s));
    s.relayPulseMs = 30000;
    TEST_ASSERT_TRUE(ok(s));
    s.relayPulseMs = 30001;
    TEST_ASSERT_FALSE(ok(s));

    s = valid();
    s.maxSkewMs = 999;
    TEST_ASSERT_FALSE(ok(s));
    s = valid();
    s.cooldownMs = 600001;
    TEST_ASSERT_FALSE(ok(s));
    s = valid();
    s.actionDeadlineMs = 60001;
    TEST_ASSERT_FALSE(ok(s));
    s = valid();
    s.bleStallMs = 4999;
    TEST_ASSERT_FALSE(ok(s));

    s = valid();
    s.lockDi = 8;
    TEST_ASSERT_TRUE(ok(s));
    s.lockDi = 9;
    TEST_ASSERT_FALSE(ok(s));
    s.lockDi = 1;
    s.relayCount = 0;
    s.rules[1].enabled = false;
    TEST_ASSERT_FALSE(ok(s, NO_RELAYS)); // no inputs to lock with
}

void test_serialize_round_trip()
{
    Settings s = valid();
    s.bearerOnly = true;
    s.requireTotp = true;
    s.lockDi = 3;
    s.relayCount = 4;
    s.cooldownMs = 12345;
    fillRule(s.rules[11], "33333333333333333333333333333333", "lockNgo");
    s.rules[11].enabled = false;

    static uint8_t buf[MAX_BLOB_SIZE];
    const size_t n = serialize(s, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, (int)n);
    TEST_ASSERT_LESS_OR_EQUAL(MAX_BLOB_SIZE, n);

    Settings t;
    setDefaults(t);
    TEST_ASSERT_TRUE(deserialize(buf, n, t));
    TEST_ASSERT_EQUAL_MEMORY(&s.rules, &t.rules, sizeof(s.rules));
    TEST_ASSERT_EQUAL_STRING(s.secret, t.secret);
    TEST_ASSERT_EQUAL_STRING(s.sourceIp, t.sourceIp);
    TEST_ASSERT_TRUE(t.enabled && t.bearerOnly && t.requireTotp && !t.requireSourceIp && !t.allowBroadRules);
    TEST_ASSERT_EQUAL_UINT8(3, t.lockDi);
    TEST_ASSERT_EQUAL_UINT8(4, t.relayCount);
    TEST_ASSERT_EQUAL_UINT32(12345, t.cooldownMs);
    TEST_ASSERT_EQUAL_UINT32(s.relayPulseMs, t.relayPulseMs);

    // A typical config is small (NVS space is shared with Nuki Hub).
    TEST_ASSERT_LESS_THAN(900, (int)n);
}

void test_lock_mqtt_settings()
{
    Settings s;
    setDefaults(s);
    TEST_ASSERT_FALSE(s.lockMqttEnabled);
    TEST_ASSERT_TRUE(s.skipRedundant);
    TEST_ASSERT_EQUAL_UINT32(LOCK_SILENCE_MS.def, s.lockSilenceMs);
    TEST_ASSERT_TRUE(validate(s, WAVESHARE, err, sizeof(err)));

    // Enabling needs user name and password.
    s.lockMqttEnabled = true;
    TEST_ASSERT_FALSE(validate(s, WAVESHARE, err, sizeof(err)));
    strcpy(s.lockMqttUser, "nukilock");
    TEST_ASSERT_FALSE(validate(s, WAVESHARE, err, sizeof(err)));
    strcpy(s.lockMqttPass, "pass word 1");
    TEST_ASSERT_TRUE(validate(s, WAVESHARE, err, sizeof(err)));
    strcpy(s.lockMqttClientId, "Nuki_2BB28570");
    TEST_ASSERT_TRUE(validate(s, WAVESHARE, err, sizeof(err)));
    strcpy(s.lockMqttClientId, "Nuki 2BB28570");
    TEST_ASSERT_FALSE(validate(s, WAVESHARE, err, sizeof(err)));
    strcpy(s.lockMqttClientId, "");
    strcpy(s.lockMqttPass, "tab\there");
    TEST_ASSERT_FALSE(validate(s, WAVESHARE, err, sizeof(err)));
    strcpy(s.lockMqttPass, "caf\xc3\xa9");
    TEST_ASSERT_FALSE(validate(s, WAVESHARE, err, sizeof(err)));
    strcpy(s.lockMqttPass, "ok");

    s.lockSilenceMs = LOCK_SILENCE_MS.min - 1;
    TEST_ASSERT_FALSE(validate(s, WAVESHARE, err, sizeof(err)));
    s.lockSilenceMs = LOCK_SILENCE_MS.max + 1;
    TEST_ASSERT_FALSE(validate(s, WAVESHARE, err, sizeof(err)));
    s.lockSilenceMs = LOCK_SILENCE_MS.max;
    TEST_ASSERT_TRUE(validate(s, WAVESHARE, err, sizeof(err)));

    // Disabled: the fields may stay empty, and a stored user name is kept.
    s.lockMqttEnabled = false;
    s.lockMqttPass[0] = 0;
    TEST_ASSERT_TRUE(validate(s, WAVESHARE, err, sizeof(err)));

    // Round trip.
    s.lockMqttEnabled = true;
    strcpy(s.lockMqttPass, "s3cret");
    strcpy(s.lockMqttClientId, "Nuki_2BB28570");
    s.skipRedundant = false;
    s.lockSilenceMs = 60000;
    static uint8_t buf[MAX_BLOB_SIZE];
    const size_t n = serialize(s, buf, sizeof(buf));
    Settings t;
    setDefaults(t);
    TEST_ASSERT_TRUE(deserialize(buf, n, t));
    TEST_ASSERT_TRUE(t.lockMqttEnabled);
    TEST_ASSERT_FALSE(t.skipRedundant);
    TEST_ASSERT_EQUAL_STRING("nukilock", t.lockMqttUser);
    TEST_ASSERT_EQUAL_STRING("s3cret", t.lockMqttPass);
    TEST_ASSERT_EQUAL_STRING("Nuki_2BB28570", t.lockMqttClientId);
    TEST_ASSERT_EQUAL_UINT32(60000, t.lockSilenceMs);

    // Bad lock flags byte.
    const size_t flagsAt = n - 4 - (1 + strlen("Nuki_2BB28570")) - (1 + strlen("s3cret")) - (1 + strlen("nukilock")) - 1;
    TEST_ASSERT_EQUAL_UINT8(1, buf[flagsAt]);
    buf[flagsAt] = 4;
    TEST_ASSERT_FALSE(deserialize(buf, n, t));
}

void test_version1_blob_still_loads()
{
    // What the previous firmware stored: version 1, no lock MQTT section.
    Settings s = valid();
    static uint8_t buf[MAX_BLOB_SIZE];
    size_t n = serialize(s, buf, sizeof(buf));
    const size_t tail = 1 + 1 + 1 + 1 + 4; // flags, three empty strings, u32
    n -= tail;
    buf[2] = 1;
    Settings t;
    setDefaults(t);
    t.lockMqttEnabled = true; // must be overwritten by the defaults
    TEST_ASSERT_TRUE(deserialize(buf, n, t));
    TEST_ASSERT_EQUAL_MEMORY(&s.rules, &t.rules, sizeof(s.rules));
    TEST_ASSERT_EQUAL_STRING(s.secret, t.secret);
    TEST_ASSERT_FALSE(t.lockMqttEnabled);
    TEST_ASSERT_TRUE(t.skipRedundant);
    TEST_ASSERT_EQUAL_UINT32(LOCK_SILENCE_MS.def, t.lockSilenceMs);
    // Version 1 with a version 2 tail, and unknown versions, are refused.
    TEST_ASSERT_FALSE(deserialize(buf, n + tail, t));
    buf[2] = 3;
    TEST_ASSERT_FALSE(deserialize(buf, n, t));
    buf[2] = 0;
    TEST_ASSERT_FALSE(deserialize(buf, n, t));
}

void test_serialize_worst_case_fits()
{
    Settings s;
    setDefaults(s);
    memset(s.secret, 'a', LEN_SECRET);
    for(Rule& r : s.rules)
    {
        r.enabled = true;
        memset(r.name, 'n', LEN_NAME);
        memset(r.token, 't', LEN_TOKEN);
        memset(r.key, 'k', LEN_TEXT);
        memset(r.device, 'd', LEN_DEVICE);
        memset(r.field, 'f', LEN_TEXT);
        memset(r.value, 'v', LEN_TEXT);
        memset(r.field2, 'g', LEN_TEXT);
        memset(r.value2, 'w', LEN_TEXT);
        memset(r.action, 'a', LEN_ACTION);
    }
    memset(s.sourceIp, '1', LEN_IP);
    s.lockMqttEnabled = true;
    memset(s.lockMqttUser, 'u', LEN_MQTT_USER);
    memset(s.lockMqttPass, 'p', LEN_MQTT_PASS);
    memset(s.lockMqttClientId, 'c', LEN_MQTT_CLIENT_ID);
    static uint8_t buf[MAX_BLOB_SIZE];
    TEST_ASSERT_EQUAL(MAX_BLOB_SIZE, serialize(s, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL(0, (int)serialize(s, buf, sizeof(buf) - 1));
    Settings t;
    TEST_ASSERT_TRUE(deserialize(buf, MAX_BLOB_SIZE, t));
    TEST_ASSERT_EQUAL_MEMORY(&s, &t, sizeof(s));
}

void test_deserialize_rejects_damage()
{
    Settings s = valid();
    static uint8_t buf[MAX_BLOB_SIZE];
    const size_t n = serialize(s, buf, sizeof(buf));
    Settings t;
    setDefaults(t);
    strcpy(t.secret, "untouched");

    // Every truncation fails and leaves the output alone.
    for(size_t len = 0; len < n; len++)
    {
        TEST_ASSERT_FALSE(deserialize(buf, len, t));
    }
    TEST_ASSERT_EQUAL_STRING("untouched", t.secret);

    static uint8_t copy[MAX_BLOB_SIZE + 1];
    memcpy(copy, buf, n);
    copy[n] = 0;
    TEST_ASSERT_FALSE(deserialize(copy, n + 1, t)); // trailing byte

    memcpy(copy, buf, n);
    copy[2] = BLOB_VERSION + 1;
    TEST_ASSERT_FALSE(deserialize(copy, n, t));     // unknown version

    memcpy(copy, buf, n);
    copy[0] = 'X';
    TEST_ASSERT_FALSE(deserialize(copy, n, t));

    memcpy(copy, buf, n);
    copy[3] = 2;                                    // enabled flag not 0/1
    TEST_ASSERT_FALSE(deserialize(copy, n, t));

    memcpy(copy, buf, n);
    copy[4] = (uint8_t)(LEN_SECRET + 1);            // secret length too long
    TEST_ASSERT_FALSE(deserialize(copy, n, t));
    TEST_ASSERT_EQUAL_STRING("untouched", t.secret);
}

void test_write_only_fields()
{
    char secret[LEN_SECRET + 1] = "old-secret";
    TEST_ASSERT_TRUE(applyWriteOnly(secret, sizeof(secret), "", false));
    TEST_ASSERT_EQUAL_STRING("old-secret", secret);   // empty = keep
    TEST_ASSERT_TRUE(applyWriteOnly(secret, sizeof(secret), "   ", false));
    TEST_ASSERT_EQUAL_STRING("old-secret", secret);   // blank = keep
    TEST_ASSERT_TRUE(applyWriteOnly(secret, sizeof(secret), nullptr, false));
    TEST_ASSERT_EQUAL_STRING("old-secret", secret);
    TEST_ASSERT_TRUE(applyWriteOnly(secret, sizeof(secret), " new \n", false));
    TEST_ASSERT_EQUAL_STRING("new", secret);          // trimmed
    TEST_ASSERT_TRUE(applyWriteOnly(secret, sizeof(secret), "ignored", true));
    TEST_ASSERT_EQUAL_STRING("", secret);             // clear wins

    char token[LEN_TOKEN + 1] = "keep";
    char tooLong[LEN_TOKEN + 2];
    memset(tooLong, 'x', LEN_TOKEN + 1);
    tooLong[LEN_TOKEN + 1] = 0;
    TEST_ASSERT_FALSE(applyWriteOnly(token, sizeof(token), tooLong, false));
    TEST_ASSERT_EQUAL_STRING("keep", token);
    tooLong[LEN_TOKEN] = 0;
    TEST_ASSERT_TRUE(applyWriteOnly(token, sizeof(token), tooLong, false));
    TEST_ASSERT_EQUAL(LEN_TOKEN, (int)strlen(token));
}

void test_text_and_numbers()
{
    char buf[8];
    TEST_ASSERT_TRUE(setTrimmed(buf, sizeof(buf), "  right "));
    TEST_ASSERT_EQUAL_STRING("right", buf);
    TEST_ASSERT_FALSE(setTrimmed(buf, sizeof(buf), "12345678"));
    TEST_ASSERT_TRUE(setTrimmed(buf, sizeof(buf), "1234567"));
    TEST_ASSERT_TRUE(copyText(buf, sizeof(buf), nullptr));
    TEST_ASSERT_EQUAL_STRING("", buf);

    uint32_t v = 7;
    TEST_ASSERT_TRUE(parseUint("30000", v));
    TEST_ASSERT_EQUAL_UINT32(30000, v);
    TEST_ASSERT_TRUE(parseUint("4294967295", v));
    TEST_ASSERT_FALSE(parseUint("4294967296", v));
    TEST_ASSERT_FALSE(parseUint("-1", v));
    TEST_ASSERT_FALSE(parseUint("3 s", v));
    TEST_ASSERT_FALSE(parseUint("", v));
    TEST_ASSERT_FALSE(parseUint(nullptr, v));
}

void test_html_escape()
{
    char out[64];
    TEST_ASSERT_TRUE(htmlEscape("<b onclick=\"x('&')\">", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("&lt;b onclick=&quot;x(&#39;&amp;&#39;)&quot;&gt;", out);
    TEST_ASSERT_TRUE(htmlEscape("Fob A - Hold Right", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("Fob A - Hold Right", out);
    char small[5];
    TEST_ASSERT_FALSE(htmlEscape("a<b", small, sizeof(small)));
    TEST_ASSERT_EQUAL_STRING("", small);
    TEST_ASSERT_TRUE(htmlEscape("abcd", small, sizeof(small)));
}

void test_rule_changes()
{
    Settings a = valid();
    Settings b = a;
    TEST_ASSERT_FALSE(ruleMatchChanged(a.rules[0], b.rules[0]));
    strcpy(b.rules[0].name, "renamed"); // a label only
    TEST_ASSERT_FALSE(ruleMatchChanged(a.rules[0], b.rules[0]));
    strcpy(b.rules[0].action, "lock");
    TEST_ASSERT_TRUE(ruleMatchChanged(a.rules[0], b.rules[0]));
    b = a;
    b.rules[0].enabled = false;
    TEST_ASSERT_TRUE(ruleMatchChanged(a.rules[0], b.rules[0]));
    b = a;
    b.rules[0].token[0] = 'x';
    TEST_ASSERT_TRUE(ruleMatchChanged(a.rules[0], b.rules[0]));

    Rule empty;
    memset(&empty, 0, sizeof(empty));
    TEST_ASSERT_TRUE(ruleEmpty(empty));
    TEST_ASSERT_FALSE(ruleEmpty(a.rules[0]));
}

// The webhook matches against the settings table directly.
void test_matching_uses_enabled_slots()
{
    Settings s = valid();
    auto fields = [](const char* field, const char* want)
    {
        if(strcmp(field, "button") == 0) return strcmp("right", want) == 0;
        if(strcmp(field, "value") == 0) return strcmp("press", want) == 0;
        return false;
    };
    using ProtectWebhookLogic::findRule;
    TEST_ASSERT_EQUAL(1, findRule(s.rules, MAX_RULES, "22222222222222222222222222222222",
                                  "sensor_button_pressed", "AABBCCDDEE01", fields));
    s.rules[1].enabled = false;
    TEST_ASSERT_EQUAL(-1, findRule(s.rules, MAX_RULES, "22222222222222222222222222222222",
                                   "sensor_button_pressed", "AABBCCDDEE01", fields));
    // An empty token or key means "not checked"; an empty device never matches.
    s = valid();
    s.rules[1].token[0] = 0;
    TEST_ASSERT_EQUAL(1, findRule(s.rules, MAX_RULES, "", "sensor_button_pressed", "AABBCCDDEE01", fields));
    s.rules[1].device[0] = 0;
    TEST_ASSERT_EQUAL(-1, findRule(s.rules, MAX_RULES, "", "sensor_button_pressed", "", fields));
}

void test_replay_guard_reset_rule()
{
    ProtectWebhookLogic::ReplayGuard<MAX_RULES> g;
    TEST_ASSERT_TRUE(g.accept("e1", 3, 1000, 10000));
    TEST_ASSERT_FALSE(g.accept("e2", 3, 2000, 10000)); // cooldown
    g.resetRule(3);
    TEST_ASSERT_TRUE(g.accept("e3", 3, 2000, 10000));  // cooldown forgotten
    TEST_ASSERT_FALSE(g.accept("e1", 4, 50000, 10000)); // replay cache kept
    g.resetRule(MAX_RULES);                             // out of range: no-op
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_defaults_are_disabled_and_valid);
    RUN_TEST(test_valid_config);
    RUN_TEST(test_secret_rules);
    RUN_TEST(test_source_ip);
    RUN_TEST(test_rule_checks);
    RUN_TEST(test_broad_rules);
    RUN_TEST(test_relay_count);
    RUN_TEST(test_ranges);
    RUN_TEST(test_serialize_round_trip);
    RUN_TEST(test_serialize_worst_case_fits);
    RUN_TEST(test_lock_mqtt_settings);
    RUN_TEST(test_version1_blob_still_loads);
    RUN_TEST(test_deserialize_rejects_damage);
    RUN_TEST(test_write_only_fields);
    RUN_TEST(test_text_and_numbers);
    RUN_TEST(test_html_escape);
    RUN_TEST(test_rule_changes);
    RUN_TEST(test_matching_uses_enabled_slots);
    RUN_TEST(test_replay_guard_reset_rule);
    return UNITY_END();
}
