#include "controller.hpp"
#include <unity.h>
#include <vector>
#include <algorithm>
using namespace ldg;
void setUp() {}
void tearDown() {}
struct Rig {
    std::vector<uint8_t> sent;
    unsigned cursor = 0;
    uint32_t now = 0;
    uint16_t power = 0, ref = 0, band = 2300;
    bool streaming = true, rfAfterFirstToggle = false, ack = true;
    uint8_t antenna = 1;
    unsigned toggles = 0;
    Operations ops;
    Controller c;
    Rig()
        : c({this,
             [](void *p, uint8_t b) {
                 static_cast<Rig *>(p)->sent.push_back(b);
                 return true;
             }},
            {}, ops) {}
    void meter() {
        uint8_t b[] = {uint8_t(power >> 8),
                       uint8_t(power),
                       uint8_t(ref >> 8),
                       uint8_t(ref),
                       uint8_t(band >> 8),
                       uint8_t(band),
                       ';',
                       ';'};
        c.feed(b, sizeof(b), now);
    }
    void run(unsigned ms) {
        for (unsigned i = 0; i < ms; ++i) {
            ++now;
            if (streaming && now % 20 == 0)
                meter();
            c.tick(now);
            while (cursor < sent.size()) {
                auto cmd = sent[cursor++];
                uint8_t a = 0;
                if (cmd == 'X')
                    streaming = false;
                if (cmd == 'S')
                    streaming = true;
                if (!ack)
                    continue;
                if (cmd == 'Z') {
                    const char *sync = "000000000000000AzAz";
                    c.feed((const uint8_t *)sync, strlen(sync), now);
                }
                if (cmd == 'A') {
                    antenna = 3 - antenna;
                    a = '0' + antenna;
                    ++toggles;
                    if (rfAfterFirstToggle && toggles == 1)
                        power = 100;
                }
                if (cmd == 'M' || cmd == 'T' || cmd == 'F' || cmd == 'P' || cmd == 'C')
                    a = cmd == 'C' ? 'A' : cmd == 'F' ? 'T' : cmd;
                if (a)
                    c.feed(&a, 1, now);
            }
        }
    }
    Id request(uint8_t cmd) {
        Request r;
        r.id[0] = uint8_t(now);
        r.id[1] = uint8_t(now >> 8);
        r.command = cmd;
        r.target = c.target();
        r.expires = now + 2000;
        unsigned i;
        TEST_ASSERT_EQUAL(int(Result::Success), int(c.submit(r, now, i)));
        return r.id;
    }
};
void startup_restores_original_port() {
    Rig r;
    r.run(2000);
    TEST_ASSERT_EQUAL(2, r.toggles);
    TEST_ASSERT_EQUAL(1, r.antenna);
    TEST_ASSERT_EQUAL(int(AntennaInit::Ready), int(r.c.state(r.now).antennaInit));
    r.run(5000);
    TEST_ASSERT_EQUAL(2, r.toggles);
}
void rf_interrupts_probe_then_zero_restores() {
    Rig r;
    r.rfAfterFirstToggle = true;
    r.run(2500);
    TEST_ASSERT_EQUAL(1, r.toggles);
    TEST_ASSERT_EQUAL(int(AntennaInit::Restoring), int(r.c.state(r.now).antennaInit));
    r.power = 0;
    r.run(2000);
    TEST_ASSERT_EQUAL(2, r.toggles);
    TEST_ASSERT_EQUAL(1, r.antenna);
    TEST_ASSERT_EQUAL(int(AntennaInit::Ready), int(r.c.state(r.now).antennaInit));
}
void auto_port_change_has_manual_guard_tune_and_restore() {
    Rig r;
    AntennaSettings a{};
    a.enabled = true;
    a.bands[BAND_20M] = ANT_B;
    r.c.configure(a, {}, {}, 13.8);
    r.run(2000);
    r.power = 100;
    r.run(200);
    r.request('C');
    r.run(1200);
    r.sent.clear();
    r.cursor = 0;
    r.band = 1160;
    r.run(5000);
    std::vector<uint8_t> actions;
    for (auto b : r.sent)
        if (b == 'M' || b == 'A' || b == 'T' || b == 'C')
            actions.push_back(b);
    const uint8_t expected[] = {'M', 'A', 'T', 'C'};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, actions.data(), 4);
    TEST_ASSERT_EQUAL(4, actions.size());
    TEST_ASSERT_EQUAL(2, r.antenna);
    TEST_ASSERT_EQUAL(MODE_AUTO, r.c.state(r.now).mode);
}
void queued_cancel_and_running_cancel_are_distinct() {
    Rig r;
    r.run(2000);
    auto id = r.request('T');
    TEST_ASSERT_TRUE(r.c.cancel(id, r.now));
    r.run(1000);
    TEST_ASSERT_EQUAL(0, std::count(r.sent.begin(), r.sent.end(), 'T'));
    id = r.request('T');
    r.run(300);
    TEST_ASSERT_FALSE(r.c.cancel(id, r.now));
    r.run(1000);
    TEST_ASSERT_EQUAL(1, std::count(r.sent.begin(), r.sent.end(), 'T'));
    TEST_ASSERT_EQUAL(int(Result::Success), int(r.c.operations().find(id)->result));
}
void external_requires_zero_and_reports_ack_only() {
    Rig r;
    SimulatedSwitch sw;
    r.c.switchDriver(&sw);
    r.run(2000);
    Request q;
    q.id[0] = 7;
    q.target = r.c.target();
    q.command = SelectExternal;
    q.port = 3;
    q.expires = r.now + 2000;
    unsigned index;
    TEST_ASSERT_EQUAL(int(Result::Success), int(r.c.submit(q, r.now, index)));
    r.run(100);
    TEST_ASSERT_EQUAL(3, sw.state().confirmed);
    TEST_ASSERT_EQUAL(int(Confirmation::Acknowledged), int(sw.state().evidence));
    r.power = 100;
    r.run(100);
    q.id[0] = 8;
    q.port = 4;
    q.expires = r.now + 2000;
    TEST_ASSERT_EQUAL(int(Result::Success), int(r.c.submit(q, r.now, index)));
    r.run(100);
    TEST_ASSERT_EQUAL(int(Result::Unsafe), int(r.c.operations().at(index).result));
    TEST_ASSERT_EQUAL(3, sw.state().confirmed);
}
void route_failure_is_partial_and_does_not_switch_back() {
    Rig r;
    SimulatedSwitch sw;
    sw.fail(true);
    r.c.switchDriver(&sw);
    AntennaSettings a{};
    a.enabled = true;
    a.bands[BAND_40M] = ANT_A;
    RouteSettings route{};
    route.enabled = true;
    route.bands[BAND_40M] = {ANT_A, 3};
    r.c.configure(a, {}, route, 13.8);
    r.run(2000);
    r.power = 100;
    r.run(200);
    TEST_ASSERT_EQUAL(0, sw.state().requested);
    r.power = 0;
    r.run(100);
    auto report = r.c.routeReport(r.now);
    TEST_ASSERT_TRUE(report.partial);
    TEST_ASSERT_EQUAL(int(Result::Failed), int(report.result));
    TEST_ASSERT_EQUAL(1, r.antenna);
    r.run(3000);
    TEST_ASSERT_FALSE(sw.state().busy);
    TEST_ASSERT_EQUAL(int(Result::Failed), int(sw.state().result));
    TEST_ASSERT_EQUAL(1, r.antenna);
}
int main() {
    UNITY_BEGIN();
    RUN_TEST(route_failure_is_partial_and_does_not_switch_back);
    RUN_TEST(startup_restores_original_port);
    RUN_TEST(rf_interrupts_probe_then_zero_restores);
    RUN_TEST(auto_port_change_has_manual_guard_tune_and_restore);
    RUN_TEST(queued_cancel_and_running_cancel_are_distinct);
    RUN_TEST(external_requires_zero_and_reports_ack_only);
    return UNITY_END();
}
