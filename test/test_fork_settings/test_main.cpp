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
    // Bytes version 4 appends: master switch, relay count, 8 x (role, flags, pulse).
    const size_t V4_TAIL = 1 + 1 + MAX_RELAYS * (1 + 1 + 4);

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

void test_rule_text_checked_on_save()
{
    auto saveOk = [](const Settings& s)
    {
        err[0] = 0;
        return validateForSave(s, WAVESHARE, err, sizeof(err));
    };
    Settings s = valid();
    TEST_ASSERT_TRUE_MESSAGE(saveOk(s), err);
    strcpy(s.rules[0].value, "-1");
    strcpy(s.rules[0].value2, "2.5");
    TEST_ASSERT_TRUE_MESSAGE(saveOk(s), err); // numbers are fine

    // The reported case: a label in the key field.
    s = valid();
    strcpy(s.rules[1].key, "Fob A \xc2\xb7 Hold Right \xe2\x86\x92 Unlatch");
    TEST_ASSERT_TRUE(ok(s)); // stored settings keep loading...
    TEST_ASSERT_FALSE(saveOk(s)); // ...but a new save is refused
    TEST_ASSERT_NOT_NULL(strstr(err, "Rule 2: the key"));
    strcpy(s.rules[1].key, "sensor button pressed");
    TEST_ASSERT_FALSE(saveOk(s));
    strcpy(s.rules[1].key, "&#8594;");
    TEST_ASSERT_FALSE(saveOk(s));
    s.rules[1].enabled = false; // drafts are not checked
    TEST_ASSERT_TRUE_MESSAGE(saveOk(s), err);

    const char* bad[] = { "Right ", "long press", "right\xe2\x86\x92", "a/b", "x\"y" };
    for(const char* b : bad)
    {
        s = valid();
        strcpy(s.rules[0].field, b);
        TEST_ASSERT_FALSE_MESSAGE(saveOk(s), b);
        TEST_ASSERT_NOT_NULL(strstr(err, "Rule 1: field "));
        s = valid();
        strcpy(s.rules[0].value, b);
        TEST_ASSERT_FALSE_MESSAGE(saveOk(s), b);
        TEST_ASSERT_NOT_NULL(strstr(err, "Rule 1: value "));
        s = valid();
        strcpy(s.rules[0].field2, b);
        TEST_ASSERT_FALSE_MESSAGE(saveOk(s), b);
        TEST_ASSERT_NOT_NULL(strstr(err, "Rule 1: field 2"));
        s = valid();
        strcpy(s.rules[0].value2, b);
        TEST_ASSERT_FALSE_MESSAGE(saveOk(s), b);
        TEST_ASSERT_NOT_NULL(strstr(err, "Rule 1: value 2"));
    }

    // Other problems still come first, as in validate().
    s = valid();
    strcpy(s.rules[0].key, "bad key");
    s.rules[0].device[0] = 0;
    TEST_ASSERT_FALSE(saveOk(s));
    TEST_ASSERT_NOT_NULL(strstr(err, "device"));
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
    TEST_ASSERT_EQUAL_UINT32(60, s.skipGraceS);
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

    // Skip grace: 0 (off) .. 600 s.
    s.skipGraceS = 0;
    TEST_ASSERT_TRUE(validate(s, WAVESHARE, err, sizeof(err)));
    s.skipGraceS = 600;
    TEST_ASSERT_TRUE(validate(s, WAVESHARE, err, sizeof(err)));
    s.skipGraceS = 601;
    TEST_ASSERT_FALSE(validate(s, WAVESHARE, err, sizeof(err)));
    s.skipGraceS = 60;

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
    s.skipGraceS = 0;
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
    TEST_ASSERT_EQUAL_UINT32(0, t.skipGraceS);

    // Bad lock flags byte.
    const size_t flagsAt = n - V4_TAIL - 4 - 4 - (1 + strlen("Nuki_2BB28570")) - (1 + strlen("s3cret")) - (1 + strlen("nukilock")) - 1;
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
    const size_t tail = 1 + 1 + 1 + 1 + 4 + 4; // flags, three empty strings, silence, grace
    n -= tail + V4_TAIL;
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
    TEST_ASSERT_EQUAL_UINT32(SKIP_GRACE_S.def, t.skipGraceS);
    // Version 1 with a later tail, and unknown versions, are refused.
    TEST_ASSERT_FALSE(deserialize(buf, n + tail, t));
    buf[2] = BLOB_VERSION + 1;
    TEST_ASSERT_FALSE(deserialize(buf, n, t));
    buf[2] = 0;
    TEST_ASSERT_FALSE(deserialize(buf, n, t));
}

void test_version2_blob_still_loads()
{
    // What the previous firmware stored: version 2, lock MQTT section without
    // the skip grace. Built by hand so it stays the old layout.
    Settings s = valid();
    s.lockMqttEnabled = true;
    s.skipRedundant = true;
    strcpy(s.lockMqttUser, "nukilock");
    strcpy(s.lockMqttPass, "s3cret");
    s.lockSilenceMs = 120000;
    s.skipGraceS = 5; // not in a version 2 blob
    static uint8_t buf[MAX_BLOB_SIZE];
    size_t n = serialize(s, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT8(BLOB_VERSION, buf[2]);
    n -= 4 + V4_TAIL; // drop the grace and the relay outputs
    buf[2] = 2;
    Settings t;
    setDefaults(t);
    t.skipGraceS = 7; // must be overwritten by the default
    TEST_ASSERT_TRUE(deserialize(buf, n, t));
    TEST_ASSERT_EQUAL_MEMORY(&s.rules, &t.rules, sizeof(s.rules));
    TEST_ASSERT_TRUE(t.lockMqttEnabled);
    TEST_ASSERT_TRUE(t.skipRedundant);
    TEST_ASSERT_EQUAL_STRING("nukilock", t.lockMqttUser);
    TEST_ASSERT_EQUAL_STRING("s3cret", t.lockMqttPass);
    TEST_ASSERT_EQUAL_UINT32(120000, t.lockSilenceMs);
    TEST_ASSERT_EQUAL_UINT32(SKIP_GRACE_S.def, t.skipGraceS);
    TEST_ASSERT_TRUE(validate(t, WAVESHARE, err, sizeof(err)));
    // Version 2 with the version 3 tail is refused, version 3 without it too.
    TEST_ASSERT_FALSE(deserialize(buf, n + 4, t));
    buf[2] = 3;
    TEST_ASSERT_FALSE(deserialize(buf, n, t));
    TEST_ASSERT_TRUE(deserialize(buf, n + 4, t));
    TEST_ASSERT_EQUAL_UINT32(5, t.skipGraceS);
}

void test_version3_blob_still_loads()
{
    // What the current v3 firmware stores: no relay outputs section. It must
    // load with relay outputs off, no stored roles, and every relay pulsing
    // for the old global relay pulse, so relayN rules behave exactly as before.
    Settings s = valid(); // rule 2: relay2
    s.relayPulseMs = 1500;
    s.relayCount = 2;
    s.lockMqttEnabled = true;
    strcpy(s.lockMqttUser, "nukilock");
    strcpy(s.lockMqttPass, "s3cret");
    s.skipGraceS = 42;
    static uint8_t buf[MAX_BLOB_SIZE];
    size_t n = serialize(s, buf, sizeof(buf));
    n -= V4_TAIL;
    buf[2] = 3;
    Settings t;
    setDefaults(t);
    t.relayOutputs = true;        // must be overwritten
    t.relays[0].stored = true;
    TEST_ASSERT_TRUE(deserialize(buf, n, t));
    TEST_ASSERT_FALSE(t.relayOutputs);
    for(size_t i = 0; i < MAX_RELAYS; i++)
    {
        TEST_ASSERT_FALSE(t.relays[i].stored);
        TEST_ASSERT_EQUAL_UINT32(1500, t.relays[i].pulseMs);
        TEST_ASSERT_TRUE(effectiveRole(t, i) == RelayRole::WebhookPulse);
        TEST_ASSERT_FALSE(effectiveInvert(t, i));
    }
    TEST_ASSERT_EQUAL_UINT32(42, t.skipGraceS);
    TEST_ASSERT_EQUAL_STRING("s3cret", t.lockMqttPass);
    TEST_ASSERT_EQUAL_MEMORY(&s.rules, &t.rules, sizeof(s.rules));
    TEST_ASSERT_TRUE(ok(t));
    TEST_ASSERT_TRUE(relayAvailableToRules(t, 2, 8));
    TEST_ASSERT_FALSE(relayAvailableToRules(t, 3, 8)); // relay count 2, as before
    // v3 with the v4 tail, and v4 without it, are refused.
    TEST_ASSERT_FALSE(deserialize(buf, n + V4_TAIL, t));
    buf[2] = 4;
    TEST_ASSERT_FALSE(deserialize(buf, n, t));
    TEST_ASSERT_TRUE(deserialize(buf, n + V4_TAIL, t));
}

void test_relay_settings_round_trip()
{
    Settings s = valid();
    s.relayOutputs = true;
    s.relays[1].stored = true;          // relay 2 (rule 2's relay) back to a webhook relay
    s.relays[1].role = (uint8_t)RelayRole::WebhookPulse;
    s.relays[1].pulseMs = 800;
    s.relays[7].stored = true;
    s.relays[7].role = (uint8_t)RelayRole::NightMode;
    s.relays[7].invert = true;
    static uint8_t buf[MAX_BLOB_SIZE];
    const size_t n = serialize(s, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);
    Settings t;
    TEST_ASSERT_TRUE(deserialize(buf, n, t));
    TEST_ASSERT_EQUAL_MEMORY(&s, &t, sizeof(s));
    TEST_ASSERT_TRUE(t.relayOutputs);
    TEST_ASSERT_TRUE(configuredRole(t, 7) == RelayRole::NightMode);
    TEST_ASSERT_TRUE(configuredInvert(t, 7));
    TEST_ASSERT_TRUE(configuredRole(t, 2) == RelayRole::DoorOpen); // default for relay 3
    TEST_ASSERT_TRUE(configuredInvert(t, 4));                      // relay 5 LockFault, inverted
    TEST_ASSERT_TRUE(ok(t));

    // Damaged relay section: flags > 3, too many relays.
    static uint8_t copy[MAX_BLOB_SIZE];
    memcpy(copy, buf, n);
    copy[n - V4_TAIL + 1 + 1 + 1] = 4; // relay 1 flags
    TEST_ASSERT_FALSE(deserialize(copy, n, t));
    memcpy(copy, buf, n);
    copy[n - V4_TAIL + 1] = MAX_RELAYS + 1;
    TEST_ASSERT_FALSE(deserialize(copy, n, t));
    memcpy(copy, buf, n);
    copy[n - V4_TAIL] = 2; // master switch not 0/1
    TEST_ASSERT_FALSE(deserialize(copy, n, t));
}

void test_unknown_stored_role_reads_off()
{
    Settings s;
    setDefaults(s);
    s.relayOutputs = true;
    s.relays[3].stored = true;
    s.relays[3].role = 200;
    TEST_ASSERT_TRUE(configuredRole(s, 3) == RelayRole::Off);
    TEST_ASSERT_TRUE(RelayOutputsLogic::roleFromStored(RelayOutputsLogic::ROLE_COUNT) == RelayRole::Off);
    TEST_ASSERT_TRUE(RelayOutputsLogic::roleFromStored(11) == RelayRole::NightMode);
}

void test_relay_rules_follow_roles()
{
    // The user's rule: "Fob - Double Right -> Relay 1".
    Settings s = valid();
    strcpy(s.rules[1].action, "relay1");
    TEST_ASSERT_TRUE(ok(s));
    s.relayOutputs = true; // relay 1 defaults to WebhookPulse: still fine
    TEST_ASSERT_TRUE(ok(s));
    TEST_ASSERT_TRUE(validateForSave(s, WAVESHARE, err, sizeof(err)));

    // A rule on a state relay (relay 2 = Secure by default) fails closed.
    strcpy(s.rules[1].action, "relay2");
    TEST_ASSERT_FALSE(ok(s));
    TEST_ASSERT_NOT_NULL(strstr(err, "relay2 has the role"));
    // ... also when the rule is fine and the role change would orphan it.
    strcpy(s.rules[1].action, "relay1");
    applyRelayForm(s, 0, (uint8_t)RelayRole::Locked, false, 3000);
    TEST_ASSERT_FALSE(ok(s));
    TEST_ASSERT_NOT_NULL(strstr(err, "relay1 has the role 'Locked'"));
    // A disabled (draft) rule doesn't block it.
    s.rules[1].enabled = false;
    TEST_ASSERT_TRUE(ok(s));

    // Master on ignores the relay count; master off uses it (as before).
    s = valid();
    strcpy(s.rules[1].action, "relay8");
    applyRelayForm(s, 7, (uint8_t)RelayRole::WebhookPulse, false, 3000);
    s.relayCount = 1;
    TEST_ASSERT_FALSE(ok(s));
    s.relayOutputs = true;
    TEST_ASSERT_TRUE(ok(s));
    TEST_ASSERT_FALSE(relayAvailableToRules(s, 9, 8));
    TEST_ASSERT_FALSE(relayAvailableToRules(s, 0, 8));
    TEST_ASSERT_EQUAL(8, relayOfAction("Relay8"));
    TEST_ASSERT_EQUAL(0, relayOfAction("unlock"));
    TEST_ASSERT_EQUAL(0, relayOfAction("relay9"));

    // No relays on the board: no outputs.
    s = valid();
    s.relayCount = 0;
    s.rules[1].enabled = false;
    s.relayOutputs = true;
    TEST_ASSERT_FALSE(ok(s, NO_RELAYS));
}

void test_relay_validation()
{
    Settings s = valid();
    s.relays[4].pulseMs = 99;
    TEST_ASSERT_FALSE(ok(s));
    TEST_ASSERT_NOT_NULL(strstr(err, "Relay 5: the pulse"));
    s.relays[4].pulseMs = 30001;
    TEST_ASSERT_FALSE(ok(s));
    s.relays[4].pulseMs = 30000;
    TEST_ASSERT_TRUE(ok(s));

    // Invert on a webhook relay would hold the intercom button down.
    s.relayOutputs = true;
    strcpy(s.rules[1].action, "relay1");
    applyRelayForm(s, 0, (uint8_t)RelayRole::WebhookPulse, true, 3000);
    TEST_ASSERT_FALSE(ok(s));
    TEST_ASSERT_NOT_NULL(strstr(err, "Invert"));
    // With relay outputs off every relay is a plain webhook relay: accepted.
    s.relayOutputs = false;
    TEST_ASSERT_TRUE(ok(s));
}

void test_relay_form_keeps_defaults_unstored()
{
    Settings s;
    setDefaults(s);
    // Submitted exactly as the defaults: nothing stored, only the pulse.
    for(size_t i = 0; i < MAX_RELAYS; i++)
    {
        const auto& d = RelayOutputsLogic::DEFAULTS[i];
        applyRelayForm(s, i, (uint8_t)d.role, d.invert, 2500);
        TEST_ASSERT_FALSE(s.relays[i].stored);
        TEST_ASSERT_EQUAL_UINT32(2500, s.relays[i].pulseMs);
    }
    // A change is stored, and stays stored even when set back to the default.
    applyRelayForm(s, 2, (uint8_t)RelayRole::Off, false, 3000);
    TEST_ASSERT_TRUE(s.relays[2].stored);
    TEST_ASSERT_TRUE(configuredRole(s, 2) == RelayRole::Off);
    applyRelayForm(s, 2, (uint8_t)RelayRole::DoorOpen, false, 3000);
    TEST_ASSERT_TRUE(s.relays[2].stored);
    TEST_ASSERT_TRUE(configuredRole(s, 2) == RelayRole::DoorOpen);
    // Invert alone counts as a choice.
    applyRelayForm(s, 3, (uint8_t)RelayRole::Locked, true, 3000);
    TEST_ASSERT_TRUE(s.relays[3].stored);
    // Reset forgets everything.
    resetRelaysToDefaults(s);
    for(size_t i = 0; i < MAX_RELAYS; i++)
    {
        TEST_ASSERT_FALSE(s.relays[i].stored);
        TEST_ASSERT_EQUAL_UINT32(RELAY_PULSE_MS.def, s.relays[i].pulseMs);
        TEST_ASSERT_TRUE(configuredRole(s, i) == RelayOutputsLogic::DEFAULTS[i].role);
    }
    // Master off: every relay is a webhook relay whatever is configured.
    applyRelayForm(s, 5, (uint8_t)RelayRole::NightMode, true, 3000);
    TEST_ASSERT_TRUE(effectiveRole(s, 5) == RelayRole::WebhookPulse);
    TEST_ASSERT_FALSE(effectiveInvert(s, 5));
    s.relayOutputs = true;
    TEST_ASSERT_TRUE(effectiveRole(s, 5) == RelayRole::NightMode);
    TEST_ASSERT_TRUE(effectiveInvert(s, 5));
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

void test_utf8_name()
{
    // "Fob A \u00b7 Hold Right \u2192 Unlatch": 28 characters, 31 bytes.
    const char* label = "Fob A \xc2\xb7 Hold Right \xe2\x86\x92 Unlatch";
    Rule r;
    TEST_ASSERT_TRUE(setTrimmed(r.name, sizeof(r.name), label));
    TEST_ASSERT_EQUAL_STRING(label, r.name);
    // Limits count bytes, and too long is refused whole: never half a character.
    char buf[4];
    TEST_ASSERT_TRUE(setTrimmed(buf, sizeof(buf), "a\xc2\xb7"));     // 3 bytes
    TEST_ASSERT_FALSE(setTrimmed(buf, sizeof(buf), "ab\xc2\xb7"));   // 4 bytes
    TEST_ASSERT_FALSE(setTrimmed(buf, sizeof(buf), "\xe2\x86\x92x")); // 4 bytes
    TEST_ASSERT_TRUE(setTrimmed(buf, sizeof(buf), " \xe2\x86\x92 "));
    TEST_ASSERT_EQUAL_STRING("\xe2\x86\x92", buf);
    // Rendered back: UTF-8 bytes pass through, markup is escaped, and an
    // old stored "&#8594;" shows as that text.
    char out[128];
    TEST_ASSERT_TRUE(htmlEscape("<\xe2\x86\x92>", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("&lt;\xe2\x86\x92&gt;", out);
    TEST_ASSERT_TRUE(htmlEscape("&#8594;", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("&amp;#8594;", out);
    // A UTF-8 name is fine in a rule (only key/field/value are identifiers).
    Settings s = valid();
    strcpy(s.rules[0].name, label);
    err[0] = 0;
    TEST_ASSERT_TRUE_MESSAGE(validateForSave(s, WAVESHARE, err, sizeof(err)), err);
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
    RUN_TEST(test_rule_text_checked_on_save);
    RUN_TEST(test_relay_count);
    RUN_TEST(test_ranges);
    RUN_TEST(test_serialize_round_trip);
    RUN_TEST(test_serialize_worst_case_fits);
    RUN_TEST(test_lock_mqtt_settings);
    RUN_TEST(test_version1_blob_still_loads);
    RUN_TEST(test_version2_blob_still_loads);
    RUN_TEST(test_version3_blob_still_loads);
    RUN_TEST(test_relay_settings_round_trip);
    RUN_TEST(test_unknown_stored_role_reads_off);
    RUN_TEST(test_relay_rules_follow_roles);
    RUN_TEST(test_relay_validation);
    RUN_TEST(test_relay_form_keeps_defaults_unstored);
    RUN_TEST(test_deserialize_rejects_damage);
    RUN_TEST(test_write_only_fields);
    RUN_TEST(test_text_and_numbers);
    RUN_TEST(test_html_escape);
    RUN_TEST(test_utf8_name);
    RUN_TEST(test_rule_changes);
    RUN_TEST(test_matching_uses_enabled_slots);
    RUN_TEST(test_replay_guard_reset_rule);
    return UNITY_END();
}
