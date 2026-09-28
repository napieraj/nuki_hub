#pragma once

// Press-to-lock latency of the Protect webhook: "Protect timing: ..." log
// lines for each webhook lock action, saying which transport carried it
// (BLE, or MQTT through the embedded lock MQTT server) and how many ms after
// the webhook the lock confirmed it. Always on with NUKI_HUB_PROTECT_WEBHOOK.
//
// The BLE lines come from protect-ws-prewarm (c31e4c20), without its event
// stream warm-up.
//
// One press is tracked at a time (the webhook refuses a second lock action
// while one is pending). Actions from other sources (GPIO, web UI) are not
// logged.

#ifdef NUKI_HUB_PROTECT_WEBHOOK

#include <cstdint>

namespace ProtectTiming
{
    // httpd task, before the action is handed to NukiWrapper (lockAction is
    // the NukiLock::LockAction value). cancel() if it wasn't queued.
    void onWebhookAction(uint8_t lockAction, const char* name);
    void cancel();

    // Nuki task, around sending a queued lock action over BLE (all retries).
    void onBleStart(uint8_t lockAction);
    void onBleEnd(uint8_t lockAction, bool success);

    // Lock MQTT: lockAction published to the lock (any task), and what came back.
    void onMqttSent(uint8_t lockAction);
    void onMqttAck(bool complete);                   // PUBREC/PUBACK, then PUBCOMP
    void onMqttLockActionEvent(uint8_t lockAction);  // lock: "about to execute"
    void onMqttCommandResponse(int code);            // lock: result of the last command
    void onMqttFallbackToBle(uint8_t lockAction);    // no confirmation in time
}

#endif
