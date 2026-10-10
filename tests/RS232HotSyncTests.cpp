#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "device/rs232/rs232HotSync.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

using ms = std::chrono::milliseconds;

namespace {

// Plays systemBus::_rs232_lend_line() against an in-memory port, once per
// "main-loop pass" (1 ms).
class FakeBus
{
public:
    explicit FakeBus(rs232HotSync &cradle) : _cradle(cradle), _thread([this] { run(); }) {}
    ~FakeBus()
    {
        _stop = true;
        _thread.join();
    }

    void feed(const std::vector<uint8_t> &bytes)
    {
        std::lock_guard<std::mutex> g(_m);
        _rx.insert(_rx.end(), bytes.begin(), bytes.end());
    }
    std::vector<uint8_t> sent()
    {
        std::lock_guard<std::mutex> g(_m);
        return _tx;
    }
    uint32_t baud()
    {
        std::lock_guard<std::mutex> g(_m);
        return _baud;
    }

private:
    void run()
    {
        uint32_t lent = 0;
        while (!_stop)
        {
            uint32_t want = _cradle.line_wanted();
            if (want == 0)
            {
                if (lent != 0)
                {
                    std::lock_guard<std::mutex> g(_m);
                    _rx.clear(); // bounded drain of noise at the cradle's rate
                    _baud = 0;
                    lent = 0;
                    _cradle.line_served(0);
                }
            }
            else
            {
                if (want != lent)
                {
                    lent = want;
                    std::lock_guard<std::mutex> g(_m);
                    _baud = want;
                }
                std::vector<uint8_t> in;
                {
                    std::lock_guard<std::mutex> g(_m);
                    in.swap(_rx);
                }
                if (!in.empty())
                    _cradle.line_received(in.data(), in.size());
                uint8_t buf[64];
                while (size_t n = _cradle.line_outgoing(buf, sizeof(buf)))
                {
                    std::lock_guard<std::mutex> g(_m);
                    _tx.insert(_tx.end(), buf, buf + n);
                }
                _cradle.line_served(lent);
            }
            std::this_thread::sleep_for(ms(1));
        }
    }

    rs232HotSync &_cradle;
    std::atomic<bool> _stop{false};
    std::mutex _m;
    std::vector<uint8_t> _rx, _tx;
    uint32_t _baud = 0;
    std::thread _thread;
};

} // namespace

TEST_CASE("host_active_at follows packets and stray bytes")
{
    rs232HotSync c(ms(200), ms(1000));
    auto t0 = rs232HotSync::clock::now();

    SUBCASE("active after construction")
    {
        CHECK(c.host_active_at(t0));
    }
    SUBCASE("inactive once quiet has passed with no packets")
    {
        CHECK(c.host_active_at(t0));
        CHECK_FALSE(c.host_active_at(t0 + ms(1500)));
    }
    SUBCASE("a packet re-activates")
    {
        CHECK_FALSE(c.host_active_at(t0 + ms(1500)));
        c.bus_packet_handled();
        CHECK(c.host_active_at(t0 + ms(1600)));
        CHECK(c.host_active_at(t0 + ms(2000)));
        CHECK_FALSE(c.host_active_at(t0 + ms(2700)));
    }
    SUBCASE("stray bytes with no packet end the quiet period at once")
    {
        c.bus_stray_bytes(5);
        CHECK_FALSE(c.host_active_at(t0 + ms(10)));
    }
    SUBCASE("stray bytes alongside a packet stay active")
    {
        c.bus_stray_bytes(5);
        c.bus_packet_handled();
        CHECK(c.host_active_at(t0 + ms(10)));
    }
}

TEST_CASE("claim")
{
    rs232HotSync c(ms(200));
    SUBCASE("succeeds once the bus serves CMP_BAUD")
    {
        FakeBus bus(c);
        CHECK(c.claim());
        CHECK(bus.baud() == rs232HotSync::CMP_BAUD);
        c.release();
    }
    SUBCASE("fails when nothing serves the line")
    {
        auto start = std::chrono::steady_clock::now();
        CHECK_FALSE(c.claim());
        CHECK(std::chrono::steady_clock::now() - start >= ms(190));
        c.release();
    }
}

TEST_CASE("read and write")
{
    rs232HotSync c(ms(200));
    uint8_t buf[16];

    SUBCASE("not lent returns -1")
    {
        CHECK(c.read(buf, sizeof(buf), 10) == -1);
        CHECK(c.write(buf, 3) == -1);
    }

    FakeBus bus(c);
    REQUIRE(c.claim());

    SUBCASE("read returns bytes the bus delivered")
    {
        bus.feed({1, 2, 3});
        int n = c.read(buf, sizeof(buf), 100);
        REQUIRE(n == 3);
        CHECK(buf[0] == 1);
        CHECK(buf[2] == 3);
    }
    SUBCASE("read honours len and keeps the rest")
    {
        bus.feed({1, 2, 3, 4});
        std::this_thread::sleep_for(ms(20));
        CHECK(c.read(buf, 2, 100) == 2);
        CHECK(c.read(buf, sizeof(buf), 100) == 2);
        CHECK(buf[0] == 3);
    }
    SUBCASE("read times out with 0")
    {
        CHECK(c.read(buf, sizeof(buf), 20) == 0);
    }
    SUBCASE("write returns after the bus has sent the bytes, in order")
    {
        std::vector<uint8_t> data(150);
        for (size_t i = 0; i < data.size(); ++i)
            data[i] = static_cast<uint8_t>(i);
        CHECK(c.write(data.data(), data.size()) == 150);
        CHECK(bus.sent() == data);
        CHECK(c.write(data.data(), 1) == 1);
        CHECK(bus.sent().size() == 151);
    }
    c.release();
}

TEST_CASE("write gives up if the bus never sends")
{
    rs232HotSync c(ms(100));
    // Lent by hand, but nobody calls line_outgoing.
    std::thread t([&] { c.claim(); });
    std::this_thread::sleep_for(ms(10));
    c.line_served(rs232HotSync::CMP_BAUD);
    t.join();
    uint8_t b = 7;
    CHECK(c.write(&b, 1) == 0);
}

TEST_CASE("set_baud_rate waits for the bus")
{
    rs232HotSync c(ms(200));
    FakeBus bus(c);
    REQUIRE(c.claim());
    c.set_baud_rate(57600);
    CHECK(bus.baud() == 57600);
    c.release();
}

TEST_CASE("release waits for the bus and leaves no stale input")
{
    rs232HotSync c(ms(200));
    FakeBus bus(c);
    REQUIRE(c.claim());
    bus.feed({9, 9, 9});
    std::this_thread::sleep_for(ms(20));
    c.release();
    CHECK(bus.baud() == 0);
    uint8_t buf[8];
    CHECK(c.read(buf, sizeof(buf), 10) == -1);
    REQUIRE(c.claim());
    CHECK(c.read(buf, sizeof(buf), 20) == 0);
    c.release();
}
