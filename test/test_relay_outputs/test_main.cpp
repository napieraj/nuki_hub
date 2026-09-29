// Host tests for src/RelayOutputsLogic.h (relay state outputs): pio test -e native

#include <unity.h>
#include <cstring>
#include <initializer_list>
#include "RelayOutputsLogic.h"

using namespace RelayOutputsLogic;

namespace
{
    Field f(uint8_t value, int64_t ms)
    {
        return { true, value, ms };
    }

    Inputs known(uint8_t lockState, uint8_t doorState)
    {
        Inputs in = {};
        in.haveLock = true;
        in.lockState = lockState;
        in.haveDoor = true;
        in.doorState = doorState;
        return in;
    }

    BleInputs ble(int64_t ms, uint8_t lockState, uint8_t doorState)
    {
        BleInputs b = {};
        b.have = true;
        b.ms = ms;
        b.lockState = lockState;
        b.doorState = doorState;
        b.accessoryBatteryState = 255;
        b.nightModeActive = 255;
        b.completionStatus = 0xFF; // Unknown
        return b;
    }

    LogView keypad(uint32_t index, uint8_t completion, uint16_t codeId, uint8_t source = 1)
    {
        LogView e = {};
        e.index = index;
        e.type = LOG_TYPE_KEYPAD;
        e.data[0] = 1;
        e.data[1] = source;
        e.data[2] = completion;
        e.data[3] = (uint8_t)(codeId & 0xff);
        e.data[4] = (uint8_t)(codeId >> 8);
        return e;
    }

    LogView door(uint32_t index, uint8_t what)
    {
        LogView e = {};
        e.index = index;
        e.type = LOG_TYPE_DOOR_SENSOR;
        e.data[0] = what;
        return e;
    }

    struct Events
    {
        int wrong = 0;
        int valid = 0;
        int jammed = 0;
        int openClose = 0;
        uint16_t lastCode = 0;
        LogEvent order[8] = {};
        int n = 0;
    };

    size_t run(LogTracker& t, const LogView* e, size_t n, Events& ev)
    {
        return processLog(t, e, n, [&ev](LogEvent x, uint16_t code)
        {
            if(ev.n < 8) ev.order[ev.n++] = x;
            ev.lastCode = code;
            ev.wrong += x == LogEvent::KeypadWrong;
            ev.valid += x == LogEvent::KeypadValid;
            ev.jammed += x == LogEvent::DoorJammed;
            ev.openClose += x == LogEvent::DoorOpenedClosed;
        });
    }
}

void setUp() {}
void tearDown() {}

void test_role_values_are_stable()
{
    // Stored in NVS: never renumber.
    TEST_ASSERT_EQUAL_UINT8(0, (uint8_t)RelayRole::Off);
    TEST_ASSERT_EQUAL_UINT8(1, (uint8_t)RelayRole::WebhookPulse);
    TEST_ASSERT_EQUAL_UINT8(2, (uint8_t)RelayRole::Secure);
    TEST_ASSERT_EQUAL_UINT8(3, (uint8_t)RelayRole::DoorOpen);
    TEST_ASSERT_EQUAL_UINT8(4, (uint8_t)RelayRole::Locked);
    TEST_ASSERT_EQUAL_UINT8(5, (uint8_t)RelayRole::LockFault);
    TEST_ASSERT_EQUAL_UINT8(6, (uint8_t)RelayRole::BatteryLow);
    TEST_ASSERT_EQUAL_UINT8(7, (uint8_t)RelayRole::DoorSensorFault);
    TEST_ASSERT_EQUAL_UINT8(8, (uint8_t)RelayRole::KeypadWrongCode);
    TEST_ASSERT_EQUAL_UINT8(9, (uint8_t)RelayRole::KeypadValidEntry);
    TEST_ASSERT_EQUAL_UINT8(10, (uint8_t)RelayRole::Unlocked);
    TEST_ASSERT_EQUAL_UINT8(11, (uint8_t)RelayRole::NightMode);
    TEST_ASSERT_EQUAL_UINT8(12, ROLE_COUNT);
    TEST_ASSERT_TRUE(roleFromStored(12) == RelayRole::Off);
    TEST_ASSERT_TRUE(roleFromStored(255) == RelayRole::Off);
    for(uint8_t i = 0; i < ROLE_COUNT; i++)
    {
        TEST_ASSERT_NOT_NULL(roleName((RelayRole)i));
    }
}

void test_defaults_table()
{
    TEST_ASSERT_TRUE(DEFAULTS[0].role == RelayRole::WebhookPulse && !DEFAULTS[0].invert);
    TEST_ASSERT_TRUE(DEFAULTS[1].role == RelayRole::Secure && !DEFAULTS[1].invert);
    TEST_ASSERT_TRUE(DEFAULTS[2].role == RelayRole::DoorOpen && !DEFAULTS[2].invert);
    TEST_ASSERT_TRUE(DEFAULTS[3].role == RelayRole::Locked && !DEFAULTS[3].invert);
    TEST_ASSERT_TRUE(DEFAULTS[4].role == RelayRole::LockFault && DEFAULTS[4].invert);
    TEST_ASSERT_TRUE(DEFAULTS[5].role == RelayRole::BatteryLow && DEFAULTS[5].invert);
    TEST_ASSERT_TRUE(DEFAULTS[6].role == RelayRole::DoorSensorFault && DEFAULTS[6].invert);
    TEST_ASSERT_TRUE(DEFAULTS[7].role == RelayRole::KeypadWrongCode && !DEFAULTS[7].invert);
}

void test_steady_roles()
{
    const Inputs lockedClosed = known(LOCK_LOCKED, DOOR_CLOSED);
    TEST_ASSERT_TRUE(steadyClosed(RelayRole::Secure, lockedClosed));
    TEST_ASSERT_TRUE(steadyClosed(RelayRole::Locked, lockedClosed));
    TEST_ASSERT_FALSE(steadyClosed(RelayRole::Unlocked, lockedClosed));
    TEST_ASSERT_FALSE(steadyClosed(RelayRole::DoorOpen, lockedClosed));

    TEST_ASSERT_FALSE(steadyClosed(RelayRole::Secure, known(LOCK_LOCKED, DOOR_OPENED)));
    TEST_ASSERT_TRUE(steadyClosed(RelayRole::DoorOpen, known(LOCK_LOCKED, DOOR_OPENED)));
    TEST_ASSERT_FALSE(steadyClosed(RelayRole::Secure, known(0x04 /* Locking */, DOOR_CLOSED)));
    TEST_ASSERT_FALSE(steadyClosed(RelayRole::Locked, known(0x04 /* Locking */, DOOR_CLOSED)));
    TEST_ASSERT_TRUE(steadyClosed(RelayRole::Unlocked, known(LOCK_UNLOCKED, DOOR_CLOSED)));
    TEST_ASSERT_TRUE(steadyClosed(RelayRole::Unlocked, known(LOCK_UNLATCHED, DOOR_CLOSED)));
    TEST_ASSERT_TRUE(steadyClosed(RelayRole::Unlocked, known(LOCK_UNLOCKED_LNGA, DOOR_CLOSED)));
    TEST_ASSERT_FALSE(steadyClosed(RelayRole::Unlocked, known(0x02 /* Unlocking */, DOOR_CLOSED)));

    // Secure with no door sensor state known: not confirmed, so not secure.
    Inputs noDoor = {};
    noDoor.haveLock = true;
    noDoor.lockState = LOCK_LOCKED;
    TEST_ASSERT_FALSE(steadyClosed(RelayRole::Secure, noDoor));
    TEST_ASSERT_TRUE(steadyClosed(RelayRole::Locked, noDoor));

    // Nothing known: everything open.
    const Inputs none = {};
    for(uint8_t r = 0; r < ROLE_COUNT; r++)
    {
        TEST_ASSERT_FALSE(steadyClosed((RelayRole)r, none));
    }

    Inputs night = lockedClosed;
    night.nightMode = true;
    TEST_ASSERT_TRUE(steadyClosed(RelayRole::NightMode, night));
}

void test_lock_fault()
{
    Inputs in = known(LOCK_LOCKED, DOOR_CLOSED);
    TEST_ASSERT_FALSE(steadyClosed(RelayRole::LockFault, in));
    in.lockState = LOCK_MOTOR_BLOCKED;
    TEST_ASSERT_TRUE(steadyClosed(RelayRole::LockFault, in));
    in.lockState = LOCK_LOCKED;
    in.haveCompletion = true;
    const uint8_t faults[] = { 0x01, 0x05, 0x06, 0x07, 0x08, 0x0B };
    for(uint8_t c : faults)
    {
        in.completionStatus = c;
        TEST_ASSERT_TRUE(steadyClosed(RelayRole::LockFault, in));
    }
    const uint8_t fine[] = { 0x00, 0x02, 0x03, 0x04, 0xE0, 0xFE, 0xFF };
    for(uint8_t c : fine)
    {
        in.completionStatus = c;
        TEST_ASSERT_FALSE(steadyClosed(RelayRole::LockFault, in));
    }
    in.bleCommError = true;
    TEST_ASSERT_TRUE(steadyClosed(RelayRole::LockFault, in));
    in.bleCommError = false;
    in.stale = true;
    TEST_ASSERT_TRUE(steadyClosed(RelayRole::LockFault, in));
}

void test_battery_bits()
{
    TEST_ASSERT_TRUE(bleLockCritical(0x01));
    TEST_ASSERT_FALSE(bleLockCritical(0x02)); // charging
    TEST_ASSERT_FALSE(bleLockCritical(0xFC));
    // Accessory byte: keypad bits 0 (reported) + 1 (critical), door sensor 2 + 3.
    TEST_ASSERT_FALSE(bleKeypadCritical(255));
    TEST_ASSERT_FALSE(bleDoorSensorCritical(255));
    TEST_ASSERT_FALSE(bleKeypadCritical(0x01));
    TEST_ASSERT_TRUE(bleKeypadCritical(0x03));
    TEST_ASSERT_FALSE(bleKeypadCritical(0x02)); // critical bit without "reported"
    TEST_ASSERT_TRUE(bleDoorSensorCritical(0x0C));
    TEST_ASSERT_FALSE(bleDoorSensorCritical(0x04));
    TEST_ASSERT_FALSE(bleDoorSensorCritical(0x08));
    TEST_ASSERT_TRUE(bleKeypadBatteryReported(0x05));
    TEST_ASSERT_FALSE(bleKeypadBatteryReported(0x04));
    TEST_ASSERT_FALSE(bleKeypadBatteryReported(255));

    Inputs in = {};
    TEST_ASSERT_FALSE(steadyClosed(RelayRole::BatteryLow, in));
    in.keypadCritical = true;
    TEST_ASSERT_TRUE(steadyClosed(RelayRole::BatteryLow, in));
    in.keypadCritical = false;
    in.doorCritical = true;
    TEST_ASSERT_TRUE(steadyClosed(RelayRole::BatteryLow, in));
}

void test_door_sensor_fault()
{
    TEST_ASSERT_FALSE(steadyClosed(RelayRole::DoorSensorFault, known(LOCK_LOCKED, DOOR_CLOSED)));
    TEST_ASSERT_FALSE(steadyClosed(RelayRole::DoorSensorFault, known(LOCK_LOCKED, DOOR_OPENED)));
    TEST_ASSERT_FALSE(steadyClosed(RelayRole::DoorSensorFault, known(LOCK_LOCKED, 0x01 /* deactivated */)));
    TEST_ASSERT_TRUE(steadyClosed(RelayRole::DoorSensorFault, known(LOCK_LOCKED, DOOR_UNKNOWN)));
    TEST_ASSERT_TRUE(steadyClosed(RelayRole::DoorSensorFault, known(LOCK_LOCKED, DOOR_UNCALIBRATED)));
    TEST_ASSERT_TRUE(steadyClosed(RelayRole::DoorSensorFault, known(LOCK_LOCKED, DOOR_TAMPERED)));
    Inputs in = known(LOCK_LOCKED, DOOR_CLOSED);
    in.doorJammed = true;
    TEST_ASSERT_TRUE(steadyClosed(RelayRole::DoorSensorFault, in));
    in.doorState = DOOR_CALIBRATING; // never while calibrating
    TEST_ASSERT_FALSE(steadyClosed(RelayRole::DoorSensorFault, in));

    JamLatch latch = {};
    latch.onLog(LogEvent::DoorJammed);
    TEST_ASSERT_TRUE(latch.latched);
    latch.onDoorStateChange(DOOR_UNKNOWN);
    TEST_ASSERT_TRUE(latch.latched);
    latch.onDoorStateChange(DOOR_CLOSED);
    TEST_ASSERT_FALSE(latch.latched);
    latch.onLog(LogEvent::DoorJammed);
    latch.onLog(LogEvent::DoorOpenedClosed);
    TEST_ASSERT_FALSE(latch.latched);
}

void test_invert_and_arming()
{
    const Inputs lockedClosed = known(LOCK_LOCKED, DOOR_CLOSED);
    // Not armed: every relay open, inverted ones too.
    for(uint8_t r = 0; r < ROLE_COUNT; r++)
    {
        TEST_ASSERT_FALSE(relayLevel((RelayRole)r, true, lockedClosed, false));
        TEST_ASSERT_FALSE(relayLevel((RelayRole)r, false, lockedClosed, false));
    }
    TEST_ASSERT_TRUE(relayLevel(RelayRole::Locked, false, lockedClosed, true));
    TEST_ASSERT_FALSE(relayLevel(RelayRole::Locked, true, lockedClosed, true));
    // Inverted fault role: closed = OK.
    TEST_ASSERT_TRUE(relayLevel(RelayRole::LockFault, true, lockedClosed, true));
    Inputs stale = lockedClosed;
    stale.stale = true;
    TEST_ASSERT_FALSE(relayLevel(RelayRole::LockFault, true, stale, true));
    // Off and WebhookPulse rest open, even inverted.
    TEST_ASSERT_FALSE(relayLevel(RelayRole::Off, true, lockedClosed, true));
    TEST_ASSERT_FALSE(relayLevel(RelayRole::WebhookPulse, true, lockedClosed, true));
    // Keypad pulse roles rest at the inverse of their pulse level.
    TEST_ASSERT_FALSE(relayLevel(RelayRole::KeypadWrongCode, false, lockedClosed, true));
    TEST_ASSERT_TRUE(relayLevel(RelayRole::KeypadWrongCode, true, lockedClosed, true));
}

void test_target_levels_defaults()
{
    RelayConfig cfg[RELAYS];
    for(size_t i = 0; i < RELAYS; i++)
    {
        cfg[i] = { DEFAULTS[i].role, DEFAULTS[i].invert, 3000 };
    }
    const Inputs lockedClosed = known(LOCK_LOCKED, DOOR_CLOSED);
    // Master off: all open.
    TEST_ASSERT_EQUAL_HEX8(0x00, targetLevels(cfg, RELAYS, false, lockedClosed, true));
    // Before the first state: all open.
    TEST_ASSERT_EQUAL_HEX8(0x00, targetLevels(cfg, RELAYS, true, lockedClosed, false));
    // Locked + closed, all OK: 2 Secure, 4 Locked, 5/6/7 inverted OK.
    TEST_ASSERT_EQUAL_HEX8(0x02 | 0x08 | 0x10 | 0x20 | 0x40, targetLevels(cfg, RELAYS, true, lockedClosed, true));
    // Door open, unlocked: 3 DoorOpen, 5/6/7 OK.
    TEST_ASSERT_EQUAL_HEX8(0x04 | 0x10 | 0x20 | 0x40, targetLevels(cfg, RELAYS, true, known(LOCK_UNLOCKED, DOOR_OPENED), true));
}

void test_merge_prefers_newer_source()
{
    MqttInputs m = {};
    m.lockState = f(LOCK_UNLOCKED, 2000);
    BleInputs b = ble(1000, LOCK_LOCKED, DOOR_CLOSED);

    Inputs in = merge(m, true, b);
    TEST_ASSERT_TRUE(in.haveLock);
    TEST_ASSERT_EQUAL_UINT8(LOCK_UNLOCKED, in.lockState); // MQTT newer
    TEST_ASSERT_TRUE(in.haveDoor);
    TEST_ASSERT_EQUAL_UINT8(DOOR_CLOSED, in.doorState);   // MQTT lacks it: BLE

    b.ms = 3000;                                          // BLE read after the MQTT message
    in = merge(m, true, b);
    TEST_ASSERT_EQUAL_UINT8(LOCK_LOCKED, in.lockState);

    b.ms = 2000;                                          // tie: MQTT
    in = merge(m, true, b);
    TEST_ASSERT_EQUAL_UINT8(LOCK_UNLOCKED, in.lockState);

    b.ms = 1000;
    in = merge(m, false, b);                              // session not live: BLE only
    TEST_ASSERT_EQUAL_UINT8(LOCK_LOCKED, in.lockState);

    // Nothing at all.
    in = merge(MqttInputs{}, true, BleInputs{});
    TEST_ASSERT_FALSE(in.haveLock);
    TEST_ASSERT_FALSE(in.haveDoor);
    TEST_ASSERT_FALSE(in.haveCompletion);

    // Batteries per flag; night mode and completion only from BLE.
    m = {};
    m.keypadCritical = f(1, 5000);
    m.lockCritical = f(0, 5000);
    b = ble(1000, LOCK_LOCKED, DOOR_CLOSED);
    b.criticalBatteryState = 0x01;
    b.accessoryBatteryState = 0x0C;
    b.nightModeActive = 1;
    b.completionStatus = 0x01;
    in = merge(m, true, b);
    TEST_ASSERT_TRUE(in.keypadCritical);
    TEST_ASSERT_FALSE(in.lockCritical);   // MQTT "false" is newer than BLE "critical"
    TEST_ASSERT_TRUE(in.doorCritical);    // BLE only
    TEST_ASSERT_TRUE(in.nightMode);
    TEST_ASSERT_TRUE(in.haveCompletion);
    TEST_ASSERT_EQUAL_UINT8(0x01, in.completionStatus);
}

void test_stale_rule()
{
    const int64_t mqttFresh = 330000;
    const int64_t bleFresh = bleFreshMs(1800);
    TEST_ASSERT_EQUAL_INT64(3660000, bleFresh);
    TEST_ASSERT_EQUAL_INT64(3660000, bleFreshMs(0)); // unset: upstream default 1800 s
    // Nothing: stale.
    TEST_ASSERT_TRUE(isStale(false, -1, mqttFresh, false, -1, bleFresh));
    // Live MQTT session heard from recently: fresh, whatever BLE says.
    TEST_ASSERT_FALSE(isStale(true, 1000, mqttFresh, false, -1, bleFresh));
    TEST_ASSERT_FALSE(isStale(true, 330000, mqttFresh, true, 9999999, bleFresh));
    // Live but silent too long, and BLE old: stale.
    TEST_ASSERT_TRUE(isStale(true, 330001, mqttFresh, true, 3660001, bleFresh));
    // BLE read recent enough: fresh.
    TEST_ASSERT_FALSE(isStale(false, -1, mqttFresh, true, 3660000, bleFresh));
    TEST_ASSERT_FALSE(isStale(true, 999999, mqttFresh, true, 1000, bleFresh));
    // Not live: the MQTT age doesn't count.
    TEST_ASSERT_TRUE(isStale(false, 10, mqttFresh, false, -1, bleFresh));
}

void test_lock_action_event()
{
    LockActionEvent e;
    TEST_ASSERT_TRUE(parseLockActionEvent("3,0,54321,12345,1", e));
    TEST_ASSERT_EQUAL_UINT32(3, e.action);
    TEST_ASSERT_EQUAL_UINT32(0, e.trigger);
    TEST_ASSERT_EQUAL_UINT32(54321, e.authId);
    TEST_ASSERT_EQUAL_UINT32(12345, e.codeId);
    TEST_ASSERT_EQUAL_UINT32(1, e.context);
    TEST_ASSERT_TRUE(isKeypadEntry(e));
    TEST_ASSERT_TRUE(parseLockActionEvent("1,0,7,9,2", e)); // fingerprint
    TEST_ASSERT_TRUE(isKeypadEntry(e));
    TEST_ASSERT_TRUE(parseLockActionEvent("1,0,7,9,0", e)); // back key with a code ID? not a code entry
    TEST_ASSERT_FALSE(isKeypadEntry(e));
    // The 4th/5th gen wrong-code bug publishes Code-ID 0: not a valid entry.
    TEST_ASSERT_TRUE(parseLockActionEvent("1,0,192100,0,1", e));
    TEST_ASSERT_FALSE(isKeypadEntry(e));
    // App, button, MQTT actions.
    TEST_ASSERT_TRUE(parseLockActionEvent("2,172,0,0,0", e));
    TEST_ASSERT_FALSE(isKeypadEntry(e));
    TEST_ASSERT_TRUE(parseLockActionEvent("1,2,0,0,2", e));
    TEST_ASSERT_FALSE(isKeypadEntry(e));
    TEST_ASSERT_TRUE(parseLockActionEvent("1,0,1,2,1\n", e));
    TEST_ASSERT_TRUE(parseLockActionEvent("1,0,1,2,1,5", e)); // more fields: ignored

    TEST_ASSERT_FALSE(parseLockActionEvent("", e));
    TEST_ASSERT_FALSE(parseLockActionEvent(nullptr, e));
    TEST_ASSERT_FALSE(parseLockActionEvent("1,0,1,2", e));
    TEST_ASSERT_FALSE(parseLockActionEvent("1,,1,2,1", e));
    TEST_ASSERT_FALSE(parseLockActionEvent("a,0,1,2,1", e));
    TEST_ASSERT_FALSE(parseLockActionEvent("-1,0,1,2,1", e));
    TEST_ASSERT_FALSE(parseLockActionEvent("1,0,1,2,1x", e));
}

void test_log_baseline_and_new_entries()
{
    LogTracker t = {};
    Events ev;
    // First read after a cold boot: only the baseline, no pulses.
    LogView first[] = { keypad(10, COMPLETION_INVALID_CODE, 1), keypad(11, COMPLETION_SUCCESS, 1) };
    TEST_ASSERT_EQUAL(0, (int)run(t, first, 2, ev));
    TEST_ASSERT_TRUE(t.haveBaseline);
    TEST_ASSERT_EQUAL_UINT32(11, t.lastIndex);
    TEST_ASSERT_EQUAL(0, ev.wrong + ev.valid);

    // The same entries again: nothing.
    TEST_ASSERT_EQUAL(0, (int)run(t, first, 2, ev));

    // New entries (any order in the list), handled oldest first.
    LogView next[] = { keypad(14, COMPLETION_SUCCESS, 0x1234), keypad(11, COMPLETION_SUCCESS, 1),
                       keypad(12, COMPLETION_INVALID_CODE, 0), keypad(13, COMPLETION_NOT_AUTHORIZED, 5) };
    TEST_ASSERT_EQUAL(3, (int)run(t, next, 4, ev));
    TEST_ASSERT_EQUAL(2, ev.wrong);
    TEST_ASSERT_EQUAL(1, ev.valid);
    TEST_ASSERT_TRUE(ev.order[0] == LogEvent::KeypadWrong);
    TEST_ASSERT_TRUE(ev.order[2] == LogEvent::KeypadValid);
    TEST_ASSERT_EQUAL_UINT16(0x1234, ev.lastCode);
    TEST_ASSERT_EQUAL_UINT32(14, t.lastIndex);

    // An empty read doesn't set a baseline.
    LogTracker fresh = {};
    TEST_ASSERT_EQUAL(0, (int)run(fresh, nullptr, 0, ev));
    TEST_ASSERT_FALSE(fresh.haveBaseline);

    // A baseline kept across a software reset (RTC memory): new entries fire.
    LogTracker kept = { true, 20 };
    Events ev2;
    LogView after[] = { keypad(21, COMPLETION_INVALID_CODE, 0), door(22, 2), door(23, 1), keypad(24, 0x04, 3) };
    TEST_ASSERT_EQUAL(4, (int)run(kept, after, 4, ev2));
    TEST_ASSERT_EQUAL(1, ev2.wrong);
    TEST_ASSERT_EQUAL(0, ev2.valid); // completion Busy: neither
    TEST_ASSERT_EQUAL(1, ev2.jammed);
    TEST_ASSERT_EQUAL(1, ev2.openClose);
}

void test_log_classify_sources()
{
    uint16_t code = 0;
    // Any keypad source counts (arrow key, code, fingerprint, others such as NFC).
    for(uint8_t source : { 0, 1, 2, 3, 7 })
    {
        TEST_ASSERT_TRUE(classify(keypad(1, COMPLETION_SUCCESS, 9, source), code) == LogEvent::KeypadValid);
        TEST_ASSERT_TRUE(classify(keypad(1, COMPLETION_INVALID_CODE, 9, source), code) == LogEvent::KeypadWrong);
    }
    TEST_ASSERT_EQUAL_UINT16(9, code);
    LogView lockAction = {};
    lockAction.type = 0x02;
    lockAction.data[3] = 0x00;
    TEST_ASSERT_TRUE(classify(lockAction, code) == LogEvent::None);
    TEST_ASSERT_TRUE(classify(door(1, 0), code) == LogEvent::DoorOpenedClosed);
    TEST_ASSERT_TRUE(classify(door(1, 1), code) == LogEvent::DoorOpenedClosed);
    TEST_ASSERT_TRUE(classify(door(1, 2), code) == LogEvent::DoorJammed);
    TEST_ASSERT_TRUE(classify(door(1, 3), code) == LogEvent::None);
}

void test_keypad_dedupe()
{
    KeypadDedupe d = {};
    // Not seen over MQTT: the log entry pulses.
    TEST_ASSERT_FALSE(d.consumeLog(5, 1000));
    // Seen over MQTT: the matching log entry is consumed once.
    d.noteMqtt(5, 1000);
    d.noteMqtt(5, 2000);
    TEST_ASSERT_FALSE(d.consumeLog(6, 3000)); // other code
    TEST_ASSERT_TRUE(d.consumeLog(5, 3000));
    TEST_ASSERT_TRUE(d.consumeLog(5, 3000));
    TEST_ASSERT_FALSE(d.consumeLog(5, 3000));
    // Too old: the log entry pulses (MQTT entry expired).
    d.noteMqtt(7, 0);
    TEST_ASSERT_FALSE(d.consumeLog(7, KeypadDedupe::WINDOW_MS + 1));
    // More MQTT entries than slots: the oldest are dropped.
    for(uint16_t c = 1; c <= 6; c++)
    {
        d.noteMqtt(c, 10000 + c);
    }
    TEST_ASSERT_FALSE(d.consumeLog(1, 20000));
    TEST_ASSERT_TRUE(d.consumeLog(6, 20000));
}

void test_role_classes()
{
    TEST_ASSERT_TRUE(isPulseRole(RelayRole::WebhookPulse));
    TEST_ASSERT_TRUE(isPulseRole(RelayRole::KeypadWrongCode));
    TEST_ASSERT_TRUE(isPulseRole(RelayRole::KeypadValidEntry));
    TEST_ASSERT_FALSE(isPulseRole(RelayRole::Secure));
    TEST_ASSERT_TRUE(usesActivityLog(RelayRole::KeypadWrongCode));
    TEST_ASSERT_TRUE(usesActivityLog(RelayRole::DoorSensorFault));
    TEST_ASSERT_FALSE(usesActivityLog(RelayRole::Secure));
    TEST_ASSERT_TRUE(isKeypadRole(RelayRole::KeypadValidEntry));
    TEST_ASSERT_FALSE(isKeypadRole(RelayRole::DoorSensorFault));
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_role_values_are_stable);
    RUN_TEST(test_defaults_table);
    RUN_TEST(test_steady_roles);
    RUN_TEST(test_lock_fault);
    RUN_TEST(test_battery_bits);
    RUN_TEST(test_door_sensor_fault);
    RUN_TEST(test_invert_and_arming);
    RUN_TEST(test_target_levels_defaults);
    RUN_TEST(test_merge_prefers_newer_source);
    RUN_TEST(test_stale_rule);
    RUN_TEST(test_lock_action_event);
    RUN_TEST(test_log_baseline_and_new_entries);
    RUN_TEST(test_log_classify_sources);
    RUN_TEST(test_keypad_dedupe);
    RUN_TEST(test_role_classes);
    return UNITY_END();
}
