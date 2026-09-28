#ifdef NUKI_HUB_EMBEDDED_LOCK_MQTT

#ifndef NUKI_HUB_PROTECT_WEBHOOK
#error "NUKI_HUB_EMBEDDED_LOCK_MQTT keeps its settings on the Protect Webhook & Relays page (NUKI_HUB_PROTECT_WEBHOOK)"
#endif

#include <Arduino.h>
#include <Preferences.h>
#include "LockMqttServer.h"
#include "ForkSettings.h"
#include "ProtectTiming.h"
#include "MqttReceiver.h"
#include "MqttTopics.h"
#include "PreferencesKeys.h"
#include "Logger.h"
#include "esp_timer.h"
#include "EspMillis.h"
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include <cerrno>
#include <cstdlib>

#ifdef NUKI_HUB_WAVESHARE_8DI8RO
#include "WaveshareBoard.h"
#endif

// QoS for lockAction. Nuki: "For all messages QOS = 2 is used by the device
// and should be used by the publisher too" (MQTT API 1.6, 4.3). Build with
// -DLOCK_MQTT_ACTION_QOS=1 to compare: QoS 1 saves the PUBREL round trip if
// the lock acts only after PUBREL.
#ifndef LOCK_MQTT_ACTION_QOS
#define LOCK_MQTT_ACTION_QOS 2
#endif
static_assert(LOCK_MQTT_ACTION_QOS >= 0 && LOCK_MQTT_ACTION_QOS <= 2, "LOCK_MQTT_ACTION_QOS must be 0..2");

using namespace LockMqttLogic;

namespace
{
    constexpr int kConns = 3;              // the lock + up to two connections still waiting for CONNECT
    // -fstack-usage: the receive path into NukiOfficial needs ~2 KB, plus
    // newlib printf for the log (~1-1.5 KB). The settings page shows the unused rest.
    constexpr uint32_t kTaskStack = 5120;
    constexpr int kSelectMs = 200;

    struct Config
    {
        bool enabled = false;
        char user[LEN_USER + 1] = {0};
        char pass[LEN_PASS + 1] = {0};
        char clientId[LEN_CLIENT_ID + 1] = {0};
        bool skipRedundant = true;
        uint32_t silenceMs = 330000;
    };

    struct Conn;

    // Session callbacks for one connection.
    struct ConnEvents : SessionEvents
    {
        int index = 0;
        void send(const uint8_t* data, size_t len) override;
        void onConnect() override;
        void onPublish(const char* topic, const uint8_t* payload, size_t len) override;
        void onLockActionSubscription(int qos) override;
        void onLockActionAck(PacketType type) override;
    };

    struct Conn
    {
        int fd = -1;
        bool broken = false;      // a send failed: close at the next loop
        Session session;
        ConnEvents events;
        char peer[16] = {0};
    };

    // Everything below is guarded by mutex (recursive: the session callbacks
    // run inside feed() and may publish).
    SemaphoreHandle_t mutex = nullptr;
    TaskHandle_t task = nullptr;
    MqttReceiver* receiver = nullptr;
    Preferences* prefs = nullptr;
    Config cfg;
    uint32_t cfgGeneration = 0;
    bool hybridAtBoot = false;

    Conn conns[kConns];
    int listenFd = -1;
    int active = -1;                  // index of the lock's session
    char path[24] = {0};              // "nuki/<ID>"
    char lockActionTopic[40] = {0};

    // Lock state reported over MQTT during the current session.
    bool haveState = false;
    uint8_t lockState = 0;
    int64_t stateTs = 0;
    bool haveDoor = false;
    uint8_t doorState = 0;
    int64_t lastCommandTs = 0;        // last lock action Nuki Hub sent (MQTT or BLE)
    bool lastActionReceived = false;

    int64_t connectedSinceMs = 0;
    char clientId[LEN_CLIENT_ID + 1] = {0};
    char activePeer[16] = {0};
    char lastRefusal[80] = {0};
    uint32_t sessions = 0;
    uint32_t refused = 0;
    uint32_t takeovers = 0;

    struct Guard
    {
        Guard() { xSemaphoreTakeRecursive(mutex, portMAX_DELAY); }
        ~Guard() { xSemaphoreGiveRecursive(mutex); }
    };

    Credentials credentials()
    {
        return { cfg.user, cfg.pass, cfg.clientId };
    }

    void updatePath()
    {
        const uint32_t id = prefs != nullptr ? prefs->getUInt(preference_nuki_id_lock, 0) : 0;
        officialPath(id, path, sizeof(path));
        snprintf(lockActionTopic, sizeof(lockActionTopic), "%s%s", path, mqtt_topic_official_lock_action);
    }

    // As if the lock's will ("connected" = "false", Nuki MQTT API 4.2) had
    // been published: NukiOfficial goes offline and actions use BLE.
    void deliverOffline()
    {
        if(receiver == nullptr || path[0] == 0)
        {
            return;
        }
        char topic[48];
        snprintf(topic, sizeof(topic), "%s%s", path, mqtt_topic_official_connected);
        char value[] = "false";
        receiver->onMqttDataReceived(topic, (byte*)value, 5);
    }

    void resetLockState()
    {
        haveState = false;
        haveDoor = false;
        lastActionReceived = false;
    }

    void closeConn(int i, const char* why)
    {
        Conn& c = conns[i];
        if(c.fd < 0)
        {
            return;
        }
        const Session::State state = c.session.state();
        const CloseReason reason = c.session.closeReason();
        ::close(c.fd);
        c.fd = -1;
        c.broken = false;
        c.session.reset();

        if(i == active)
        {
            active = -1;
            Log->printf("Lock MQTT: lock disconnected (%s), actions use BLE\n", why);
            resetLockState();
            deliverOffline();
        }
        else if(reason == CloseReason::Refused)
        {
            refused++;
            snprintf(lastRefusal, sizeof(lastRefusal), "%s from %s: %s", c.session.info().clientId, c.peer,
                     connectResultText(c.session.refusal()));
            Log->printf("Lock MQTT: refused client '%s' from %s: %s\n", c.session.info().clientId, c.peer,
                        connectResultText(c.session.refusal()));
        }
        else if(state != Session::State::Idle)
        {
            if(reason == CloseReason::Malformed || reason == CloseReason::ConnectTimeout || reason == CloseReason::ProtocolError)
            {
                refused++;
                snprintf(lastRefusal, sizeof(lastRefusal), "%s: %s", c.peer, why);
            }
            Log->printf("Lock MQTT: connection from %s closed before CONNECT (%s)\n", c.peer, why);
        }
    }

    void closeAll()
    {
        for(int i = 0; i < kConns; i++)
        {
            closeConn(i, "server switched off");
        }
        if(listenFd >= 0)
        {
            ::close(listenFd);
            listenFd = -1;
            Log->println("Lock MQTT: stopped");
        }
    }

    void ConnEvents::send(const uint8_t* data, size_t len)
    {
        Conn& c = conns[index];
        size_t done = 0;
        int spins = 0;
        while(c.fd >= 0 && !c.broken && done < len)
        {
            const int n = ::send(c.fd, data + done, len - done, 0);
            if(n > 0)
            {
                done += (size_t)n;
                continue;
            }
            if(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && spins++ < 20)
            {
                vTaskDelay(pdMS_TO_TICKS(5)); // a few bytes into an empty socket: practically never
                continue;
            }
            c.broken = true;
        }
    }

    void ConnEvents::onConnect()
    {
        Conn& c = conns[index];
        if(active >= 0 && active != index)
        {
            // MQTT 3.1.1 3.1.4-2 style takeover: the newest authenticated
            // connection wins; the old one may be half-open (lock rebooted,
            // Wi-Fi roamed) and must not keep the lock out.
            takeovers++;
            Log->printf("Lock MQTT: takeover, closing the previous session from %s\n", conns[active].peer);
            Conn& old = conns[active];
            ::close(old.fd);
            old.fd = -1;
            old.broken = false;
            old.session.reset();
        }
        active = index;
        sessions++;
        resetLockState();
        connectedSinceMs = espMillis();
        strlcpy(clientId, c.session.info().clientId, sizeof(clientId));
        strlcpy(activePeer, c.peer, sizeof(activePeer));
        Log->printf("Lock MQTT: lock connected (client '%s', keepalive %u s, from %s)\n", clientId,
                    (unsigned)c.session.info().keepAliveS, c.peer);
    }

    void ConnEvents::onPublish(const char* topic, const uint8_t* payload, size_t len)
    {
        if(index != active)
        {
            return;
        }
        const size_t pathLen = strlen(path);
        if(pathLen == 0 || strncmp(topic, path, pathLen) != 0 || topic[pathLen] != '/')
        {
            return; // not nuki/<paired lock ID>/... (e.g. homeassistant/...): nobody wants it
        }
        const char* sub = topic + pathLen;
        char value[96];
        const size_t n = len < sizeof(value) - 1 ? len : sizeof(value) - 1;
        memcpy(value, payload, n);
        value[n] = 0;

        const int64_t now = espMillis();
        if(strcmp(sub, mqtt_topic_official_state) == 0)
        {
            haveState = true;
            lockState = (uint8_t)atoi(value);
            stateTs = now;
        }
        else if(strcmp(sub, mqtt_topic_official_doorsensorState) == 0)
        {
            haveDoor = true;
            doorState = (uint8_t)atoi(value);
        }
        else if(strcmp(sub, mqtt_topic_official_lockActionEvent) == 0)
        {
            ProtectTiming::onMqttLockActionEvent((uint8_t)atoi(value));
        }
        else if(strcmp(sub, mqtt_topic_official_commandResponse) == 0)
        {
            ProtectTiming::onMqttCommandResponse(atoi(value));
        }

        if(receiver != nullptr)
        {
            // Into upstream's hybrid mode, exactly like a message from a broker.
            receiver->onMqttDataReceived(topic, (byte*)value, (unsigned int)n);
        }
    }

    void ConnEvents::onLockActionSubscription(int qos)
    {
        if(index != active)
        {
            return;
        }
        if(qos >= 0)
        {
            Log->printf("Lock MQTT: lock subscribed to %s (QoS %d): lock actions go over MQTT\n", lockActionTopic, qos);
        }
        else
        {
            Log->println("Lock MQTT: lock unsubscribed from lockAction: lock actions use BLE");
        }
    }

    void ConnEvents::onLockActionAck(PacketType type)
    {
        if(index != active)
        {
            return;
        }
        lastActionReceived = true;
        ProtectTiming::onMqttAck(type == PUBCOMP);
    }

    bool openListener()
    {
        const int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if(fd < 0)
        {
            Log->printf("Lock MQTT: socket() failed (%d)\n", errno);
            return false;
        }
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(LockMqttServer::PORT);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if(::bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0 || ::listen(fd, 2) != 0)
        {
            Log->printf("Lock MQTT: can't listen on port %u (%d)\n", (unsigned)LockMqttServer::PORT, errno);
            ::close(fd);
            return false;
        }
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        listenFd = fd;
        Log->printf("Lock MQTT: listening on port %u for the Nuki lock\n", (unsigned)LockMqttServer::PORT);
        return true;
    }

    void acceptOne(int64_t now)
    {
        struct sockaddr_in peer = {};
        socklen_t peerLen = sizeof(peer);
        const int fd = ::accept(listenFd, (struct sockaddr*)&peer, &peerLen);
        if(fd < 0)
        {
            return;
        }
        int slot = -1;
        for(int i = 0; i < kConns; i++)
        {
            if(conns[i].fd < 0)
            {
                slot = i;
                break;
            }
        }
        if(slot < 0)
        {
            // Full: drop the oldest connection that hasn't authenticated.
            // The lock's own session is never dropped for a newcomer.
            int64_t oldest = INT64_MAX;
            for(int i = 0; i < kConns; i++)
            {
                if(i != active && conns[i].session.state() == Session::State::AwaitConnect &&
                   conns[i].session.lastRxMs() < oldest)
                {
                    oldest = conns[i].session.lastRxMs();
                    slot = i;
                }
            }
            if(slot < 0)
            {
                ::close(fd);
                return;
            }
            closeConn(slot, "replaced by a newer connection");
        }
        Conn& c = conns[slot];
        c.fd = fd;
        c.broken = false;
        inet_ntoa_r(peer.sin_addr, c.peer, sizeof(c.peer));
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        // Notice a lock that vanished (Wi-Fi gone) in ~90 s instead of
        // 1.5 x its keepalive (450 s for the Nuki lock's 300 s).
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
#if defined(TCP_KEEPIDLE) && defined(TCP_KEEPINTVL) && defined(TCP_KEEPCNT)
        int idle = 60, intvl = 10, cnt = 3;
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
#endif
        updatePath();
        c.session.begin(now, lockActionTopic, LOCK_MQTT_ACTION_QOS);
    }

    void readConn(int i, int64_t now)
    {
        Conn& c = conns[i];
        uint8_t buf[256];
        const int n = ::recv(c.fd, buf, sizeof(buf), 0);
        if(n == 0)
        {
            closeConn(i, "connection closed");
            return;
        }
        if(n < 0)
        {
            if(errno != EAGAIN && errno != EWOULDBLOCK)
            {
                closeConn(i, errno == ETIMEDOUT ? "TCP keepalive timeout" : "socket error");
            }
            return;
        }
        const Credentials cred = credentials();
        if(!c.session.feed(buf, (size_t)n, now, cred, c.events) && c.fd >= 0)
        {
            closeConn(i, closeReasonText(c.session.closeReason()));
        }
    }

    // Hybrid mode, actions over official MQTT, and the BLE retry if the lock
    // doesn't confirm an MQTT action within 2 s. Written only when different;
    // left alone while the server is off.
    void applyHybridPreferences(bool enabled)
    {
        if(prefs == nullptr || !enabled)
        {
            return;
        }
        const char* keys[] = { preference_official_hybrid_enabled, preference_official_hybrid_actions, preference_official_hybrid_retry };
        for(const char* k : keys)
        {
            if(!prefs->getBool(k, false))
            {
                prefs->putBool(k, true);
                Log->printf("Lock MQTT: switched on Nuki Hub setting '%s'\n", k);
            }
        }
    }

    void refreshConfig()
    {
        const uint32_t gen = ForkSettings::generation();
        if(gen == cfgGeneration)
        {
            return;
        }
        Config next;
        {
            ForkSettings::ReadLock lock(pdMS_TO_TICKS(200));
            if(!lock.locked())
            {
                return; // busy: next loop
            }
            const ForkSettings::Settings& s = lock.settings();
            next.enabled = s.lockMqttEnabled;
            strlcpy(next.user, s.lockMqttUser, sizeof(next.user));
            strlcpy(next.pass, s.lockMqttPass, sizeof(next.pass));
            strlcpy(next.clientId, s.lockMqttClientId, sizeof(next.clientId));
            next.skipRedundant = s.skipRedundant;
            next.silenceMs = s.lockSilenceMs;
        }
        {
            Guard g;
            const bool credsChanged = strcmp(next.user, cfg.user) != 0 || strcmp(next.pass, cfg.pass) != 0 ||
                                      strcmp(next.clientId, cfg.clientId) != 0;
            cfg = next;
            cfgGeneration = gen;
            if(!cfg.enabled)
            {
                closeAll();
            }
            else if(credsChanged && active >= 0)
            {
                closeConn(active, "credentials changed, the lock must log in again");
            }
        }
        // Not under our mutex: the webhook takes the settings lock first, then ours.
        applyHybridPreferences(next.enabled);
    }

    void serverTask(void*)
    {
        for(;;)
        {
            refreshConfig();
            if(!cfg.enabled)
            {
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
            if(listenFd < 0)
            {
                Guard g;
                if(!openListener())
                {
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    continue;
                }
            }

            fd_set rfds;
            FD_ZERO(&rfds);
            int maxFd = listenFd;
            FD_SET(listenFd, &rfds);
            {
                Guard g;
                for(Conn& c : conns)
                {
                    if(c.fd >= 0)
                    {
                        FD_SET(c.fd, &rfds);
                        maxFd = c.fd > maxFd ? c.fd : maxFd;
                    }
                }
            }
            struct timeval tv = { 0, kSelectMs * 1000 };
            const int r = ::select(maxFd + 1, &rfds, nullptr, nullptr, &tv);

            Guard g;
            const int64_t now = espMillis();
            if(r > 0)
            {
                for(int i = 0; i < kConns; i++)
                {
                    if(conns[i].fd >= 0 && FD_ISSET(conns[i].fd, &rfds))
                    {
                        readConn(i, now);
                    }
                }
                if(listenFd >= 0 && FD_ISSET(listenFd, &rfds))
                {
                    acceptOne(now);
                }
            }
            for(int i = 0; i < kConns; i++)
            {
                Conn& c = conns[i];
                if(c.fd < 0)
                {
                    continue;
                }
                if(c.broken)
                {
                    closeConn(i, "send failed");
                }
                else if(!c.session.poll(now))
                {
                    closeConn(i, closeReasonText(c.session.closeReason()));
                }
            }
        }
    }
}

void LockMqttServer::syncHybridPreferences(Preferences* preferences)
{
    if(preferences == nullptr)
    {
        return;
    }
    prefs = preferences;
    bool enabled = false;
    {
        ForkSettings::ReadLock lock(pdMS_TO_TICKS(1000));
        enabled = lock.locked() && lock.settings().lockMqttEnabled;
    }
    // At boot: what NukiOfficial will read in its constructor.
    hybridAtBoot = preferences->getBool(preference_official_hybrid_enabled, false) || enabled;
    applyHybridPreferences(enabled);
}

void LockMqttServer::begin(MqttReceiver* r, Preferences* preferences)
{
    if(task != nullptr)
    {
        return;
    }
    mutex = xSemaphoreCreateRecursiveMutex();
    if(mutex == nullptr)
    {
        Log->println("Lock MQTT: out of memory");
        return;
    }
    receiver = r;
    prefs = preferences;
    for(int i = 0; i < kConns; i++)
    {
        conns[i].events.index = i;
    }
    updatePath();
#ifdef NUKI_HUB_WAVESHARE_8DI8RO
    const BaseType_t core = WaveshareBoard::NETWORK_CORE;
#else
    const BaseType_t core = 1;
#endif
    if(xTaskCreatePinnedToCore(serverTask, "lockmqtt", kTaskStack, nullptr, 3, &task, core) != pdPASS)
    {
        task = nullptr;
        Log->println("Lock MQTT: task not started (out of memory)");
    }
}

bool LockMqttServer::canSendLockAction()
{
    if(mutex == nullptr)
    {
        return false;
    }
    Guard g;
    return active >= 0 && conns[active].session.lockActionQos() >= 0;
}

bool LockMqttServer::publishLockAction(int action)
{
    if(mutex == nullptr)
    {
        return false;
    }
    bool ok = false;
    uint16_t id = 0;
    uint8_t qos = 0;
    {
        Guard g;
        if(active >= 0)
        {
            Conn& c = conns[active];
            char payload[8];
            snprintf(payload, sizeof(payload), "%d", action);
            ok = c.session.publishLockAction(payload, c.events, &id) && !c.broken;
            qos = c.session.lastActionQos();
            if(c.broken)
            {
                // Wake the server task (select sees EOF) so it closes the session.
                ::shutdown(c.fd, SHUT_RDWR);
            }
            if(ok)
            {
                lastCommandTs = espMillis();
                lastActionReceived = false;
                // Still under the mutex: the server task can't handle the
                // lock's PUBREC before the press is marked as sent over MQTT.
                ProtectTiming::onMqttSent((uint8_t)action);
            }
        }
    }
    if(ok)
    {
        Log->printf("Lock MQTT: lockAction %d sent (QoS %u, id %u)\n", action, (unsigned)qos, (unsigned)id);
    }
    else
    {
        Log->printf("Lock MQTT: lockAction %d not sent (no lock session)\n", action);
    }
    return ok;
}

bool LockMqttServer::lastLockActionReceived()
{
    if(mutex == nullptr)
    {
        return false;
    }
    Guard g;
    return lastActionReceived;
}

void LockMqttServer::noteBleCommand()
{
    if(mutex == nullptr)
    {
        return;
    }
    Guard g;
    lastCommandTs = espMillis();
}

bool LockMqttServer::liveState(LiveState& out, int64_t& stateAgeMs)
{
    out = {};
    stateAgeMs = -1;
    if(mutex == nullptr)
    {
        return false;
    }
    Guard g;
    const int64_t now = espMillis();
    out.sessionLive = active >= 0 && conns[active].session.state() == Session::State::Live;
    out.haveState = out.sessionLive && haveState;
    out.stateAfterCommand = out.haveState && stateTs > lastCommandTs;
    out.lockState = lockState;
    out.haveDoor = out.sessionLive && haveDoor;
    out.doorState = doorState;
    out.lastRxAgeMs = out.sessionLive ? now - conns[active].session.lastRxMs() : -1;
    stateAgeMs = out.haveState ? now - stateTs : -1;
    return out.sessionLive;
}

void LockMqttServer::status(Status& out)
{
    memset(&out, 0, sizeof(out));
    out.lockState = -1;
    out.doorState = -1;
    out.lastRxAgeMs = -1;
    out.stateAgeMs = -1;
    out.hybridReady = hybridAtBoot;
    if(mutex == nullptr)
    {
        return;
    }
    Guard g;
    const int64_t now = espMillis();
    out.enabled = cfg.enabled;
    out.listening = listenFd >= 0;
    out.connected = active >= 0;
    out.sessions = sessions;
    out.refused = refused;
    out.takeovers = takeovers;
    strlcpy(out.lastRefusal, lastRefusal, sizeof(out.lastRefusal));
    if(task != nullptr)
    {
        out.stackFreeBytes = (uint32_t)uxTaskGetStackHighWaterMark(task);
    }
    if(active >= 0)
    {
        const Session& s = conns[active].session;
        out.lockActionSubscribed = s.lockActionQos() >= 0;
        out.connectedSinceMs = connectedSinceMs;
        out.lastRxAgeMs = now - s.lastRxMs();
        out.keepAliveS = s.info().keepAliveS;
        strlcpy(out.clientId, clientId, sizeof(out.clientId));
        strlcpy(out.peer, activePeer, sizeof(out.peer));
        if(haveState)
        {
            out.lockState = lockState;
            out.stateAgeMs = now - stateTs;
        }
        if(haveDoor)
        {
            out.doorState = doorState;
        }
    }
}

#endif
