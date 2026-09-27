// Host tests for src/ProtectWebhookLogic.h: pio test -e native
// The cases mirror scripts/protect_webhook_test.py suite.

#include <unity.h>
#include <cstring>
#include "ProtectWebhookLogic.h"

using namespace ProtectWebhookLogic;

namespace
{
    // Same shape as ProtectRule in ProtectWebhookConfig.h.example.
    struct Rule
    {
        const char* token;
        const char* key;
        const char* device;
        const char* field;
        const char* value;
        const char* field2;
        const char* value2;
        const char* action;
    };

    const char* const TOKEN_HOLD = "11111111111111111111111111111111";
    const char* const TOKEN_PRESS = "22222222222222222222222222222222";

    const Rule RULES[] = {
        { TOKEN_HOLD, "sensor_button_pressed", "AA:BB:CC:DD:EE:01", "button", "right", "value", "longPress", "unlock" },
        { TOKEN_PRESS, "sensor_button_pressed", "AA:BB:CC:DD:EE:01", "button", "right", "value", "press", "lock" },
    };

    // A trigger's fields as Protect sends them (button + gesture).
    struct Trigger
    {
        const char* key;
        const char* device;
        const char* button;
        const char* value;
    };

    int match(const char* token, const Trigger& t)
    {
        return findRule(RULES, token, t.key, t.device, [&t](const char* field, const char* want)
        {
            if(strcmp(field, "button") == 0) return textEquals(t.button, want);
            if(strcmp(field, "value") == 0) return textEquals(t.value, want);
            return false;
        });
    }

    const Trigger HOLD = { "sensor_button_pressed", "AABBCCDDEE01", "right", "longPress" };
    const Trigger PRESS = { "sensor_button_pressed", "AABBCCDDEE01", "right", "press" };

    const int64_t NOW = 1790000000000LL; // 2026-09 in ms
    const int64_t SKEW = 15000;
    const int64_t COOLDOWN = 10000;
}

void setUp() {}
void tearDown() {}

// ---- MAC and token comparison

void test_mac_ignores_case_and_separators()
{
    TEST_ASSERT_TRUE(macMatches("AABBCCDDEE01", "AA:BB:CC:DD:EE:01"));
    TEST_ASSERT_TRUE(macMatches("aa-bb-cc-dd-ee-01", "AA:BB:CC:DD:EE:01"));
    TEST_ASSERT_FALSE(macMatches("AABBCCDDEE02", "AA:BB:CC:DD:EE:01"));
    TEST_ASSERT_FALSE(macMatches("AABBCCDDEE", "AA:BB:CC:DD:EE:01"));
    TEST_ASSERT_FALSE(macMatches("", "AA:BB:CC:DD:EE:01"));
}

void test_ct_equals()
{
    TEST_ASSERT_TRUE(ctEquals(TOKEN_HOLD, TOKEN_HOLD));
    TEST_ASSERT_FALSE(ctEquals(TOKEN_HOLD, TOKEN_PRESS));
    TEST_ASSERT_FALSE(ctEquals("", TOKEN_HOLD));
    TEST_ASSERT_FALSE(ctEquals("1111", TOKEN_HOLD));
}

void test_field_text_and_number()
{
    TEST_ASSERT_TRUE(textEquals("LongPress", "longPress"));
    TEST_ASSERT_FALSE(textEquals(nullptr, "longPress"));
    TEST_ASSERT_TRUE(numberEquals(2, "2"));
    TEST_ASSERT_FALSE(numberEquals(12, "2"));
}

// ---- Timestamps (bench: stale, future, legacy)

void test_timestamp_normalisation()
{
    TEST_ASSERT_EQUAL_INT64(NOW, normalizeTimestampMs(NOW));
    TEST_ASSERT_EQUAL_INT64(1790000000000LL, normalizeTimestampMs(1790000000LL)); // seconds
    TEST_ASSERT_EQUAL_INT64(0, normalizeTimestampMs(0));
    TEST_ASSERT_EQUAL_INT64(0, normalizeTimestampMs(-5));
    TEST_ASSERT_EQUAL_INT64(0, normalizeTimestampMs(INT64_MIN));
    TEST_ASSERT_EQUAL_INT64(0, normalizeTimestampMs(INT64_MAX));
    TEST_ASSERT_EQUAL_INT64(0, normalizeTimestampMs(5000000000000LL)); // year 2128
}

void test_freshness()
{
    TEST_ASSERT_TRUE(isFresh(NOW, NOW, SKEW));
    TEST_ASSERT_TRUE(isFresh(NOW, NOW - SKEW, SKEW));
    TEST_ASSERT_FALSE(isFresh(NOW, NOW - 60000, SKEW)); // stale (-60 s)
    TEST_ASSERT_FALSE(isFresh(NOW, NOW + 60000, SKEW)); // future (+60 s)
    TEST_ASSERT_FALSE(isFresh(NOW, 0, SKEW));           // missing
}

void test_synthesized_event_id()
{
    char buf[64];
    TEST_ASSERT_EQUAL_STRING("abc", eventIdOrSynth("abc", NOW, "k", "d", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("1790000000000|sensor_button_pressed|AABBCCDDEE01",
                             eventIdOrSynth("", NOW, "sensor_button_pressed", "AABBCCDDEE01", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("1790000000000|k|d", eventIdOrSynth(nullptr, NOW, "k", "d", buf, sizeof(buf)));
}

// ---- Rule selection (bench: token, fob, key, button, gesture)

void test_valid_presses_select_their_rule()
{
    TEST_ASSERT_EQUAL_INT(0, match(TOKEN_HOLD, HOLD));
    TEST_ASSERT_EQUAL_INT(1, match(TOKEN_PRESS, PRESS));
}

void test_rejections()
{
    Trigger otherFob = HOLD;
    otherFob.device = "00:11:22:33:44:55";
    Trigger wrongKey = HOLD;
    wrongKey.key = "motion";
    Trigger otherButton = HOLD;
    otherButton.button = "left";
    Trigger otherGesture = HOLD;
    otherGesture.value = "doublePress";
    Trigger noButton = HOLD;
    noButton.button = nullptr;

    TEST_ASSERT_EQUAL_INT(-1, match(TOKEN_HOLD, otherFob));
    TEST_ASSERT_EQUAL_INT(-1, match("00000000000000000000000000000000", HOLD)); // wrong alarm token
    TEST_ASSERT_EQUAL_INT(-1, match("", HOLD));                                 // missing alarm token
    TEST_ASSERT_EQUAL_INT(-1, match(TOKEN_HOLD, wrongKey));
    TEST_ASSERT_EQUAL_INT(-1, match(TOKEN_HOLD, otherButton));
    TEST_ASSERT_EQUAL_INT(-1, match(TOKEN_HOLD, otherGesture));
    TEST_ASSERT_EQUAL_INT(-1, match(TOKEN_HOLD, noButton));
    // The token decides the rule: a hold sent to the press alarm's URL doesn't match.
    TEST_ASSERT_EQUAL_INT(-1, match(TOKEN_PRESS, HOLD));
}

// ---- Replay and per-rule cooldown (bench: replay, new press, other rule)

void test_replay_and_cooldown()
{
    ReplayGuard<2> guard;
    TEST_ASSERT_TRUE(guard.accept("ev1", 0, NOW, COOLDOWN));                  // valid press
    TEST_ASSERT_FALSE(guard.accept("ev1", 0, NOW + 100, COOLDOWN));           // replay same press
    TEST_ASSERT_FALSE(guard.accept("ev2", 0, NOW + 200, COOLDOWN));           // new press < 10 s
    TEST_ASSERT_TRUE(guard.accept("ev3", 1, NOW + 300, COOLDOWN));            // other rule in cooldown
    TEST_ASSERT_FALSE(guard.accept("ev1", 1, NOW + 400, COOLDOWN));           // replay via other rule
    TEST_ASSERT_TRUE(guard.accept("ev4", 0, NOW + COOLDOWN, COOLDOWN));       // after cooldown
    TEST_ASSERT_FALSE(guard.accept("ev4", 0, NOW + 3 * COOLDOWN, COOLDOWN));  // replay stays refused
}

void test_refused_event_is_not_remembered()
{
    ReplayGuard<1> guard;
    TEST_ASSERT_TRUE(guard.accept("a", 0, NOW, COOLDOWN));
    TEST_ASSERT_FALSE(guard.accept("b", 0, NOW + 1, COOLDOWN));        // cooldown, not recorded
    TEST_ASSERT_TRUE(guard.accept("b", 0, NOW + COOLDOWN, COOLDOWN));  // so it's accepted later
}

void test_first_event_at_time_zero()
{
    ReplayGuard<1> guard;
    TEST_ASSERT_TRUE(guard.accept("a", 0, 0, COOLDOWN));
    TEST_ASSERT_FALSE(guard.accept("b", 0, 1, COOLDOWN));
}

void test_long_event_ids_still_replay_protected()
{
    char longId[100];
    memset(longId, 'x', sizeof(longId) - 1);
    longId[sizeof(longId) - 1] = 0;
    ReplayGuard<1> guard;
    TEST_ASSERT_TRUE(guard.accept(longId, 0, NOW, 0));
    TEST_ASSERT_FALSE(guard.accept(longId, 0, NOW + 1, 0));
}

void test_replay_cache_is_bounded()
{
    ReplayGuard<1, 2> guard;
    TEST_ASSERT_TRUE(guard.accept("a", 0, NOW, 0));
    TEST_ASSERT_TRUE(guard.accept("b", 0, NOW, 0));
    TEST_ASSERT_TRUE(guard.accept("c", 0, NOW, 0));      // evicts "a"
    TEST_ASSERT_FALSE(guard.accept("c", 0, NOW, 0));
    TEST_ASSERT_TRUE(guard.accept("a", 0, NOW, 0));      // forgotten; freshness check covers this
}

void test_rule_index_out_of_range()
{
    ReplayGuard<1> guard;
    TEST_ASSERT_FALSE(guard.accept("a", 1, NOW, 0));
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_mac_ignores_case_and_separators);
    RUN_TEST(test_ct_equals);
    RUN_TEST(test_field_text_and_number);
    RUN_TEST(test_timestamp_normalisation);
    RUN_TEST(test_freshness);
    RUN_TEST(test_synthesized_event_id);
    RUN_TEST(test_valid_presses_select_their_rule);
    RUN_TEST(test_rejections);
    RUN_TEST(test_replay_and_cooldown);
    RUN_TEST(test_refused_event_is_not_remembered);
    RUN_TEST(test_first_event_at_time_zero);
    RUN_TEST(test_long_event_ids_still_replay_protected);
    RUN_TEST(test_replay_cache_is_bounded);
    RUN_TEST(test_rule_index_out_of_range);
    return UNITY_END();
}
