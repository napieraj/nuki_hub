#pragma once

// Embedded MQTT server for the Nuki lock (NUKI_HUB_EMBEDDED_LOCK_MQTT), pure
// part: MQTT 3.1.1 packet parsing/encoding, the per-connection session state
// machine, topic matching and the redundant-action rule. No sockets, Arduino
// or FreeRTOS, so it is host-tested (pio test -e native, test_lock_mqtt).
// LockMqttServer.cpp owns the sockets and wires this into Nuki Hub.
//
// Only what the Nuki lock uses (Nuki MQTT API 1.6, developer.nuki.io):
//  - CONNECT/CONNACK (MQTT 3.1.1, protocol level 4; level 3 "MQIsdp" too),
//    user name + password required, optional expected client ID
//  - PUBLISH in both directions, QoS 0/1/2: the lock publishes its state
//    topics with QoS 0 + retain, commandResponse/lockActionEvent with QoS 2
//    and subscribes to lockAction with QoS 2
//  - PUBACK, PUBREC, PUBREL, PUBCOMP
//  - SUBSCRIBE/SUBACK, UNSUBSCRIBE/UNSUBACK (only to learn whether the lock
//    listens to .../lockAction; nothing else is ever sent to it)
//  - PINGREQ/PINGRESP, DISCONNECT, keepalive timeout (1.5 x keepalive)
// No retained store, no will delivery, no other subscribers, no persistence
// (clean sessions only: a session never outlives its TCP connection).

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <strings.h>

namespace LockMqttLogic
{
    enum PacketType : uint8_t
    {
        CONNECT = 1, CONNACK = 2, PUBLISH = 3, PUBACK = 4, PUBREC = 5, PUBREL = 6, PUBCOMP = 7,
        SUBSCRIBE = 8, SUBACK = 9, UNSUBSCRIBE = 10, UNSUBACK = 11, PINGREQ = 12, PINGRESP = 13, DISCONNECT = 14
    };

    // Nuki app limits: user name and password up to 32 characters (MQTT API 1.5).
    constexpr size_t LEN_USER = 32;
    constexpr size_t LEN_PASS = 32;
    constexpr size_t LEN_CLIENT_ID = 32;
    constexpr size_t LEN_TOPIC = 63;      // "nuki/XXXXXXXX/doorsensorBatteryCritical" is 39
    constexpr size_t RX_CAP = 320;        // one packet; bigger PUBLISHes are acked and dropped
    constexpr size_t TX_CAP = 96;         // the largest packet we send (PUBLISH lockAction)
    constexpr size_t QOS2_IN_SLOTS = 4;   // inbound QoS 2 ids awaiting PUBREL
    constexpr int64_t CONNECT_TIMEOUT_MS = 5000;

    // CONNACK return codes (MQTT 3.1.1, 3.2.2.3).
    enum class ConnectResult : uint8_t
    {
        Accepted = 0,
        BadProtocol = 1,
        IdentifierRejected = 2,
        BadCredentials = 4,
        NotAuthorized = 5,
        Malformed = 0xff // no CONNACK, just close
    };

    inline const char* connectResultText(ConnectResult r)
    {
        switch(r)
        {
        case ConnectResult::Accepted: return "accepted";
        case ConnectResult::BadProtocol: return "unsupported MQTT protocol version";
        case ConnectResult::IdentifierRejected: return "client ID not allowed";
        case ConnectResult::BadCredentials: return "bad user name or password";
        case ConnectResult::NotAuthorized: return "not authorized";
        default: return "malformed CONNECT";
        }
    }

    // Constant-time for equal lengths.
    inline bool ctEqual(const char* a, size_t aLen, const char* b)
    {
        const size_t bLen = strlen(b);
        uint8_t diff = aLen == bLen ? 0 : 1;
        const size_t n = aLen < bLen ? aLen : bLen;
        for(size_t i = 0; i < n; i++)
        {
            diff |= (uint8_t)a[i] ^ (uint8_t)b[i];
        }
        return diff == 0;
    }

    // --- fixed header --------------------------------------------------------

    // Returns the fixed header length (2..5) once complete, 0 if more bytes are
    // needed, -1 if the remaining length is malformed (more than 4 bytes).
    inline int decodeFixedHeader(const uint8_t* buf, size_t len, uint8_t& type, uint8_t& flags, uint32_t& remaining)
    {
        if(len < 2)
        {
            return 0;
        }
        type = buf[0] >> 4;
        flags = buf[0] & 0x0f;
        remaining = 0;
        uint32_t mul = 1;
        for(size_t i = 1; i <= 4; i++)
        {
            if(i >= len)
            {
                return 0;
            }
            remaining += (uint32_t)(buf[i] & 0x7f) * mul;
            if((buf[i] & 0x80) == 0)
            {
                return (int)i + 1;
            }
            mul *= 128;
        }
        return -1;
    }

    inline size_t encodeRemaining(uint8_t* out, uint32_t len)
    {
        size_t n = 0;
        do
        {
            uint8_t b = len % 128;
            len /= 128;
            if(len > 0)
            {
                b |= 0x80;
            }
            out[n++] = b;
        } while(len > 0 && n < 4);
        return n;
    }

    // --- field reader --------------------------------------------------------

    class Cursor
    {
    public:
        Cursor(const uint8_t* p, size_t len) : _p(p), _len(len) {}
        bool u8(uint8_t& v)
        {
            if(_pos + 1 > _len) return false;
            v = _p[_pos++];
            return true;
        }
        bool u16(uint16_t& v)
        {
            if(_pos + 2 > _len) return false;
            v = (uint16_t)((_p[_pos] << 8) | _p[_pos + 1]);
            _pos += 2;
            return true;
        }
        // Length-prefixed string/binary: points into the buffer.
        bool str(const char*& s, size_t& n)
        {
            uint16_t l;
            if(!u16(l) || _pos + l > _len) return false;
            s = (const char*)_p + _pos;
            n = l;
            _pos += l;
            return true;
        }
        size_t pos() const { return _pos; }
        size_t left() const { return _len - _pos; }

    private:
        const uint8_t* _p;
        size_t _len;
        size_t _pos = 0;
    };

    // --- CONNECT ---------------------------------------------------------------

    struct Credentials
    {
        const char* user;     // required, never empty when the server runs
        const char* pass;
        const char* clientId; // "" = any client ID
    };

    struct ConnectInfo
    {
        uint8_t protocolLevel = 0;
        bool cleanSession = false;
        bool hasWill = false;
        uint16_t keepAliveS = 0;
        char clientId[LEN_CLIENT_ID + 1] = {0}; // truncated copy, for the log and the status page
    };

    // body = variable header + payload of a CONNECT (after the fixed header).
    inline ConnectResult parseConnect(const uint8_t* body, size_t len, const Credentials& cred, ConnectInfo& info)
    {
        Cursor c(body, len);
        const char* proto;
        size_t protoLen;
        uint8_t level, flags;
        if(!c.str(proto, protoLen) || !c.u8(level) || !c.u8(flags) || !c.u16(info.keepAliveS))
        {
            return ConnectResult::Malformed;
        }
        const bool mqtt311 = protoLen == 4 && memcmp(proto, "MQTT", 4) == 0;
        const bool mqtt31 = protoLen == 6 && memcmp(proto, "MQIsdp", 6) == 0;
        if(!mqtt311 && !mqtt31)
        {
            return ConnectResult::Malformed;
        }
        info.protocolLevel = level;
        if((mqtt311 && level != 4) || (mqtt31 && level != 3))
        {
            return ConnectResult::BadProtocol;
        }
        if(flags & 0x01)
        {
            return ConnectResult::Malformed; // reserved bit
        }
        info.cleanSession = flags & 0x02;
        info.hasWill = flags & 0x04;
        const uint8_t willQos = (flags >> 3) & 0x03;
        const bool hasUser = flags & 0x80;
        const bool hasPass = flags & 0x40;
        if(willQos == 3 || (!info.hasWill && (willQos != 0 || (flags & 0x20))))
        {
            return ConnectResult::Malformed;
        }

        const char* clientId;
        size_t clientIdLen;
        if(!c.str(clientId, clientIdLen))
        {
            return ConnectResult::Malformed;
        }
        const size_t copy = clientIdLen < LEN_CLIENT_ID ? clientIdLen : LEN_CLIENT_ID;
        memcpy(info.clientId, clientId, copy);
        info.clientId[copy] = 0;
        for(size_t i = 0; i < copy; i++)
        {
            const char ch = info.clientId[i];
            if(ch < 0x20 || ch > 0x7e)
            {
                info.clientId[i] = '?'; // for the log only
            }
        }
        if(info.hasWill)
        {
            const char* s;
            size_t n;
            if(!c.str(s, n) || !c.str(s, n))
            {
                return ConnectResult::Malformed;
            }
        }
        const char* user = nullptr;
        size_t userLen = 0;
        const char* pass = nullptr;
        size_t passLen = 0;
        if(hasUser && !c.str(user, userLen))
        {
            return ConnectResult::Malformed;
        }
        if(hasPass && !c.str(pass, passLen))
        {
            return ConnectResult::Malformed;
        }
        if(c.left() != 0)
        {
            return ConnectResult::Malformed;
        }
        // 3.1.1: a password without a user name is a protocol error.
        if(hasPass && !hasUser)
        {
            return ConnectResult::Malformed;
        }
        if(cred.user == nullptr || cred.user[0] == 0 || cred.pass == nullptr || cred.pass[0] == 0)
        {
            return ConnectResult::NotAuthorized; // server not configured
        }
        // Evaluate both, so the timing doesn't say which one was wrong.
        const bool userOk = hasUser && ctEqual(user, userLen, cred.user);
        const bool passOk = hasPass && ctEqual(pass, passLen, cred.pass);
        if(!(userOk && passOk))
        {
            return ConnectResult::BadCredentials;
        }
        if(cred.clientId != nullptr && cred.clientId[0] != 0)
        {
            if(clientIdLen != strlen(cred.clientId) || strncasecmp(clientId, cred.clientId, clientIdLen) != 0)
            {
                return ConnectResult::IdentifierRejected;
            }
        }
        return ConnectResult::Accepted;
    }

    // --- topics -------------------------------------------------------------------

    // MQTT topic filter match with '+' and '#'. Filters starting with a
    // wildcard don't match topics starting with '$' (none here anyway).
    inline bool topicMatches(const char* filter, size_t filterLen, const char* topic)
    {
        size_t f = 0;
        const char* t = topic;
        while(f < filterLen)
        {
            const char fc = filter[f];
            if(fc == '#')
            {
                return f + 1 == filterLen && (f == 0 || filter[f - 1] == '/');
            }
            if(fc == '+')
            {
                if(f > 0 && filter[f - 1] != '/')
                {
                    return false;
                }
                while(*t != 0 && *t != '/') t++;
                f++;
                if(f < filterLen && filter[f] != '/')
                {
                    return false;
                }
                continue;
            }
            if(*t == 0)
            {
                // "a/b/#" also matches "a/b"
                return f + 2 == filterLen && filter[f] == '/' && filter[f + 1] == '#';
            }
            if(fc != *t)
            {
                return false;
            }
            f++;
            t++;
        }
        return *t == 0;
    }

    // "nuki/" + Nuki ID in upper-case hex, as NukiOfficial::setUid builds it.
    inline void officialPath(uint32_t nukiId, char* out, size_t cap)
    {
        static const char* hex = "0123456789ABCDEF";
        char digits[9];
        int n = 0;
        do
        {
            digits[n++] = hex[nukiId & 0xf];
            nukiId >>= 4;
        } while(nukiId != 0 && n < 8);
        size_t o = 0;
        const char* prefix = "nuki/";
        for(const char* p = prefix; *p && o + 1 < cap; p++) out[o++] = *p;
        while(n > 0 && o + 1 < cap) out[o++] = digits[--n];
        if(cap > 0) out[o] = 0;
    }

    // --- encoders (return the packet length, 0 if it doesn't fit) --------------

    inline size_t encodeConnack(uint8_t* out, ConnectResult rc)
    {
        out[0] = CONNACK << 4;
        out[1] = 2;
        out[2] = 0; // session present: never (clean sessions only)
        out[3] = (uint8_t)rc;
        return 4;
    }

    inline size_t encodeAck(uint8_t* out, PacketType type, uint16_t id)
    {
        out[0] = (uint8_t)((type << 4) | (type == PUBREL ? 0x02 : 0x00));
        out[1] = 2;
        out[2] = (uint8_t)(id >> 8);
        out[3] = (uint8_t)id;
        return 4;
    }

    inline size_t encodePingResp(uint8_t* out)
    {
        out[0] = PINGRESP << 4;
        out[1] = 0;
        return 2;
    }

    inline size_t encodeSuback(uint8_t* out, size_t cap, uint16_t id, const uint8_t* codes, size_t n)
    {
        if(n + 4 > cap || n + 2 > 127)
        {
            return 0;
        }
        out[0] = SUBACK << 4;
        out[1] = (uint8_t)(2 + n);
        out[2] = (uint8_t)(id >> 8);
        out[3] = (uint8_t)id;
        memcpy(out + 4, codes, n);
        return 4 + n;
    }

    inline size_t encodePublish(uint8_t* out, size_t cap, const char* topic, const char* payload, uint8_t qos, uint16_t id)
    {
        const size_t tl = strlen(topic);
        const size_t pl = strlen(payload);
        const uint32_t remaining = (uint32_t)(2 + tl + (qos > 0 ? 2 : 0) + pl);
        uint8_t hdr[5];
        hdr[0] = (uint8_t)((PUBLISH << 4) | (qos << 1));
        const size_t hl = 1 + encodeRemaining(hdr + 1, remaining);
        if(hl + remaining > cap || tl > 0xffff)
        {
            return 0;
        }
        size_t o = 0;
        memcpy(out, hdr, hl);
        o += hl;
        out[o++] = (uint8_t)(tl >> 8);
        out[o++] = (uint8_t)tl;
        memcpy(out + o, topic, tl);
        o += tl;
        if(qos > 0)
        {
            out[o++] = (uint8_t)(id >> 8);
            out[o++] = (uint8_t)id;
        }
        memcpy(out + o, payload, pl);
        return o + pl;
    }

    // --- one connection -------------------------------------------------------------

    // Callbacks from Session::feed (and publishLockAction) into the server.
    class SessionEvents
    {
    public:
        virtual ~SessionEvents() {}
        virtual void send(const uint8_t* data, size_t len) = 0;
        // Complete PUBLISH from the client. topic is NUL-terminated; payload is not.
        virtual void onPublish(const char* topic, const uint8_t* payload, size_t len) = 0;
        // lockAction subscription granted (qos 0..2) or removed (-1).
        virtual void onLockActionSubscription(int qos) = 0;
        // The client acknowledged our lockAction PUBLISH: PUBACK (QoS 1) or PUBREC (QoS 2), then PUBCOMP.
        virtual void onLockActionAck(PacketType type) = 0;
    };

    enum class CloseReason : uint8_t
    {
        None, Disconnect, Refused, Malformed, ProtocolError, Timeout, ConnectTimeout
    };

    inline const char* closeReasonText(CloseReason r)
    {
        switch(r)
        {
        case CloseReason::Disconnect: return "DISCONNECT from the client";
        case CloseReason::Refused: return "CONNECT refused";
        case CloseReason::Malformed: return "malformed packet";
        case CloseReason::ProtocolError: return "protocol error";
        case CloseReason::Timeout: return "keepalive timeout";
        case CloseReason::ConnectTimeout: return "no CONNECT in time";
        default: return "closed";
        }
    }

    // Outbound lockAction delivery state (one at a time: the lock processes one
    // command at a time anyway).
    enum class ActionState : uint8_t { None, Sent, Received, Completed };

    class Session
    {
    public:
        enum class State : uint8_t { Idle, AwaitConnect, Live, Closed };

        // lockActionTopic: "nuki/<ID>/lockAction". actionQos: the QoS we'd like
        // to send lockAction with (capped by the granted subscription QoS).
        void begin(int64_t nowMs, const char* lockActionTopic, uint8_t actionQos)
        {
            _state = State::AwaitConnect;
            _acceptedMs = nowMs;
            _lastRxMs = nowMs;
            _connectedMs = 0;
            _rxLen = 0;
            _discard = 0;
            _info = ConnectInfo();
            _lockActionQos = -1;
            _actionQos = actionQos > 2 ? 2 : actionQos;
            _nextId = 1;
            _actionId = 0;
            _actionState = ActionState::None;
            _closeReason = CloseReason::None;
            _refusal = ConnectResult::Accepted;
            _oversized = 0;
            memset(_qos2In, 0, sizeof(_qos2In));
            _qos2Next = 0;
            size_t n = strlen(lockActionTopic);
            if(n > LEN_TOPIC) n = LEN_TOPIC;
            memcpy(_lockActionTopic, lockActionTopic, n);
            _lockActionTopic[n] = 0;
        }

        void reset()
        {
            _state = State::Idle;
        }

        // Feed bytes read from the socket. Returns false once the connection
        // must be closed (closeReason() says why). A CONNECT that was just
        // accepted leaves state() == Live.
        bool feed(const uint8_t* data, size_t len, int64_t nowMs, const Credentials& cred, SessionEvents& ev)
        {
            if(_state != State::AwaitConnect && _state != State::Live)
            {
                return false;
            }
            _lastRxMs = nowMs;
            size_t i = 0;
            while(i < len)
            {
                if(_discard > 0)
                {
                    const size_t n = (len - i) < _discard ? (len - i) : _discard;
                    _discard -= n;
                    i += n;
                    continue;
                }
                const size_t room = RX_CAP - _rxLen;
                if(room == 0)
                {
                    return close(CloseReason::Malformed); // not reached: drain() empties a full buffer
                }
                const size_t n = (len - i) < room ? (len - i) : room;
                memcpy(_rx + _rxLen, data + i, n);
                _rxLen += n;
                i += n;
                if(!drain(nowMs, cred, ev))
                {
                    return false;
                }
            }
            return true;
        }

        // Keepalive and CONNECT timeout. Returns false if the connection must be closed.
        bool poll(int64_t nowMs)
        {
            if(_state == State::AwaitConnect && nowMs - _acceptedMs > CONNECT_TIMEOUT_MS)
            {
                return close(CloseReason::ConnectTimeout);
            }
            // MQTT 3.1.1 3.1.2.10: one and a half times the keepalive.
            if(_state == State::Live && _info.keepAliveS > 0 && nowMs - _lastRxMs > (int64_t)_info.keepAliveS * 1500)
            {
                return close(CloseReason::Timeout);
            }
            return _state == State::AwaitConnect || _state == State::Live;
        }

        // Encode and send lockAction (payload e.g. "2"). Returns false if the
        // session isn't live or the lock doesn't subscribe to lockAction.
        bool publishLockAction(const char* payload, SessionEvents& ev, uint16_t* idOut = nullptr)
        {
            if(_state != State::Live || _lockActionQos < 0)
            {
                return false;
            }
            const uint8_t qos = (uint8_t)(_actionQos < _lockActionQos ? _actionQos : _lockActionQos);
            uint16_t id = 0;
            if(qos > 0)
            {
                id = _nextId++;
                if(_nextId == 0) _nextId = 1;
            }
            uint8_t out[TX_CAP];
            const size_t n = encodePublish(out, sizeof(out), _lockActionTopic, payload, qos, id);
            if(n == 0)
            {
                return false;
            }
            _actionId = id;
            _actionState = qos == 0 ? ActionState::Completed : ActionState::Sent;
            _lastActionQos = qos;
            ev.send(out, n);
            if(idOut != nullptr) *idOut = id;
            return true;
        }

        State state() const { return _state; }
        const ConnectInfo& info() const { return _info; }
        CloseReason closeReason() const { return _closeReason; }
        ConnectResult refusal() const { return _refusal; }
        int lockActionQos() const { return _lockActionQos; }
        ActionState actionState() const { return _actionState; }
        uint8_t lastActionQos() const { return _lastActionQos; }
        int64_t lastRxMs() const { return _lastRxMs; }
        int64_t connectedMs() const { return _connectedMs; }
        uint32_t oversized() const { return _oversized; }

    private:
        bool close(CloseReason r)
        {
            if(_closeReason == CloseReason::None)
            {
                _closeReason = r;
            }
            _state = State::Closed;
            return false;
        }

        // Process every complete packet in _rx.
        bool drain(int64_t nowMs, const Credentials& cred, SessionEvents& ev)
        {
            while(_rxLen > 0)
            {
                uint8_t type, flags;
                uint32_t remaining;
                const int hl = decodeFixedHeader(_rx, _rxLen, type, flags, remaining);
                if(hl < 0)
                {
                    return close(CloseReason::Malformed);
                }
                if(hl == 0)
                {
                    return true; // need more
                }
                const size_t total = (size_t)hl + remaining;
                if(total > RX_CAP)
                {
                    return oversized(type, flags, (size_t)hl, total, ev);
                }
                if(total > _rxLen)
                {
                    return true; // need more
                }
                if(!packet(type, flags, _rx + hl, remaining, nowMs, cred, ev))
                {
                    return false;
                }
                memmove(_rx, _rx + total, _rxLen - total);
                _rxLen -= total;
            }
            return true;
        }

        // A packet bigger than RX_CAP: only a PUBLISH from a live client is
        // tolerated (e.g. Home Assistant discovery configs if the user left
        // auto discovery on). It is acknowledged and dropped.
        bool oversized(uint8_t type, uint8_t flags, size_t hl, size_t total, SessionEvents& ev)
        {
            if(type != PUBLISH || _state != State::Live)
            {
                return close(CloseReason::Malformed);
            }
            if(_rxLen < RX_CAP)
            {
                return true; // wait until the buffer is full, the id is near the start
            }
            const uint8_t qos = (flags >> 1) & 0x03;
            Cursor c(_rx + hl, _rxLen - hl);
            const char* topic;
            size_t topicLen;
            uint16_t id = 0;
            if(qos == 3 || !c.str(topic, topicLen) || (qos > 0 && !c.u16(id)))
            {
                return close(CloseReason::Malformed);
            }
            ackPublish(qos, id, ev);
            _oversized++;
            _discard = total - _rxLen;
            _rxLen = 0;
            return true;
        }

        void ackPublish(uint8_t qos, uint16_t id, SessionEvents& ev)
        {
            uint8_t out[4];
            if(qos == 1)
            {
                ev.send(out, encodeAck(out, PUBACK, id));
            }
            else if(qos == 2)
            {
                ev.send(out, encodeAck(out, PUBREC, id));
            }
        }

        bool qos2Seen(uint16_t id) const
        {
            for(uint16_t x : _qos2In)
            {
                if(x == id) return true;
            }
            return false;
        }

        void qos2Remember(uint16_t id)
        {
            _qos2In[_qos2Next] = id;
            _qos2Next = (_qos2Next + 1) % QOS2_IN_SLOTS;
        }

        void qos2Forget(uint16_t id)
        {
            for(uint16_t& x : _qos2In)
            {
                if(x == id) x = 0;
            }
        }

        bool packet(uint8_t type, uint8_t flags, const uint8_t* body, size_t len, int64_t nowMs,
                    const Credentials& cred, SessionEvents& ev)
        {
            uint8_t out[8];
            if(_state == State::AwaitConnect)
            {
                if(type != CONNECT || flags != 0)
                {
                    return close(CloseReason::ProtocolError);
                }
                const ConnectResult rc = parseConnect(body, len, cred, _info);
                _refusal = rc;
                if(rc == ConnectResult::Malformed)
                {
                    return close(CloseReason::Malformed);
                }
                ev.send(out, encodeConnack(out, rc));
                if(rc != ConnectResult::Accepted)
                {
                    return close(CloseReason::Refused);
                }
                _state = State::Live;
                _connectedMs = nowMs;
                return true;
            }

            switch(type)
            {
            case PUBLISH:
            {
                const uint8_t qos = (flags >> 1) & 0x03;
                Cursor c(body, len);
                const char* topic;
                size_t topicLen;
                uint16_t id = 0;
                if(qos == 3 || !c.str(topic, topicLen) || (qos > 0 && (!c.u16(id) || id == 0)) || topicLen == 0)
                {
                    return close(CloseReason::Malformed);
                }
                const bool duplicate = qos == 2 && qos2Seen(id);
                ackPublish(qos, id, ev);
                if(duplicate)
                {
                    return true; // re-sent before our PUBREC arrived: delivered already
                }
                if(qos == 2)
                {
                    qos2Remember(id);
                }
                if(topicLen > LEN_TOPIC)
                {
                    _oversized++;
                    return true;
                }
                char t[LEN_TOPIC + 1];
                memcpy(t, topic, topicLen);
                t[topicLen] = 0;
                if(strlen(t) != topicLen)
                {
                    return close(CloseReason::Malformed); // embedded NUL
                }
                ev.onPublish(t, body + c.pos(), c.left());
                return true;
            }
            case PUBREL:
            {
                Cursor c(body, len);
                uint16_t id;
                if(flags != 0x02 || !c.u16(id))
                {
                    return close(CloseReason::Malformed);
                }
                qos2Forget(id);
                ev.send(out, encodeAck(out, PUBCOMP, id));
                return true;
            }
            case PUBACK:
            case PUBREC:
            case PUBCOMP:
            {
                Cursor c(body, len);
                uint16_t id;
                if(flags != 0 || !c.u16(id))
                {
                    return close(CloseReason::Malformed);
                }
                if(type == PUBREC)
                {
                    ev.send(out, encodeAck(out, PUBREL, id));
                }
                if(id != 0 && id == _actionId)
                {
                    if(type == PUBCOMP || (type == PUBACK && _lastActionQos == 1))
                    {
                        _actionState = ActionState::Completed;
                    }
                    else if(_actionState == ActionState::Sent)
                    {
                        _actionState = ActionState::Received;
                    }
                    ev.onLockActionAck((PacketType)type);
                }
                return true;
            }
            case SUBSCRIBE:
            case UNSUBSCRIBE:
            {
                Cursor c(body, len);
                uint16_t id;
                if(flags != 0x02 || !c.u16(id) || c.left() == 0)
                {
                    return close(CloseReason::Malformed);
                }
                uint8_t codes[16];
                size_t n = 0;
                int lockActionQos = _lockActionQos;
                while(c.left() > 0)
                {
                    const char* filter;
                    size_t filterLen;
                    uint8_t reqQos = 0;
                    if(!c.str(filter, filterLen) || filterLen == 0 || (type == SUBSCRIBE && !c.u8(reqQos)))
                    {
                        return close(CloseReason::Malformed);
                    }
                    const bool covers = topicMatches(filter, filterLen, _lockActionTopic);
                    if(type == SUBSCRIBE)
                    {
                        if(n == sizeof(codes))
                        {
                            return close(CloseReason::ProtocolError);
                        }
                        const bool bad = reqQos > 2;
                        codes[n++] = bad ? 0x80 : reqQos;
                        if(covers && !bad)
                        {
                            lockActionQos = reqQos;
                        }
                    }
                    else if(covers)
                    {
                        lockActionQos = -1;
                    }
                }
                if(type == SUBSCRIBE)
                {
                    uint8_t sub[4 + sizeof(codes)];
                    ev.send(sub, encodeSuback(sub, sizeof(sub), id, codes, n));
                }
                else
                {
                    ev.send(out, encodeAck(out, UNSUBACK, id));
                }
                if(lockActionQos != _lockActionQos)
                {
                    _lockActionQos = lockActionQos;
                    ev.onLockActionSubscription(lockActionQos);
                }
                return true;
            }
            case PINGREQ:
                if(flags != 0 || len != 0)
                {
                    return close(CloseReason::Malformed);
                }
                ev.send(out, encodePingResp(out));
                return true;
            case DISCONNECT:
                return close(CloseReason::Disconnect);
            default:
                return close(CloseReason::ProtocolError); // incl. a second CONNECT
            }
        }

        State _state = State::Idle;
        uint8_t _rx[RX_CAP];
        size_t _rxLen = 0;
        size_t _discard = 0;
        int64_t _acceptedMs = 0;
        int64_t _connectedMs = 0;
        int64_t _lastRxMs = 0;
        ConnectInfo _info;
        char _lockActionTopic[LEN_TOPIC + 1] = {0};
        int _lockActionQos = -1;
        uint8_t _actionQos = 2;
        uint8_t _lastActionQos = 0;
        uint16_t _nextId = 1;
        uint16_t _actionId = 0;
        ActionState _actionState = ActionState::None;
        uint16_t _qos2In[QOS2_IN_SLOTS] = {0};
        size_t _qos2Next = 0;
        CloseReason _closeReason = CloseReason::None;
        ConnectResult _refusal = ConnectResult::Accepted;
        uint32_t _oversized = 0;
    };

    // --- redundant webhook actions -------------------------------------------------

    // Nuki MQTT API 1.6, 3.3 / 3.6.
    constexpr uint8_t LOCK_STATE_LOCKED = 1;
    constexpr uint8_t LOCK_STATE_UNLOCKED = 3;
    constexpr uint8_t DOOR_DEACTIVATED = 1;
    constexpr uint8_t DOOR_CLOSED = 2;

    struct LiveState
    {
        bool sessionLive;        // the lock's MQTT session is up
        bool haveState;          // .../state arrived during this session
        bool stateAfterCommand;  // ... and after the last lock action Nuki Hub sent (any transport)
        uint8_t lockState;
        bool haveDoor;           // .../doorsensorState arrived during this session
        uint8_t doorState;
        int64_t lastRxAgeMs;     // since the last packet from the lock (PUBLISH, PINGREQ, ...)
    };

    enum class Skip : uint8_t { Send, AlreadyLocked, AlreadyUnlocked };

    // Skip `lock` if locked (and, if a door sensor reports, the door is
    // closed), skip `unlock` if unlocked. Only with a live session whose state
    // is newer than Nuki Hub's last command and a lock heard from within
    // maxSilenceMs. Never anything else (unlatch, lockNgo*, fullLock, fob...).
    inline Skip redundantAction(const char* action, const LiveState& st, int64_t maxSilenceMs)
    {
        if(action == nullptr || !st.sessionLive || !st.haveState || !st.stateAfterCommand ||
           st.lastRxAgeMs < 0 || st.lastRxAgeMs > maxSilenceMs)
        {
            return Skip::Send;
        }
        if(strcmp(action, "lock") == 0)
        {
            const bool doorOk = !st.haveDoor || st.doorState == DOOR_DEACTIVATED || st.doorState == DOOR_CLOSED;
            return st.lockState == LOCK_STATE_LOCKED && doorOk ? Skip::AlreadyLocked : Skip::Send;
        }
        if(strcmp(action, "unlock") == 0)
        {
            return st.lockState == LOCK_STATE_UNLOCKED ? Skip::AlreadyUnlocked : Skip::Send;
        }
        return Skip::Send;
    }
}
