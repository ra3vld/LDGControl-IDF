#include "controller.hpp"
#include "image_stream.hpp"
#include <unity.h>
#include <vector>
#include <algorithm>
#include <limits>
using namespace ldg;
void setUp() {}
void tearDown() {}
struct Peer {
    std::vector<uint8_t> writes;
    uint8_t antenna = 1;
    bool acknowledge = true;
    static bool write(void *p, uint8_t b) {
        static_cast<Peer *>(p)->writes.push_back(b);
        return true;
    }
};
static void cycle(Protocol &p, Peer &peer, uint32_t &now, unsigned duration) {
    size_t cursor = peer.writes.size();
    for (unsigned i = 0; i < duration; ++i) {
        p.tick(++now);
        while (cursor < peer.writes.size()) {
            uint8_t c = peer.writes[cursor++];
            if (!peer.acknowledge)
                continue;
            if (c == 'Z') {
                const char *a = "000000000000000AzAz";
                p.feed((const uint8_t *)a, strlen(a), now);
            } else if (c == 'A') {
                peer.antenna = 3 - peer.antenna;
                uint8_t b = '0' + peer.antenna;
                p.feed(&b, 1, now);
            } else if (c == 'T' || c == 'F' || c == 'M' || c == 'P' || c == 'C') {
                uint8_t b = c == 'C' ? 'A' : c == 'F' ? 'T' : c;
                p.feed(&b, 1, now);
            }
        }
    }
}
void command_result_is_not_acceptance() {
    Peer peer;
    Protocol p({&peer, Peer::write});
    uint32_t now = 0;
    TEST_ASSERT_TRUE(p.start('T', now));
    Result r;
    TEST_ASSERT_FALSE(p.takeResult(r));
    cycle(p, peer, now, 1000);
    TEST_ASSERT_TRUE(p.takeResult(r));
    TEST_ASSERT_EQUAL(int(Result::Success), int(r));
    TEST_ASSERT_EQUAL('X', peer.writes[2]);
    TEST_ASSERT_EQUAL('S', peer.writes.back());
}
void sync_required_after_timeout() {
    Peer peer;
    Protocol p({&peer, Peer::write});
    uint32_t now = 0;
    peer.acknowledge = false;
    p.start('M', now);
    cycle(p, peer, now, 3000);
    Result r;
    TEST_ASSERT_TRUE(p.takeResult(r));
    TEST_ASSERT_EQUAL(int(Result::Timeout), int(r));
    peer.writes.clear();
    peer.acknowledge = true;
    p.start('P', now);
    cycle(p, peer, now, 1500);
    TEST_ASSERT_TRUE(p.takeResult(r));
    TEST_ASSERT_EQUAL(int(Result::Success), int(r));
    auto z = std::find(peer.writes.begin(), peer.writes.end(), 'Z');
    auto b = std::find(peer.writes.begin(), peer.writes.end(), 'P');
    TEST_ASSERT_TRUE(z < b);
}
void full_sync_no_partial_match() {
    Peer peer;
    Protocol p({&peer, Peer::write});
    uint32_t now = 0;
    peer.acknowledge = false;
    p.start('M', now);
    cycle(p, peer, now, 3000);
    Result r;
    p.takeResult(r);
    p.start('P', now);
    cycle(p, peer, now, 260);
    const char *partial = "000AzAz";
    p.feed((const uint8_t *)partial, strlen(partial), now);
    cycle(p, peer, now, 2600);
    TEST_ASSERT_TRUE(p.takeResult(r));
    TEST_ASSERT_EQUAL(int(Result::Timeout), int(r));
    TEST_ASSERT_TRUE(std::find(peer.writes.begin(), peer.writes.end(), 'P') == peer.writes.end());
}
void meter_null_swr_and_fragmentation() {
    Peer peer;
    Protocol p({&peer, Peer::write});
    uint8_t packet[] = {0, 0, 0, 0, 9, 0, ';', ';'};
    p.feed(packet, 3, 4);
    p.feed(packet + 3, 5, 5);
    TEST_ASSERT_TRUE(p.state(5).meterValid);
    TEST_ASSERT_TRUE(std::isnan(p.state(5).meter.swr));
    TEST_ASSERT_FALSE(p.state(3006).meterValid);
    p.fault();
    TEST_ASSERT_FALSE(p.state(6).meterValid);
}
void operations_deduplicate_bound_lifetime() {
    Operations ops;
    Id target{};
    target[0] = 9;
    Request req{};
    req.id[0] = 1;
    req.target = target;
    req.command = 'A';
    req.expires = 2000;
    unsigned index = 99;
    TEST_ASSERT_EQUAL(int(Result::Success), int(ops.accept(req, target, 0, index)));
    TEST_ASSERT_EQUAL(0, index);
    ops.at(index).state = OperationState::Running;
    TEST_ASSERT_EQUAL(int(Result::Success), int(ops.accept(req, target, 3000, index)));
    req.command = 'T';
    TEST_ASSERT_EQUAL(int(Result::DuplicateConflict), int(ops.accept(req, target, 1, index)));
    req.command = 'A';
    for (unsigned i = 2; i <= 16; ++i) {
        req.id[0] = i;
        TEST_ASSERT_EQUAL(int(Result::Success), int(ops.accept(req, target, 0, index)));
    }
    req.id[0] = 17;
    TEST_ASSERT_EQUAL(int(Result::Busy), int(ops.accept(req, target, 0, index)));
    ops.finish(0, Result::Success, 1);
    TEST_ASSERT_EQUAL(int(Result::Busy), int(ops.accept(req, target, 1, index)));
    req.expires = 62000;
    TEST_ASSERT_EQUAL(int(Result::Success), int(ops.accept(req, target, 60001, index)));
}
void operation_epoch_and_rollover() {
    Operations ops;
    Id target{};
    Request r{};
    r.command = 'M';
    r.target = target;
    r.expires = 99;
    unsigned index;
    TEST_ASSERT_EQUAL(int(Result::Success), int(ops.accept(r, target, UINT32_MAX - 100, index)));
    target[0] = 1;
    TEST_ASSERT_EQUAL(-1, ops.next(target, 1));
    TEST_ASSERT_EQUAL(int(Result::TargetChanged), int(ops.at(index).result));
}
void external_zero_and_manual_policy() {
    RoutePlanner p;
    RouteSettings cfg{};
    cfg.enabled = true;
    cfg.bands[BAND_40M].external = 3;
    tuner_state_t s{};
    s.meter.band = BAND_40M;
    s.meter.forward_power_raw = 100;
    s.meter.forward_power_watts = 30;
    s.meterAgeMs = 0;
    for (unsigned i = 0; i < 3; ++i)
        p.observe(s, i * 100, true);
    TEST_ASSERT_EQUAL(0, p.poll(s, cfg, {}));
    s.meter.forward_power_raw = 0;
    s.meter.forward_power_watts = 0;
    s.meterAgeMs = 600;
    TEST_ASSERT_EQUAL(0, p.poll(s, cfg, {}));
    s.meterAgeMs = 0;
    TEST_ASSERT_EQUAL(3, p.poll(s, cfg, {}));
    TEST_ASSERT_EQUAL(0, p.poll(s, cfg, {}));
    p.manual();
    TEST_ASSERT_EQUAL(0, p.poll(s, cfg, {}));
}
void image_header_accepts_every_fragment_boundary() {
    uint8_t input[4096];
    for (size_t i = 0; i < sizeof(input); ++i)
        input[i] = uint8_t(i);
    for (size_t split = 1; split < 40; ++split) {
        ImagePrefix<24> prefix;
        std::vector<uint8_t> output;
        size_t at = 0;
        while (at < sizeof(input)) {
            size_t length = std::min(split, sizeof(input) - at), take = 0;
            if (!prefix.complete()) {
                take = prefix.append(input + at, length);
                if (prefix.complete())
                    output.insert(output.end(), prefix.data(), prefix.data() + 24);
            }
            output.insert(output.end(), input + at + take, input + at + length);
            at += length;
        }
        TEST_ASSERT_EQUAL(sizeof(input), output.size());
        TEST_ASSERT_EQUAL_UINT8_ARRAY(input, output.data(), sizeof(input));
    }
}
void content_ranges_reject_wrong_units_overflow_and_incomplete_values() {
    auto r = parseContentRange("bytes 4096-8191/12345");
    TEST_ASSERT_TRUE(r.valid);
    TEST_ASSERT_EQUAL(4096, r.first);
    TEST_ASSERT_EQUAL(8191, r.last);
    TEST_ASSERT_EQUAL(12345, r.total);
    for (const char *s :
         {"bytes 0-1/*", "bytes 0-4096/4096", "bytes 4096-0/8192", "bytes 0-4294967296/8192",
          "bytes 0-1/2 garbage", "bytes 0-1", "items 0-1/2", "bytes 0-1/0"})
        TEST_ASSERT_FALSE(parseContentRange(s).valid);
}
void operation_retention_uses_local_clock_for_remote_commands() {
    Operations ops;
    Id target{};
    Request req;
    req.target = target;
    req.expires = 100100;
    unsigned index;
    for (unsigned i = 0; i < 16; ++i) {
        req.id[0] = i;
        TEST_ASSERT_EQUAL(int(Result::Success), int(ops.accept(req, target, 100000, index, 500)));
        ops.finish(index, Result::Success, 500);
    }
    req.id[0] = 16;
    req.expires = 101000;
    TEST_ASSERT_EQUAL(int(Result::Busy), int(ops.accept(req, target, 100000, index, 501)));
    req.expires = 161000;
    TEST_ASSERT_EQUAL(int(Result::Success), int(ops.accept(req, target, 160000, index, 60500)));
}
int main() {
    UNITY_BEGIN();
    RUN_TEST(operation_retention_uses_local_clock_for_remote_commands);
    RUN_TEST(image_header_accepts_every_fragment_boundary);
    RUN_TEST(content_ranges_reject_wrong_units_overflow_and_incomplete_values);
    RUN_TEST(command_result_is_not_acceptance);
    RUN_TEST(sync_required_after_timeout);
    RUN_TEST(full_sync_no_partial_match);
    RUN_TEST(meter_null_swr_and_fragmentation);
    RUN_TEST(operations_deduplicate_bound_lifetime);
    RUN_TEST(operation_epoch_and_rollover);
    RUN_TEST(external_zero_and_manual_policy);
    return UNITY_END();
}
