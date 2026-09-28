// Host tests for src/LockMqttLogic.h (embedded MQTT server for the Nuki
// lock): pio test -e native

#include <unity.h>
#include <cstring>
#include <string>
#include <vector>
#include "LockMqttLogic.h"

using namespace LockMqttLogic;

namespace
{
    const char* TOPIC_ACTION = "nuki/2BB28570/lockAction";

    struct Recorder : SessionEvents
    {
        std::vector<std::vector<uint8_t>> sent;
        std::vector<std::pair<std::string, std::string>> published;
        std::vector<int> subscriptions;
        std::vector<PacketType> acks;

        void send(const uint8_t* data, size_t len) override
        {
            sent.emplace_back(data, data + len);
        }
        void onPublish(const char* topic, const uint8_t* payload, size_t len) override
        {
            published.emplace_back(topic, std::string((const char*)payload, len));
        }
        void onLockActionSubscription(int qos) override
        {
            subscriptions.push_back(qos);
        }
        void onLockActionAck(PacketType type) override
        {
            acks.push_back(type);
        }
    };

    void putStr(std::vector<uint8_t>& v, const char* s)
    {
        const size_t n = strlen(s);
        v.push_back((uint8_t)(n >> 8));
        v.push_back((uint8_t)n);
        v.insert(v.end(), s, s + n);
    }

    std::vector<uint8_t> packet(uint8_t first, const std::vector<uint8_t>& body)
    {
        std::vector<uint8_t> p;
        p.push_back(first);
        uint8_t len[4];
        const size_t n = encodeRemaining(len, (uint32_t)body.size());
        p.insert(p.end(), len, len + n);
        p.insert(p.end(), body.begin(), body.end());
        return p;
    }

    // CONNECT as the Nuki lock sends it (mosquitto log: "Nuki_39C748EC (p2, c1, k300, u'...')").
    std::vector<uint8_t> connect(const char* clientId, const char* user, const char* pass, uint16_t keepAlive = 300,
                                 bool will = true, const char* proto = "MQTT", uint8_t level = 4)
    {
        std::vector<uint8_t> b;
        putStr(b, proto);
        b.push_back(level);
        uint8_t flags = 0x02; // clean session
        if(will) flags |= 0x04 | 0x20; // will, retained, QoS 0
        if(user) flags |= 0x80;
        if(pass) flags |= 0x40;
        b.push_back(flags);
        b.push_back((uint8_t)(keepAlive >> 8));
        b.push_back((uint8_t)keepAlive);
        putStr(b, clientId);
        if(will)
        {
            putStr(b, "nuki/2BB28570/connected");
            putStr(b, "false");
        }
        if(user) putStr(b, user);
        if(pass) putStr(b, pass);
        return packet(CONNECT << 4, b);
    }

    std::vector<uint8_t> publish(const char* topic, const char* payload, uint8_t qos, uint16_t id = 0, bool retain = false, bool dup = false)
    {
        std::vector<uint8_t> b;
        putStr(b, topic);
        if(qos > 0)
        {
            b.push_back((uint8_t)(id >> 8));
            b.push_back((uint8_t)id);
        }
        b.insert(b.end(), payload, payload + strlen(payload));
        return packet((uint8_t)((PUBLISH << 4) | (dup ? 8 : 0) | (qos << 1) | (retain ? 1 : 0)), b);
    }

    std::vector<uint8_t> subscribe(uint16_t id, const std::vector<std::pair<const char*, uint8_t>>& filters)
    {
        std::vector<uint8_t> b = { (uint8_t)(id >> 8), (uint8_t)id };
        for(const auto& f : filters)
        {
            putStr(b, f.first);
            b.push_back(f.second);
        }
        return packet((SUBSCRIBE << 4) | 0x02, b);
    }

    std::vector<uint8_t> ack(PacketType type, uint16_t id)
    {
        return packet((uint8_t)((type << 4) | (type == PUBREL ? 2 : 0)), { (uint8_t)(id >> 8), (uint8_t)id });
    }

    const Credentials CRED = { "nukilock", "s3cret-pass", "" };

    bool feed(Session& s, Recorder& r, const std::vector<uint8_t>& p, int64_t now = 1000, const Credentials& c = CRED)
    {
        return s.feed(p.data(), p.size(), now, c, r);
    }

    // A session with the lock connected and subscribed to lockAction at QoS 2.
    void liveSession(Session& s, Recorder& r, uint8_t actionQos = 2)
    {
        s.begin(0, TOPIC_ACTION, actionQos);
        TEST_ASSERT_TRUE(feed(s, r, connect("Nuki_2BB28570", "nukilock", "s3cret-pass")));
        TEST_ASSERT_TRUE(feed(s, r, subscribe(1, { { TOPIC_ACTION, 2 } })));
        r.sent.clear();
        r.subscriptions.clear();
    }
}

void test_remaining_length()
{
    uint8_t buf[5];
    const uint32_t values[] = { 0, 127, 128, 16383, 16384, 2097151, 2097152, 268435455 };
    for(uint32_t v : values)
    {
        buf[0] = 0x30;
        const size_t n = encodeRemaining(buf + 1, v);
        uint8_t type, flags;
        uint32_t decoded;
        TEST_ASSERT_EQUAL(1 + (int)n, decodeFixedHeader(buf, 1 + n, type, flags, decoded));
        TEST_ASSERT_EQUAL_UINT32(v, decoded);
        TEST_ASSERT_EQUAL(0, decodeFixedHeader(buf, n, type, flags, decoded)); // incomplete
    }
    const uint8_t bad[] = { 0x30, 0xff, 0xff, 0xff, 0xff, 0x01 };
    uint8_t type, flags;
    uint32_t decoded;
    TEST_ASSERT_EQUAL(-1, decodeFixedHeader(bad, sizeof(bad), type, flags, decoded));
}

void test_connect_accepts_the_lock()
{
    Session s;
    Recorder r;
    s.begin(0, TOPIC_ACTION, 2);
    TEST_ASSERT_TRUE(feed(s, r, connect("Nuki_2BB28570", "nukilock", "s3cret-pass")));
    TEST_ASSERT_TRUE(s.state() == Session::State::Live);
    TEST_ASSERT_EQUAL_STRING("Nuki_2BB28570", s.info().clientId);
    TEST_ASSERT_EQUAL(300, s.info().keepAliveS);
    TEST_ASSERT_TRUE(s.info().cleanSession);
    TEST_ASSERT_TRUE(s.info().hasWill);
    TEST_ASSERT_EQUAL(1, (int)r.sent.size());
    const std::vector<uint8_t> connack = { 0x20, 0x02, 0x00, 0x00 };
    TEST_ASSERT_TRUE(r.sent[0] == connack);
}

void test_connect_refusals()
{
    struct Case
    {
        std::vector<uint8_t> p;
        Credentials cred;
        uint8_t rc; // 0xff = closed without CONNACK
    };
    const Credentials withId = { "nukilock", "s3cret-pass", "Nuki_2BB28570" };
    const Credentials notConfigured = { "", "", "" };
    const Case cases[] = {
        { connect("Nuki_2BB28570", "nukilock", "wrong"), CRED, 4 },
        { connect("Nuki_2BB28570", "other", "s3cret-pass"), CRED, 4 },
        { connect("Nuki_2BB28570", "nukilock", "s3cret-pas"), CRED, 4 },     // prefix
        { connect("Nuki_2BB28570", "nukilock", "s3cret-pass!"), CRED, 4 },   // longer
        { connect("Nuki_2BB28570", nullptr, nullptr), CRED, 4 },            // anonymous
        { connect("Nuki_2BB28570", "nukilock", nullptr), CRED, 4 },         // no password
        { connect("Nuki_2BB28570", "nukilock", "s3cret-pass"), notConfigured, 5 },
        { connect("Nuki_11111111", "nukilock", "s3cret-pass"), withId, 2 },
        { connect("mosquitto_pub", "nukilock", "s3cret-pass"), withId, 2 },
        { connect("Nuki_2BB28570", "nukilock", "s3cret-pass", 60, true, "MQTT", 5), CRED, 1 }, // MQTT 5
        { connect("Nuki_2BB28570", "nukilock", "s3cret-pass", 60, true, "MQTT", 3), CRED, 1 },
        { connect("Nuki_2BB28570", "nukilock", "s3cret-pass", 60, true, "XYZW", 4), CRED, 0xff },
    };
    for(const Case& c : cases)
    {
        Session s;
        Recorder r;
        s.begin(0, TOPIC_ACTION, 2);
        TEST_ASSERT_FALSE(feed(s, r, c.p, 1000, c.cred));
        TEST_ASSERT_TRUE(s.state() == Session::State::Closed);
        if(c.rc == 0xff)
        {
            TEST_ASSERT_EQUAL(0, (int)r.sent.size());
            TEST_ASSERT_TRUE(s.closeReason() == CloseReason::Malformed);
        }
        else
        {
            TEST_ASSERT_EQUAL(1, (int)r.sent.size());
            TEST_ASSERT_EQUAL_HEX8(c.rc, r.sent[0][3]);
            TEST_ASSERT_TRUE(s.closeReason() == CloseReason::Refused);
        }
    }

    // The expected client ID matches case-insensitively; MQTT 3.1 is fine too.
    Session s;
    Recorder r;
    s.begin(0, TOPIC_ACTION, 2);
    TEST_ASSERT_TRUE(feed(s, r, connect("nuki_2bb28570", "nukilock", "s3cret-pass", 60, false, "MQIsdp", 3), 1000, withId));
    TEST_ASSERT_TRUE(s.state() == Session::State::Live);
}

void test_first_packet_must_be_connect()
{
    Session s;
    Recorder r;
    s.begin(0, TOPIC_ACTION, 2);
    TEST_ASSERT_FALSE(feed(s, r, publish(TOPIC_ACTION, "3", 0)));
    TEST_ASSERT_TRUE(s.closeReason() == CloseReason::ProtocolError);
    TEST_ASSERT_EQUAL(0, (int)r.published.size());

    Session s2;
    Recorder r2;
    s2.begin(0, TOPIC_ACTION, 2);
    const std::vector<uint8_t> ping = { 0xc0, 0x00 };
    TEST_ASSERT_FALSE(feed(s2, r2, ping));
}

void test_connect_timeout_and_keepalive()
{
    Session s;
    Recorder r;
    s.begin(0, TOPIC_ACTION, 2);
    TEST_ASSERT_TRUE(s.poll(CONNECT_TIMEOUT_MS));
    TEST_ASSERT_FALSE(s.poll(CONNECT_TIMEOUT_MS + 1));
    TEST_ASSERT_TRUE(s.closeReason() == CloseReason::ConnectTimeout);

    // A slow-loris CONNECT (bytes trickling in) doesn't extend the deadline.
    Session s2;
    Recorder r2;
    s2.begin(0, TOPIC_ACTION, 2);
    const std::vector<uint8_t> c = connect("Nuki_2BB28570", "nukilock", "s3cret-pass");
    TEST_ASSERT_TRUE(s2.feed(c.data(), 3, 4000, CRED, r2));
    TEST_ASSERT_FALSE(s2.poll(CONNECT_TIMEOUT_MS + 1));

    // Keepalive 300 s: closed after 450 s without any packet.
    Session s3;
    Recorder r3;
    s3.begin(0, TOPIC_ACTION, 2);
    TEST_ASSERT_TRUE(feed(s3, r3, connect("Nuki_2BB28570", "nukilock", "s3cret-pass", 300), 1000));
    TEST_ASSERT_TRUE(s3.poll(1000 + 450000));
    const std::vector<uint8_t> ping = { 0xc0, 0x00 };
    TEST_ASSERT_TRUE(feed(s3, r3, ping, 400000));
    const std::vector<uint8_t> pingresp = { 0xd0, 0x00 };
    TEST_ASSERT_TRUE(r3.sent.back() == pingresp);
    TEST_ASSERT_TRUE(s3.poll(400000 + 450000));
    TEST_ASSERT_FALSE(s3.poll(400000 + 450001));
    TEST_ASSERT_TRUE(s3.closeReason() == CloseReason::Timeout);
}

void test_subscribe_lock_action()
{
    Session s;
    Recorder r;
    s.begin(0, TOPIC_ACTION, 2);
    TEST_ASSERT_TRUE(feed(s, r, connect("Nuki_2BB28570", "nukilock", "s3cret-pass")));
    r.sent.clear();
    TEST_ASSERT_EQUAL(-1, s.lockActionQos());
    TEST_ASSERT_FALSE(s.publishLockAction("2", r)); // "Allow locking" off: nothing to send to

    TEST_ASSERT_TRUE(feed(s, r, subscribe(7, { { "nuki/2BB28570/lock", 2 }, { TOPIC_ACTION, 2 }, { "nuki/2BB28570/unlock", 2 } })));
    const std::vector<uint8_t> suback = { 0x90, 0x05, 0x00, 0x07, 0x02, 0x02, 0x02 };
    TEST_ASSERT_TRUE(r.sent.back() == suback);
    TEST_ASSERT_EQUAL(2, s.lockActionQos());
    TEST_ASSERT_EQUAL(1, (int)r.subscriptions.size());
    TEST_ASSERT_EQUAL(2, r.subscriptions[0]);

    // Unsubscribing removes it.
    std::vector<uint8_t> b = { 0x00, 0x08 };
    putStr(b, TOPIC_ACTION);
    TEST_ASSERT_TRUE(feed(s, r, packet((UNSUBSCRIBE << 4) | 2, b)));
    const std::vector<uint8_t> unsuback = { 0xb0, 0x02, 0x00, 0x08 };
    TEST_ASSERT_TRUE(r.sent.back() == unsuback);
    TEST_ASSERT_EQUAL(-1, s.lockActionQos());

    // Wildcards count; a bad QoS gets 0x80.
    TEST_ASSERT_TRUE(feed(s, r, subscribe(9, { { "nuki/+/lockAction", 1 }, { "x", 3 } })));
    const std::vector<uint8_t> suback2 = { 0x90, 0x04, 0x00, 0x09, 0x01, 0x80 };
    TEST_ASSERT_TRUE(r.sent.back() == suback2);
    TEST_ASSERT_EQUAL(1, s.lockActionQos());

    // SUBSCRIBE with wrong fixed-header flags is malformed.
    std::vector<uint8_t> sb = { 0x00, 0x0a };
    putStr(sb, TOPIC_ACTION);
    sb.push_back(2);
    TEST_ASSERT_FALSE(feed(s, r, packet(SUBSCRIBE << 4, sb)));
}

void test_topic_matching()
{
    struct Case
    {
        const char* filter;
        const char* topic;
        bool match;
    };
    const Case cases[] = {
        { "nuki/2BB28570/lockAction", "nuki/2BB28570/lockAction", true },
        { "nuki/2BB28570/lockaction", "nuki/2BB28570/lockAction", false },
        { "nuki/2BB28570/lock", "nuki/2BB28570/lockAction", false },
        { "nuki/+/lockAction", "nuki/2BB28570/lockAction", true },
        { "nuki/#", "nuki/2BB28570/lockAction", true },
        { "#", "nuki/2BB28570/lockAction", true },
        { "nuki/2BB28570/lockAction/#", "nuki/2BB28570/lockAction", true },
        { "nuki/+", "nuki/2BB28570/lockAction", false },
        { "+/+/+", "nuki/2BB28570/lockAction", true },
        { "nuki/2BB28570/lockAction/x", "nuki/2BB28570/lockAction", false },
        { "nuki/2B#", "nuki/2BB28570/lockAction", false },
        { "nuki/2B+/lockAction", "nuki/2BB28570/lockAction", false },
    };
    for(const Case& c : cases)
    {
        TEST_ASSERT_EQUAL_MESSAGE(c.match, topicMatches(c.filter, strlen(c.filter), c.topic), c.filter);
    }

    char path[32];
    officialPath(0x2bb28570, path, sizeof(path));
    TEST_ASSERT_EQUAL_STRING("nuki/2BB28570", path);
    officialPath(0x1a, path, sizeof(path));
    TEST_ASSERT_EQUAL_STRING("nuki/1A", path);
    officialPath(0, path, sizeof(path));
    TEST_ASSERT_EQUAL_STRING("nuki/0", path);
}

void test_inbound_publish_qos()
{
    Session s;
    Recorder r;
    liveSession(s, r);

    // State topics: QoS 0, retained.
    TEST_ASSERT_TRUE(feed(s, r, publish("nuki/2BB28570/state", "1", 0, 0, true)));
    TEST_ASSERT_EQUAL(0, (int)r.sent.size());
    TEST_ASSERT_EQUAL(1, (int)r.published.size());
    TEST_ASSERT_EQUAL_STRING("nuki/2BB28570/state", r.published[0].first.c_str());
    TEST_ASSERT_EQUAL_STRING("1", r.published[0].second.c_str());

    // QoS 1: PUBACK.
    TEST_ASSERT_TRUE(feed(s, r, publish("nuki/2BB28570/commandResponse", "0", 1, 5)));
    const std::vector<uint8_t> puback = { 0x40, 0x02, 0x00, 0x05 };
    TEST_ASSERT_TRUE(r.sent.back() == puback);

    // QoS 2: delivered once, PUBREC; a DUP re-send before PUBREL is not delivered again.
    TEST_ASSERT_TRUE(feed(s, r, publish("nuki/2BB28570/lockActionEvent", "1,172,0,0,0", 2, 6)));
    const std::vector<uint8_t> pubrec = { 0x50, 0x02, 0x00, 0x06 };
    TEST_ASSERT_TRUE(r.sent.back() == pubrec);
    TEST_ASSERT_EQUAL(3, (int)r.published.size());
    TEST_ASSERT_TRUE(feed(s, r, publish("nuki/2BB28570/lockActionEvent", "1,172,0,0,0", 2, 6, false, true)));
    TEST_ASSERT_TRUE(r.sent.back() == pubrec);
    TEST_ASSERT_EQUAL(3, (int)r.published.size());
    TEST_ASSERT_TRUE(feed(s, r, ack(PUBREL, 6)));
    const std::vector<uint8_t> pubcomp = { 0x70, 0x02, 0x00, 0x06 };
    TEST_ASSERT_TRUE(r.sent.back() == pubcomp);
    // After PUBREL the id is free again: a new message with it is delivered.
    TEST_ASSERT_TRUE(feed(s, r, publish("nuki/2BB28570/commandResponse", "0", 2, 6)));
    TEST_ASSERT_EQUAL(4, (int)r.published.size());

    // Invalid: QoS 3, packet id 0, empty topic.
    Session s2;
    Recorder r2;
    liveSession(s2, r2);
    TEST_ASSERT_FALSE(feed(s2, r2, publish("nuki/2BB28570/state", "1", 1, 0)));
}

void test_several_packets_and_split_reads()
{
    Session s;
    Recorder r;
    s.begin(0, TOPIC_ACTION, 2);
    std::vector<uint8_t> all = connect("Nuki_2BB28570", "nukilock", "s3cret-pass");
    const std::vector<std::vector<uint8_t>> more = {
        subscribe(1, { { TOPIC_ACTION, 2 } }),
        publish("nuki/2BB28570/connected", "true", 0, 0, true),
        publish("nuki/2BB28570/state", "3", 0, 0, true),
        publish("nuki/2BB28570/doorsensorState", "2", 0, 0, true),
    };
    for(const auto& p : more) all.insert(all.end(), p.begin(), p.end());
    // One byte at a time.
    for(size_t i = 0; i < all.size(); i++)
    {
        TEST_ASSERT_TRUE(s.feed(&all[i], 1, 1000, CRED, r));
    }
    TEST_ASSERT_EQUAL(3, (int)r.published.size());
    TEST_ASSERT_EQUAL_STRING("2", r.published[2].second.c_str());
    TEST_ASSERT_EQUAL(2, s.lockActionQos());

    // All at once.
    Session s2;
    Recorder r2;
    s2.begin(0, TOPIC_ACTION, 2);
    TEST_ASSERT_TRUE(s2.feed(all.data(), all.size(), 1000, CRED, r2));
    TEST_ASSERT_EQUAL(3, (int)r2.published.size());
}

void test_oversized_publish_is_acked_and_dropped()
{
    Session s;
    Recorder r;
    liveSession(s, r);
    std::string big(1500, 'x'); // e.g. a Home Assistant discovery config
    const std::vector<uint8_t> p = publish("homeassistant/lock/2BB28570/config", big.c_str(), 1, 42, true);
    const std::vector<uint8_t> next = publish("nuki/2BB28570/state", "1", 0);
    std::vector<uint8_t> both = p;
    both.insert(both.end(), next.begin(), next.end());
    // In chunks like a socket would deliver them.
    for(size_t i = 0; i < both.size(); i += 100)
    {
        const size_t n = both.size() - i < 100 ? both.size() - i : 100;
        TEST_ASSERT_TRUE(s.feed(&both[i], n, 1000, CRED, r));
    }
    const std::vector<uint8_t> puback = { 0x40, 0x02, 0x00, 0x2a };
    TEST_ASSERT_TRUE(r.sent[0] == puback);
    TEST_ASSERT_EQUAL(1, (int)r.published.size());
    TEST_ASSERT_EQUAL_STRING("nuki/2BB28570/state", r.published[0].first.c_str());
    TEST_ASSERT_EQUAL(1, (int)s.oversized());

    // An oversized CONNECT (or anything but PUBLISH) closes the connection.
    Session s2;
    Recorder r2;
    s2.begin(0, TOPIC_ACTION, 2);
    std::string longId(400, 'a');
    TEST_ASSERT_FALSE(feed(s2, r2, connect(longId.c_str(), "nukilock", "s3cret-pass")));
    TEST_ASSERT_TRUE(s2.closeReason() == CloseReason::Malformed);
}

void test_publish_lock_action_qos2()
{
    Session s;
    Recorder r;
    liveSession(s, r);
    uint16_t id = 0;
    TEST_ASSERT_TRUE(s.publishLockAction("2", r, &id));
    TEST_ASSERT_EQUAL(1, id);
    TEST_ASSERT_TRUE(s.actionState() == ActionState::Sent);
    std::vector<uint8_t> expected = { 0x34, (uint8_t)(2 + strlen(TOPIC_ACTION) + 2 + 1) };
    putStr(expected, TOPIC_ACTION);
    expected.push_back(0x00);
    expected.push_back(0x01);
    expected.push_back('2');
    TEST_ASSERT_TRUE(r.sent.back() == expected);

    // PUBREC: the lock has it -> we send PUBREL; PUBCOMP completes.
    TEST_ASSERT_TRUE(feed(s, r, ack(PUBREC, 1)));
    const std::vector<uint8_t> pubrel = { 0x62, 0x02, 0x00, 0x01 };
    TEST_ASSERT_TRUE(r.sent.back() == pubrel);
    TEST_ASSERT_TRUE(s.actionState() == ActionState::Received);
    TEST_ASSERT_TRUE(feed(s, r, ack(PUBCOMP, 1)));
    TEST_ASSERT_TRUE(s.actionState() == ActionState::Completed);
    TEST_ASSERT_EQUAL(2, (int)r.acks.size());
    TEST_ASSERT_TRUE(r.acks[0] == PUBREC);
    TEST_ASSERT_TRUE(r.acks[1] == PUBCOMP);

    // The next one gets a new id; an ack for an old id doesn't count.
    TEST_ASSERT_TRUE(s.publishLockAction("1", r, &id));
    TEST_ASSERT_EQUAL(2, id);
    TEST_ASSERT_TRUE(feed(s, r, ack(PUBREC, 1)));
    TEST_ASSERT_TRUE(s.actionState() == ActionState::Sent);
}

void test_publish_lock_action_qos_capped()
{
    // We ask for QoS 1, the lock subscribed with 2: QoS 1 (PUBACK completes).
    Session s;
    Recorder r;
    liveSession(s, r, 1);
    uint16_t id = 0;
    TEST_ASSERT_TRUE(s.publishLockAction("3", r, &id));
    TEST_ASSERT_EQUAL_HEX8(0x32, r.sent.back()[0]);
    TEST_ASSERT_TRUE(feed(s, r, ack(PUBACK, id)));
    TEST_ASSERT_TRUE(s.actionState() == ActionState::Completed);

    // Lock subscribed with QoS 0: QoS 0, no id.
    Session s0;
    Recorder r0;
    s0.begin(0, TOPIC_ACTION, 2);
    TEST_ASSERT_TRUE(feed(s0, r0, connect("Nuki_2BB28570", "nukilock", "s3cret-pass")));
    TEST_ASSERT_TRUE(feed(s0, r0, subscribe(1, { { TOPIC_ACTION, 0 } })));
    TEST_ASSERT_TRUE(s0.publishLockAction("1", r0));
    TEST_ASSERT_EQUAL_HEX8(0x30, r0.sent.back()[0]);
}

void test_disconnect_and_second_connect()
{
    Session s;
    Recorder r;
    liveSession(s, r);
    const std::vector<uint8_t> disconnect = { 0xe0, 0x00 };
    TEST_ASSERT_FALSE(feed(s, r, disconnect));
    TEST_ASSERT_TRUE(s.closeReason() == CloseReason::Disconnect);
    TEST_ASSERT_FALSE(s.publishLockAction("2", r));

    Session s2;
    Recorder r2;
    liveSession(s2, r2);
    TEST_ASSERT_FALSE(feed(s2, r2, connect("Nuki_2BB28570", "nukilock", "s3cret-pass")));
    TEST_ASSERT_TRUE(s2.closeReason() == CloseReason::ProtocolError);
}

void test_redundant_action_rule()
{
    LiveState st = { true, true, true, LOCK_STATE_LOCKED, false, 0, 1000 };
    const int64_t maxSilence = 330000;
    TEST_ASSERT_TRUE(redundantAction("lock", st, maxSilence) == Skip::AlreadyLocked);
    TEST_ASSERT_TRUE(redundantAction("unlock", st, maxSilence) == Skip::Send);
    // Never skipped, whatever the state.
    const char* never[] = { "unlatch", "lockNgo", "lockNgoUnlatch", "fullLock", "fobAction1", "relay1", "Lock" };
    for(const char* a : never)
    {
        TEST_ASSERT_TRUE_MESSAGE(redundantAction(a, st, maxSilence) == Skip::Send, a);
    }

    // Door sensor: closed or deactivated is fine; open/unknown/tampered sends.
    st.haveDoor = true;
    st.doorState = DOOR_CLOSED;
    TEST_ASSERT_TRUE(redundantAction("lock", st, maxSilence) == Skip::AlreadyLocked);
    st.doorState = DOOR_DEACTIVATED;
    TEST_ASSERT_TRUE(redundantAction("lock", st, maxSilence) == Skip::AlreadyLocked);
    const uint8_t sends[] = { 3, 4, 5, 16, 240, 255, 0 };
    for(uint8_t d : sends)
    {
        st.doorState = d;
        TEST_ASSERT_TRUE(redundantAction("lock", st, maxSilence) == Skip::Send);
    }
    st.haveDoor = false;

    st.lockState = LOCK_STATE_UNLOCKED;
    TEST_ASSERT_TRUE(redundantAction("unlock", st, maxSilence) == Skip::AlreadyUnlocked);
    TEST_ASSERT_TRUE(redundantAction("lock", st, maxSilence) == Skip::Send);
    const uint8_t transitional[] = { 0, 2, 4, 5, 6, 7, 254, 255 };
    for(uint8_t s : transitional)
    {
        st.lockState = s;
        TEST_ASSERT_TRUE(redundantAction("unlock", st, maxSilence) == Skip::Send);
        TEST_ASSERT_TRUE(redundantAction("lock", st, maxSilence) == Skip::Send);
    }

    // Without a live session, fresh contact, or a state newer than our last command: send.
    st.lockState = LOCK_STATE_LOCKED;
    LiveState x = st;
    x.sessionLive = false;
    TEST_ASSERT_TRUE(redundantAction("lock", x, maxSilence) == Skip::Send);
    x = st;
    x.haveState = false;
    TEST_ASSERT_TRUE(redundantAction("lock", x, maxSilence) == Skip::Send);
    x = st;
    x.stateAfterCommand = false;
    TEST_ASSERT_TRUE(redundantAction("lock", x, maxSilence) == Skip::Send);
    x = st;
    x.lastRxAgeMs = maxSilence + 1;
    TEST_ASSERT_TRUE(redundantAction("lock", x, maxSilence) == Skip::Send);
    x.lastRxAgeMs = maxSilence;
    TEST_ASSERT_TRUE(redundantAction("lock", x, maxSilence) == Skip::AlreadyLocked);
    TEST_ASSERT_TRUE(redundantAction(nullptr, x, maxSilence) == Skip::Send);
}

void test_redundant_action_grace()
{
    LiveState st = { true, true, true, LOCK_STATE_LOCKED, false, 0, 1000 };
    const int64_t maxSilence = 330000;
    const int64_t g = 60000;
    const int64_t now = 1000000;

    // No previous action, or grace off: skip as before.
    TEST_ASSERT_TRUE(redundantAction("lock", st, maxSilence, Grace{ false, 0, now, g }) == Skip::AlreadyLocked);
    TEST_ASSERT_TRUE(redundantAction("lock", st, maxSilence, Grace{ true, now - 1, now, 0 }) == Skip::AlreadyLocked);
    TEST_ASSERT_TRUE(redundantAction("lock", st, maxSilence, NO_GRACE) == Skip::AlreadyLocked);

    // Inside the window: sent (and says why); edges: 0 ms, g - 1 ms inside, g ms outside.
    TEST_ASSERT_TRUE(redundantAction("lock", st, maxSilence, Grace{ true, now - 12000, now, g }) == Skip::SendGrace);
    TEST_ASSERT_TRUE(redundantAction("lock", st, maxSilence, Grace{ true, now, now, g }) == Skip::SendGrace);
    TEST_ASSERT_TRUE(redundantAction("lock", st, maxSilence, Grace{ true, now - g + 1, now, g }) == Skip::SendGrace);
    TEST_ASSERT_TRUE(redundantAction("lock", st, maxSilence, Grace{ true, now - g, now, g }) == Skip::AlreadyLocked);
    TEST_ASSERT_TRUE(redundantAction("lock", st, maxSilence, Grace{ true, now - 10 * g, now, g }) == Skip::AlreadyLocked);

    // Clock edge cases: a previous action "in the future" counts as inside;
    // an action at boot (0) with now still small is inside; far apart is outside.
    TEST_ASSERT_TRUE(redundantAction("lock", st, maxSilence, Grace{ true, now + 5, now, g }) == Skip::SendGrace);
    TEST_ASSERT_TRUE(redundantAction("lock", st, maxSilence, Grace{ true, 0, 0, g }) == Skip::SendGrace);
    TEST_ASSERT_TRUE(redundantAction("lock", st, maxSilence, Grace{ true, 0, g - 1, g }) == Skip::SendGrace);
    TEST_ASSERT_TRUE(redundantAction("lock", st, maxSilence, Grace{ true, 0, INT64_MAX / 2, 600000 }) == Skip::AlreadyLocked);

    // Unlock the same way.
    st.lockState = LOCK_STATE_UNLOCKED;
    TEST_ASSERT_TRUE(redundantAction("unlock", st, maxSilence, Grace{ true, now - 1000, now, g }) == Skip::SendGrace);
    TEST_ASSERT_TRUE(redundantAction("unlock", st, maxSilence, Grace{ true, now - g, now, g }) == Skip::AlreadyUnlocked);

    // SendGrace only where a skip would have happened: otherwise plain Send.
    const Grace in = { true, now - 1000, now, g };
    TEST_ASSERT_TRUE(redundantAction("lock", st, maxSilence, in) == Skip::Send);
    TEST_ASSERT_TRUE(redundantAction("unlatch", st, maxSilence, in) == Skip::Send);
    TEST_ASSERT_TRUE(redundantAction(nullptr, st, maxSilence, in) == Skip::Send);
    LiveState x = st;
    x.sessionLive = false;
    TEST_ASSERT_TRUE(redundantAction("unlock", x, maxSilence, in) == Skip::Send);
    x = st;
    x.lastRxAgeMs = maxSilence + 1;
    TEST_ASSERT_TRUE(redundantAction("unlock", x, maxSilence, in) == Skip::Send);
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_remaining_length);
    RUN_TEST(test_connect_accepts_the_lock);
    RUN_TEST(test_connect_refusals);
    RUN_TEST(test_first_packet_must_be_connect);
    RUN_TEST(test_connect_timeout_and_keepalive);
    RUN_TEST(test_subscribe_lock_action);
    RUN_TEST(test_topic_matching);
    RUN_TEST(test_inbound_publish_qos);
    RUN_TEST(test_several_packets_and_split_reads);
    RUN_TEST(test_oversized_publish_is_acked_and_dropped);
    RUN_TEST(test_publish_lock_action_qos2);
    RUN_TEST(test_publish_lock_action_qos_capped);
    RUN_TEST(test_disconnect_and_second_connect);
    RUN_TEST(test_redundant_action_rule);
    RUN_TEST(test_redundant_action_grace);
    return UNITY_END();
}
