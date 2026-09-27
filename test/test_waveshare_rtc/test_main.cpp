// Host tests for src/WaveshareRtcLogic.h (PCF85063 <-> UTC): pio test -e native

#include <unity.h>
#include <cstdint>
#include <ctime>
#include "WaveshareRtcLogic.h"

using namespace WaveshareRtcLogic;

namespace
{
    // Registers 0x04..0x0A for 2026-09-27 13:21:54 UTC (a Sunday).
    const uint8_t SAMPLE[7] = { 0x54, 0x21, 0x13, 0x27, 0x00, 0x09, 0x26 };
    const int64_t SAMPLE_EPOCH = 1790515314;
}

void setUp() {}
void tearDown() {}

void test_decode_valid()
{
    int64_t e = 0;
    TEST_ASSERT_EQUAL(RtcStatus::Ok, decode(0x00, SAMPLE, e));
    TEST_ASSERT_EQUAL_INT64(SAMPLE_EPOCH, e);
    // CAP_SEL (bit 0) and the unused high bits of the date registers are ignored.
    uint8_t t[7];
    for(int i = 0; i < 7; i++) t[i] = SAMPLE[i];
    t[3] |= 0xc0;
    t[5] |= 0xe0;
    TEST_ASSERT_EQUAL(RtcStatus::Ok, decode(0x01, t, e));
    TEST_ASSERT_EQUAL_INT64(SAMPLE_EPOCH, e);
}

void test_decode_rejects_oscillator_stop_and_modes()
{
    int64_t e = 42;
    uint8_t t[7];
    for(int i = 0; i < 7; i++) t[i] = SAMPLE[i];
    t[0] |= SECONDS_OS;
    TEST_ASSERT_EQUAL(RtcStatus::OscillatorStopped, decode(0x00, t, e));
    TEST_ASSERT_EQUAL(RtcStatus::ClockStopped, decode(CTRL1_STOP, SAMPLE, e));
    TEST_ASSERT_EQUAL(RtcStatus::TwelveHourMode, decode(CTRL1_12_24, SAMPLE, e));
    TEST_ASSERT_EQUAL_INT64(42, e); // untouched on failure
}

void test_decode_rejects_garbage()
{
    int64_t e = 0;
    // Power-on reset values (2000-01-01, OS clear here): before the floor.
    const uint8_t reset[7] = { 0x00, 0x00, 0x00, 0x01, 0x06, 0x01, 0x00 };
    TEST_ASSERT_EQUAL(RtcStatus::Invalid, decode(0x00, reset, e));
    // Non-BCD nibbles.
    uint8_t t[7];
    for(int i = 0; i < 7; i++) t[i] = SAMPLE[i];
    t[6] = 0x2a;
    TEST_ASSERT_EQUAL(RtcStatus::Invalid, decode(0x00, t, e));
    t[6] = 0x26;
    t[1] = 0x5f;
    TEST_ASSERT_EQUAL(RtcStatus::Invalid, decode(0x00, t, e));
    // Impossible dates.
    const uint8_t feb30[7] = { 0x00, 0x00, 0x00, 0x30, 0x00, 0x02, 0x27 };
    TEST_ASSERT_EQUAL(RtcStatus::Invalid, decode(0x00, feb30, e));
    const uint8_t feb29nonleap[7] = { 0x00, 0x00, 0x00, 0x29, 0x00, 0x02, 0x27 };
    TEST_ASSERT_EQUAL(RtcStatus::Invalid, decode(0x00, feb29nonleap, e));
    const uint8_t feb29leap[7] = { 0x00, 0x00, 0x00, 0x29, 0x00, 0x02, 0x28 };
    TEST_ASSERT_EQUAL(RtcStatus::Ok, decode(0x00, feb29leap, e));
    const uint8_t hour24[7] = { 0x00, 0x00, 0x24, 0x01, 0x00, 0x01, 0x27 };
    TEST_ASSERT_EQUAL(RtcStatus::Invalid, decode(0x00, hour24, e));
    const uint8_t month13[7] = { 0x00, 0x00, 0x00, 0x01, 0x00, 0x13, 0x27 };
    TEST_ASSERT_EQUAL(RtcStatus::Invalid, decode(0x00, month13, e));
}

void test_encode_sample()
{
    uint8_t t[7];
    TEST_ASSERT_TRUE(encode(SAMPLE_EPOCH, t));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(SAMPLE, t, 7);
}

void test_encode_range()
{
    uint8_t t[7];
    TEST_ASSERT_FALSE(encode(0, t));
    TEST_ASSERT_FALSE(encode(TIME_FLOOR - 1, t));
    TEST_ASSERT_TRUE(encode(TIME_FLOOR, t));
    TEST_ASSERT_TRUE(encode(TIME_CEIL - 1, t));
    TEST_ASSERT_FALSE(encode(TIME_CEIL, t));
}

// Every hour-ish from 2026 to 2099: round trip, and fields match gmtime (UTC).
void test_roundtrip_against_gmtime()
{
    for(int64_t e = TIME_FLOOR; e < TIME_CEIL; e += 3599 * 7 + 13)
    {
        uint8_t t[7];
        TEST_ASSERT_TRUE(encode(e, t));
        int64_t back = 0;
        TEST_ASSERT_EQUAL(RtcStatus::Ok, decode(0x00, t, back));
        TEST_ASSERT_EQUAL_INT64(e, back);

        const time_t tt = (time_t)e;
        struct tm g;
        gmtime_r(&tt, &g);
        TEST_ASSERT_EQUAL_INT(g.tm_sec, fromBcd(t[0]));
        TEST_ASSERT_EQUAL_INT(g.tm_min, fromBcd(t[1]));
        TEST_ASSERT_EQUAL_INT(g.tm_hour, fromBcd(t[2]));
        TEST_ASSERT_EQUAL_INT(g.tm_mday, fromBcd(t[3]));
        TEST_ASSERT_EQUAL_INT(g.tm_wday, t[4]);
        TEST_ASSERT_EQUAL_INT(g.tm_mon + 1, fromBcd(t[5]));
        TEST_ASSERT_EQUAL_INT(g.tm_year - 100, fromBcd(t[6]));
    }
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_decode_valid);
    RUN_TEST(test_decode_rejects_oscillator_stop_and_modes);
    RUN_TEST(test_decode_rejects_garbage);
    RUN_TEST(test_encode_sample);
    RUN_TEST(test_encode_range);
    RUN_TEST(test_roundtrip_against_gmtime);
    return UNITY_END();
}
