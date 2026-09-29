#pragma once

// Relay state outputs (Waveshare 8DI-8RO), pure part: the relay roles, their
// defaults, merging the lock's MQTT and BLE state, the stale rule, the
// evaluation role + invert + state -> relay closed/open, and the activity-log
// and lockActionEvent decoding for the event (pulse) roles. No Arduino, NVS or
// FreeRTOS: shared by ForkSettingsLogic.h, WaveshareOutputs.cpp, the web page
// and the host tests (pio test -e native, test_relay_outputs).
//
// "Closed" = relay coil energized (COM-NO closed). Invert flips it.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace RelayOutputsLogic
{
    constexpr uint8_t RELAYS = 8;

    // Stored in NVS (forkcfg blob): APPEND ONLY, never renumber. An unknown
    // stored value reads as Off.
    enum class RelayRole : uint8_t
    {
        Off = 0,
        WebhookPulse = 1,     // P: a Protect webhook rule "relayN" (today's behaviour)
        Secure = 2,           // S: locked and door closed
        DoorOpen = 3,         // S: door opened
        Locked = 4,           // S: locked (not locking)
        LockFault = 5,        // S: motor blocked, failed last action, BLE error, state stale
        BatteryLow = 6,       // S: lock, keypad or door sensor battery critical
        DoorSensorFault = 7,  // S: door sensor unknown/uncalibrated/tampered, or jammed (log)
        KeypadWrongCode = 8,  // P: keypad log entry "invalid code" / "not authorized"
        KeypadValidEntry = 9, // P: valid keypad entry (MQTT lockActionEvent or log)
        Unlocked = 10,        // S: unlocked, unlocked (lock 'n' go), unlatched
        NightMode = 11,       // S: night mode active (BLE only)
    };
    constexpr uint8_t ROLE_COUNT = 12;

    inline RelayRole roleFromStored(uint8_t v)
    {
        return v < ROLE_COUNT ? (RelayRole)v : RelayRole::Off;
    }

    inline const char* roleName(RelayRole r)
    {
        switch(r)
        {
        case RelayRole::Off: return "Off";
        case RelayRole::WebhookPulse: return "Webhook pulse";
        case RelayRole::Secure: return "Secure (locked + door closed)";
        case RelayRole::DoorOpen: return "Door open";
        case RelayRole::Locked: return "Locked";
        case RelayRole::LockFault: return "Lock fault";
        case RelayRole::BatteryLow: return "Battery low";
        case RelayRole::DoorSensorFault: return "Door sensor fault";
        case RelayRole::KeypadWrongCode: return "Keypad wrong code (pulse)";
        case RelayRole::KeypadValidEntry: return "Keypad valid entry (pulse)";
        case RelayRole::Unlocked: return "Unlocked";
        case RelayRole::NightMode: return "Night mode";
        }
        return "Off";
    }

    inline bool isPulseRole(RelayRole r)
    {
        return r == RelayRole::WebhookPulse || r == RelayRole::KeypadWrongCode || r == RelayRole::KeypadValidEntry;
    }

    // Needs the lock's activity log over BLE ("Publish auth data" + valid PIN).
    // DoorSensorFault only for its "jammed" part.
    inline bool usesActivityLog(RelayRole r)
    {
        return r == RelayRole::KeypadWrongCode || r == RelayRole::KeypadValidEntry || r == RelayRole::DoorSensorFault;
    }

    inline bool isKeypadRole(RelayRole r)
    {
        return r == RelayRole::KeypadWrongCode || r == RelayRole::KeypadValidEntry;
    }

    struct RelayDefault
    {
        RelayRole role;
        bool invert;
    };

    // Relay 1-8 when the master switch is on and the relay has no stored
    // role. Fault roles are inverted (closed = OK): a reboot, power loss or
    // cut wire reads as a fault.
    constexpr RelayDefault DEFAULTS[RELAYS] = {
        { RelayRole::WebhookPulse, false },     // 1: intercom door-open button
        { RelayRole::Secure, false },           // 2
        { RelayRole::DoorOpen, false },         // 3
        { RelayRole::Locked, false },           // 4
        { RelayRole::LockFault, true },         // 5
        { RelayRole::BatteryLow, true },        // 6
        { RelayRole::DoorSensorFault, true },   // 7
        { RelayRole::KeypadWrongCode, false },  // 8
    };

    // --- Nuki values (BLE API 2.3.0 / MQTT API 1.6; the same numbers) ------
    constexpr uint8_t LOCK_LOCKED = 0x01;
    constexpr uint8_t LOCK_UNLOCKED = 0x03;
    constexpr uint8_t LOCK_UNLATCHED = 0x05;
    constexpr uint8_t LOCK_UNLOCKED_LNGA = 0x06;
    constexpr uint8_t LOCK_MOTOR_BLOCKED = 0xFE;

    constexpr uint8_t DOOR_CLOSED = 0x02;
    constexpr uint8_t DOOR_OPENED = 0x03;
    constexpr uint8_t DOOR_UNKNOWN = 0x04;
    constexpr uint8_t DOOR_CALIBRATING = 0x05;
    constexpr uint8_t DOOR_UNCALIBRATED = 0x10;
    constexpr uint8_t DOOR_TAMPERED = 0xF0;

    constexpr uint8_t LOG_TYPE_KEYPAD = 0x05;      // LoggingType::KeypadAction
    constexpr uint8_t LOG_TYPE_DOOR_SENSOR = 0x06; // LoggingType::DoorSensor

    constexpr uint8_t COMPLETION_SUCCESS = 0x00;
    constexpr uint8_t COMPLETION_NOT_AUTHORIZED = 0x09;
    constexpr uint8_t COMPLETION_INVALID_CODE = 0xE0;

    // lastLockActionCompletionStatus values that mean the last action failed
    // mechanically (MotorBlocked, LowMotorVoltage, ClutchFailure,
    // MotorPowerFailure, IncompleteFailure, Failure).
    inline bool completionIsFault(uint8_t c)
    {
        return c == 0x01 || c == 0x05 || c == 0x06 || c == 0x07 || c == 0x08 || c == 0x0B;
    }

    // --- battery bits ------------------------------------------------------
    // Key turner state, decoded like NukiNetworkLock::publishKeyTurnerState.
    inline bool bleLockCritical(uint8_t criticalBatteryState)
    {
        return (criticalBatteryState & 1) == 1;
    }
    inline bool bleKeypadCritical(uint8_t accessoryBatteryState)
    {
        return accessoryBatteryState != 255 && (accessoryBatteryState & 3) == 3;
    }
    inline bool bleDoorSensorCritical(uint8_t accessoryBatteryState)
    {
        return accessoryBatteryState != 255 && (accessoryBatteryState & 12) == 12;
    }
    // Bit 0 of the accessory byte: the lock reports a keypad battery state,
    // i.e. it knows a keypad (hint only; Nuki Hub's hasKeypad() uses the config).
    inline bool bleKeypadBatteryReported(uint8_t accessoryBatteryState)
    {
        return accessoryBatteryState != 255 && (accessoryBatteryState & 1) == 1;
    }

    // --- inputs from the two sources -------------------------------------
    struct Field
    {
        bool have;
        uint8_t value;
        int64_t ms; // when it arrived (monotonic ms)
    };

    // What the lock published over MQTT during the current session.
    struct MqttInputs
    {
        Field lockState;
        Field doorState;
        Field lockCritical;   // value 0/1
        Field keypadCritical;
        Field doorCritical;
    };

    // The last successful BLE key turner state read.
    struct BleInputs
    {
        bool have;
        int64_t ms;
        uint8_t lockState;
        uint8_t doorState;
        uint8_t criticalBatteryState;
        uint8_t accessoryBatteryState;
        uint8_t nightModeActive;
        uint8_t completionStatus;
    };

    // Merged view the roles are evaluated on.
    struct Inputs
    {
        bool haveLock;
        uint8_t lockState;
        bool haveDoor;
        uint8_t doorState;
        bool lockCritical;
        bool keypadCritical;
        bool doorCritical;
        bool haveCompletion;
        uint8_t completionStatus;
        bool nightMode;
        bool stale;
        bool bleCommError;
        bool doorJammed;
        // Last definite lock position (locked / unlocked / unlatched /
        // unlocked lock 'n' go), held while the lock reports a transient or
        // fault state (locking, unlocking, unlatching, motor blocked, ...).
        bool havePosition;
        uint8_t position;
    };

    // States that say where the bolt is. Everything else (locking, unlocking,
    // unlatching, uncalibrated, motor blocked, undefined) doesn't.
    inline bool isDefiniteLockState(uint8_t s)
    {
        return s == LOCK_LOCKED || s == LOCK_UNLOCKED || s == LOCK_UNLATCHED || s == LOCK_UNLOCKED_LNGA;
    }

    // Keeps the last definite lock state across transient and fault states,
    // so a jam against an already locked bolt doesn't read as "not locked".
    // Lock fault still reports the jam from the live state.
    struct PositionHold
    {
        bool have;
        uint8_t state;

        void update(bool haveLock, uint8_t lockState)
        {
            if(haveLock && isDefiniteLockState(lockState))
            {
                have = true;
                state = lockState;
            }
        }
    };

    // The lock position the Locked / Unlocked / Secure roles use: the live
    // state if it is definite, else the held one; false if neither is known.
    inline bool lockPosition(const Inputs& in, uint8_t& out)
    {
        if(in.haveLock && isDefiniteLockState(in.lockState))
        {
            out = in.lockState;
            return true;
        }
        if(in.havePosition)
        {
            out = in.position;
            return true;
        }
        return false;
    }

    // The newer of the two: MQTT only while its session is live, BLE fills
    // in whatever MQTT hasn't sent. Ties go to MQTT.
    inline bool pick(const Field& mqtt, bool mqttLive, bool bleHave, uint8_t bleValue, int64_t bleMs, uint8_t& out)
    {
        if(mqttLive && mqtt.have && (!bleHave || mqtt.ms >= bleMs))
        {
            out = mqtt.value;
            return true;
        }
        if(bleHave)
        {
            out = bleValue;
            return true;
        }
        return false;
    }

    // Stale unless the lock's MQTT session is live and the lock was heard
    // from (any packet) within mqttFreshMs, or the last successful BLE key
    // turner state read is at most bleFreshMs old.
    inline bool isStale(bool mqttLive, int64_t lastRxAgeMs, int64_t mqttFreshMs, bool bleHave, int64_t bleAgeMs,
                        int64_t bleFreshMs)
    {
        const bool mqttFresh = mqttLive && lastRxAgeMs >= 0 && lastRxAgeMs <= mqttFreshMs;
        const bool bleFresh = bleHave && bleAgeMs >= 0 && bleAgeMs <= bleFreshMs;
        return !mqttFresh && !bleFresh;
    }

    // 2 x the lock state poll interval + 60 s.
    inline int64_t bleFreshMs(int32_t pollIntervalS)
    {
        return (int64_t)(pollIntervalS > 0 ? pollIntervalS : 1800) * 2000 + 60000;
    }

    inline Inputs merge(const MqttInputs& m, bool mqttLive, const BleInputs& b)
    {
        Inputs in = {};
        in.haveLock = pick(m.lockState, mqttLive, b.have, b.lockState, b.ms, in.lockState);
        in.haveDoor = pick(m.doorState, mqttLive, b.have, b.doorState, b.ms, in.doorState);
        uint8_t v = 0;
        in.lockCritical = pick(m.lockCritical, mqttLive, b.have, bleLockCritical(b.criticalBatteryState) ? 1 : 0, b.ms, v) && v;
        v = 0;
        in.keypadCritical = pick(m.keypadCritical, mqttLive, b.have, bleKeypadCritical(b.accessoryBatteryState) ? 1 : 0, b.ms, v) && v;
        v = 0;
        in.doorCritical = pick(m.doorCritical, mqttLive, b.have, bleDoorSensorCritical(b.accessoryBatteryState) ? 1 : 0, b.ms, v) && v;
        in.haveCompletion = b.have;
        in.completionStatus = b.completionStatus;
        in.nightMode = b.have && b.nightModeActive == 1;
        return in;
    }

    // Closed-when of a steady role, before Invert. Pulse roles rest open.
    inline bool steadyClosed(RelayRole r, const Inputs& in)
    {
        uint8_t pos = 0;
        const bool havePos = lockPosition(in, pos);
        const bool locked = havePos && pos == LOCK_LOCKED;
        switch(r)
        {
        case RelayRole::Secure:
            return locked && in.haveDoor && in.doorState == DOOR_CLOSED;
        case RelayRole::DoorOpen:
            return in.haveDoor && in.doorState == DOOR_OPENED;
        case RelayRole::Locked:
            return locked;
        case RelayRole::Unlocked:
            return havePos && (pos == LOCK_UNLOCKED || pos == LOCK_UNLOCKED_LNGA || pos == LOCK_UNLATCHED);
        case RelayRole::LockFault:
            return (in.haveLock && in.lockState == LOCK_MOTOR_BLOCKED) ||
                   (in.haveCompletion && completionIsFault(in.completionStatus)) || in.bleCommError || in.stale;
        case RelayRole::BatteryLow:
            return in.lockCritical || in.keypadCritical || in.doorCritical;
        case RelayRole::DoorSensorFault:
            if(in.haveDoor && in.doorState == DOOR_CALIBRATING)
            {
                return false;
            }
            return in.doorJammed || (in.haveDoor && (in.doorState == DOOR_UNKNOWN || in.doorState == DOOR_UNCALIBRATED ||
                                                     in.doorState == DOOR_TAMPERED));
        case RelayRole::NightMode:
            return in.nightMode;
        default:
            return false;
        }
    }

    // The level a relay should rest at / follow (steady roles). Not armed
    // (nothing known since boot): open, inverted roles too. Off and
    // WebhookPulse: open. Pulse roles rest at their inverse pulse level.
    inline bool relayLevel(RelayRole r, bool invert, const Inputs& in, bool armed)
    {
        if(!armed || r == RelayRole::Off || r == RelayRole::WebhookPulse)
        {
            return false;
        }
        if(isPulseRole(r))
        {
            return invert;
        }
        return steadyClosed(r, in) != invert;
    }

    struct RelayConfig
    {
        RelayRole role;
        bool invert;
        uint32_t pulseMs;
    };

    // Bit N-1 = relay N closed. Master off: all open (only webhook pulses move them).
    inline uint8_t targetLevels(const RelayConfig* cfg, size_t n, bool enabled, const Inputs& in, bool armed)
    {
        uint8_t out = 0;
        for(size_t i = 0; enabled && i < n && i < RELAYS; i++)
        {
            if(relayLevel(cfg[i].role, cfg[i].invert, in, armed))
            {
                out |= (uint8_t)(1u << i);
            }
        }
        return out;
    }

    // --- MQTT lockActionEvent ------------------------------------------------
    // "LockAction,Trigger,Auth-ID,Code-ID,context" (Nuki MQTT API): context is
    // auto-unlock (0/1), the number of button/fob presses, or for the keypad
    // 0 = back key, 1 = code, 2 = fingerprint. Example keypad unlatch:
    // "3,0,54321,12345,1".
    struct LockActionEvent
    {
        uint32_t action;
        uint32_t trigger;
        uint32_t authId;
        uint32_t codeId;
        uint32_t context;
    };

    inline bool parseLockActionEvent(const char* s, LockActionEvent& out)
    {
        if(s == nullptr)
        {
            return false;
        }
        uint32_t v[5] = {};
        const char* p = s;
        for(int i = 0; i < 5; i++)
        {
            if(*p < '0' || *p > '9')
            {
                return false;
            }
            char* end = nullptr;
            const unsigned long x = strtoul(p, &end, 10);
            if(end == p || x > 0xffffffffUL)
            {
                return false;
            }
            v[i] = (uint32_t)x;
            p = end;
            if(i < 4)
            {
                if(*p != ',')
                {
                    return false;
                }
                p++;
            }
        }
        // Tolerate a trailing newline/space (and anything the lock may append
        // after a comma in future firmware).
        if(*p != 0 && *p != ',' && *p != '\n' && *p != '\r' && *p != ' ')
        {
            return false;
        }
        out = { v[0], v[1], v[2], v[3], v[4] };
        return true;
    }

    // A keypad code or fingerprint entry (a keypad Code-ID). A wrong code is
    // not published (MQTT API); a 4th/5th gen firmware bug publishes one as
    // "1,0,<auth>,0,1", which Code-ID 0 excludes.
    inline bool isKeypadEntry(const LockActionEvent& e)
    {
        return e.codeId > 0 && (e.context == 1 || e.context == 2);
    }

    // --- activity log ---------------------------------------------------------
    struct LogView
    {
        uint32_t index;
        uint8_t type;    // LoggingType
        uint8_t data[5]; // LogEntry.data
    };

    enum class LogEvent : uint8_t { None, KeypadWrong, KeypadValid, DoorJammed, DoorOpenedClosed };

    // Keypad: data[1] source (0 arrow key, 1 code, 2 fingerprint, others e.g.
    // NFC on newer keypads: counted the same), data[2] completion, data[3..4]
    // code ID. Door sensor: data[0] 0 opened, 1 closed, 2 jammed.
    inline LogEvent classify(const LogView& e, uint16_t& codeId)
    {
        codeId = 0;
        if(e.type == LOG_TYPE_KEYPAD)
        {
            codeId = (uint16_t)(e.data[3] | (e.data[4] << 8));
            if(e.data[2] == COMPLETION_SUCCESS)
            {
                return LogEvent::KeypadValid;
            }
            if(e.data[2] == COMPLETION_INVALID_CODE || e.data[2] == COMPLETION_NOT_AUTHORIZED)
            {
                return LogEvent::KeypadWrong;
            }
            return LogEvent::None;
        }
        if(e.type == LOG_TYPE_DOOR_SENSOR)
        {
            if(e.data[0] == 2)
            {
                return LogEvent::DoorJammed;
            }
            if(e.data[0] == 0 || e.data[0] == 1)
            {
                return LogEvent::DoorOpenedClosed;
            }
        }
        return LogEvent::None;
    }

    // Highest log index seen. The first read without a baseline only records
    // it (old entries never fire).
    struct LogTracker
    {
        bool haveBaseline;
        uint32_t lastIndex;
    };

    // Calls onEvent(LogEvent, codeId) for each new entry, oldest first
    // (entries may come in any order). Returns the number of new entries.
    template<typename F> size_t processLog(LogTracker& t, const LogView* entries, size_t n, F onEvent)
    {
        uint32_t maxIndex = t.lastIndex;
        for(size_t i = 0; i < n; i++)
        {
            maxIndex = entries[i].index > maxIndex ? entries[i].index : maxIndex;
        }
        if(!t.haveBaseline)
        {
            t.haveBaseline = n > 0;
            t.lastIndex = maxIndex;
            return 0;
        }
        size_t fired = 0;
        uint32_t from = t.lastIndex;
        // Oldest first without sorting (n is small: the authlog max entries).
        for(;;)
        {
            const LogView* next = nullptr;
            for(size_t i = 0; i < n; i++)
            {
                if(entries[i].index > from && (next == nullptr || entries[i].index < next->index))
                {
                    next = &entries[i];
                }
            }
            if(next == nullptr)
            {
                break;
            }
            uint16_t codeId = 0;
            const LogEvent ev = classify(*next, codeId);
            if(ev != LogEvent::None)
            {
                onEvent(ev, codeId);
            }
            fired++;
            from = next->index;
        }
        t.lastIndex = maxIndex;
        return fired;
    }

    // Door sensor "jammed" (log) is latched until the door is next reported
    // opened or closed: a newer door log entry, or a door state change.
    struct JamLatch
    {
        bool latched;

        void onLog(LogEvent e)
        {
            if(e == LogEvent::DoorJammed)
            {
                latched = true;
            }
            else if(e == LogEvent::DoorOpenedClosed)
            {
                latched = false;
            }
        }
        void onDoorStateChange(uint8_t state)
        {
            if(state == DOOR_OPENED || state == DOOR_CLOSED)
            {
                latched = false;
            }
        }
    };

    // A valid keypad entry reported over MQTT is seen again later in the
    // activity log: the log entry with the same code ID within windowMs is
    // consumed instead of pulsing a second time.
    struct KeypadDedupe
    {
        static constexpr size_t SLOTS = 4;
        static constexpr int64_t WINDOW_MS = 30 * 60 * 1000;
        struct Slot
        {
            bool used;
            uint16_t codeId;
            int64_t ms;
        } slots[SLOTS];

        void noteMqtt(uint16_t codeId, int64_t nowMs)
        {
            size_t pick = 0;
            for(size_t i = 0; i < SLOTS; i++)
            {
                if(!slots[i].used)
                {
                    pick = i;
                    break;
                }
                if(slots[i].ms < slots[pick].ms)
                {
                    pick = i;
                }
            }
            slots[pick] = { true, codeId, nowMs };
        }

        // True: this log entry was already reported over MQTT (don't pulse).
        bool consumeLog(uint16_t codeId, int64_t nowMs)
        {
            size_t best = SLOTS;
            for(size_t i = 0; i < SLOTS; i++)
            {
                if(slots[i].used && nowMs - slots[i].ms > WINDOW_MS)
                {
                    slots[i].used = false;
                }
                if(slots[i].used && slots[i].codeId == codeId && (best == SLOTS || slots[i].ms < slots[best].ms))
                {
                    best = i;
                }
            }
            if(best == SLOTS)
            {
                return false;
            }
            slots[best].used = false;
            return true;
        }
    };
}
