#pragma once

// Embedded MQTT server for the Nuki lock (NUKI_HUB_EMBEDDED_LOCK_MQTT).
//
// The lock (Nuki MQTT API, set up in the Nuki app: broker = this board's IP,
// port 1883, user name + password) connects to this board instead of an
// external broker. Exactly one client: the lock. Nuki Hub is not a network
// client; it talks to the session in-process:
//  - what the lock publishes under nuki/<ID>/ goes straight to
//    NukiNetworkLock::onMqttDataReceived, i.e. into upstream's hybrid mode
//    (NukiOfficial), as if it came from a broker;
//  - NukiNetworkLock::publishOffAction (hybrid "send actions through official
//    MQTT") becomes a PUBLISH of nuki/<ID>/lockAction to the lock.
// Nothing on the network can publish lockAction: nobody else can subscribe
// or publish, and the lock only gets what Nuki Hub sends.
//
// Settings (NVS "forkcfg", page "Protect Webhook & Relays"): on/off, user
// name, password, optional expected client ID. A CONNECT with the right
// credentials replaces the running session (MQTT client takeover), so a
// half-open old connection can't keep the lock out. Unauthenticated sockets
// get 5 s to send a valid CONNECT; at most two wait at a time.
//
// One task ("lockmqtt", core 1) owns the sockets; other tasks only publish
// (under the same recursive mutex).

#ifdef NUKI_HUB_EMBEDDED_LOCK_MQTT

#include <cstdint>
#include "LockMqttLogic.h"

class MqttReceiver;
class Preferences;

namespace LockMqttServer
{
    constexpr uint16_t PORT = 1883;

    // setup(), after ForkSettings::begin() and before the lock objects exist:
    // if the server is enabled, switch on Nuki Hub's hybrid mode, "send
    // actions through official MQTT" and "retry over BLE if failed".
    void syncHybridPreferences(Preferences* preferences);

    // setup(), once the lock's NukiNetworkLock exists. Starts the task.
    void begin(MqttReceiver* receiver, Preferences* preferences);

    // Hybrid routing: a live, authenticated lock session that subscribes to
    // .../lockAction ("Allow locking" on in the Nuki app).
    bool canSendLockAction();

    // Any task. False if nothing was sent (no session, not subscribed, socket error).
    bool publishLockAction(int action);

    // For the MQTT -> BLE fallback: the lock acknowledged (PUBREC/PUBACK) the
    // most recent lockAction we sent, so it has the command.
    bool lastLockActionReceived();

    // Nuki Hub sent a lock action over BLE (nuki task): a later redundant-
    // action skip needs a state from the lock newer than this.
    void noteBleCommand();

    // For the Protect webhook's redundant-action skip.
    bool liveState(LockMqttLogic::LiveState& out, int64_t& stateAgeMs);

    struct Status
    {
        bool enabled;
        bool listening;
        bool connected;
        bool lockActionSubscribed;
        bool hybridReady;          // Nuki Hub's hybrid mode was on at boot
        int64_t connectedSinceMs;  // uptime ms
        int64_t lastRxAgeMs;       // -1 = never
        int16_t lockState;         // -1 = none this session
        int64_t stateAgeMs;
        int16_t doorState;         // -1 = none this session
        uint16_t keepAliveS;
        char clientId[LockMqttLogic::LEN_CLIENT_ID + 1];
        char peer[16];
        char lastRefusal[80];
        uint32_t sessions;
        uint32_t refused;
        uint32_t takeovers;
        uint32_t stackFreeBytes;
    };
    void status(Status& out);
}

#endif
