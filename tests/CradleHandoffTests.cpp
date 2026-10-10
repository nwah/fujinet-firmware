// lib/bus/rs232: the handoff between the RS232 bus and the HotSync service.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "bus/rs232/CradleHandoff.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

static long ms_since(Clock::time_point t)
{
    return (long)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count();
}

// Stands in for the bus: pumps the line every millisecond.
struct FakeBus
{
    CradleHandoff &h;
    std::mutex m;
    std::vector<uint8_t> sent;
    std::string log; // bytes as chars, 'B' for a baud switch
    std::vector<uint32_t> bauds;
    std::vector<uint8_t> palm; // bytes to feed in
    std::atomic<bool> stop{false};
    std::thread th;

    explicit FakeBus(CradleHandoff &hh) : h(hh) { th = std::thread([this] { run(); }); }
    ~FakeBus() { halt(); }

    void halt()
    {
        stop = true;
        if (th.joinable())
            th.join();
    }

    void feed(const std::vector<uint8_t> &b)
    {
        std::lock_guard<std::mutex> lk(m);
        palm.insert(palm.end(), b.begin(), b.end());
    }

    void run()
    {
        while (!stop)
        {
            {
                std::lock_guard<std::mutex> lk(m);
                uint8_t buf[64];
                size_t n;
                while ((n = h.outgoing(buf, sizeof buf)) > 0)
                {
                    sent.insert(sent.end(), buf, buf + n);
                    log.append((const char *)buf, n);
                }
                uint32_t baud;
                if (h.baud_wanted(baud))
                {
                    bauds.push_back(baud);
                    log.push_back('B');
                    h.baud_switched();
                }
                if (!palm.empty())
                {
                    h.received(palm.data(), palm.size());
                    palm.clear();
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
};

static const uint8_t ABC[] = {'a', 'b', 'c'};

TEST_CASE("idle handoff refuses everything")
{
    CradleHandoff h;
    uint8_t b[4];
    CHECK_FALSE(h.accept());
    CHECK(h.read(b, 4, 0) == -1);
    CHECK(h.write(ABC, 3) == -1);
    CHECK_FALSE(h.active());
    CHECK(h.start(nullptr, 0));
    CHECK_FALSE(h.start(nullptr, 0));
}

TEST_CASE("start, accept, read")
{
    CradleHandoff h;
    CHECK(h.start(ABC, 3));
    CHECK(h.active());
    CHECK_FALSE(h.accepted());
    CHECK(h.accept());
    CHECK_FALSE(h.accept());
    CHECK(h.accepted());
    uint8_t b[8];
    CHECK(h.read(b, 2, 0) == 2);
    CHECK(b[0] == 'a');
    CHECK(b[1] == 'b');
    CHECK(h.read(b, 8, 0) == 1);
    CHECK(b[0] == 'c');

    auto t = Clock::now();
    CHECK(h.read(b, 8, 50) == 0);
    long ms = ms_since(t);
    CHECK(ms >= 40);
    CHECK(ms < 500);
}

TEST_CASE("write reaches the bus in order")
{
    CradleHandoff h;
    h.start(nullptr, 0);
    h.accept();
    CHECK(h.write(ABC, 3) == 3);
    CHECK(h.write(ABC, 2) == 2);
    uint8_t b[16];
    CHECK(h.outgoing(b, 4) == 4);
    CHECK(h.outgoing(b + 4, 12) == 1);
    CHECK(std::string((char *)b, 5) == "abcab");
    CHECK(h.outgoing(b, 16) == 0);
}

TEST_CASE("bytes written before a rate change go out before the switch")
{
    CradleHandoff h;
    FakeBus bus(h);
    h.start(nullptr, 0);
    h.accept();
    const uint8_t A[] = {'A', 'A', 'A', 'A'};
    const uint8_t B[] = {'B', 'B'};
    h.write(A, 4);
    h.set_baud_rate(57600);
    h.write(B, 2);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    bus.halt();
    CHECK(bus.log == "AAAABBB");
    REQUIRE(bus.bauds.size() == 1);
    CHECK(bus.bauds[0] == 57600);
}

TEST_CASE("set_baud_rate gives up after a second with no bus")
{
    CradleHandoff h;
    h.start(nullptr, 0);
    h.accept();
    auto t = Clock::now();
    h.set_baud_rate(9600);
    long ms = ms_since(t);
    CHECK(ms >= 900);
    CHECK(ms < 1200 + 300);
}

TEST_CASE("bus feeds the service through received")
{
    CradleHandoff h;
    FakeBus bus(h);
    h.start(nullptr, 0);
    h.accept();
    bus.feed({'x', 'y'});
    uint8_t b[8];
    CHECK(h.read(b, 8, 500) >= 1);
    CHECK(b[0] == 'x');
}

TEST_CASE("finish keeps pending output, take_back ends everything")
{
    CradleHandoff h;
    h.start(nullptr, 0);
    h.accept();
    h.write(ABC, 3);
    h.finish();
    CHECK(h.finished());
    CHECK(h.accepted());
    CHECK(h.active());
    uint8_t b[8];
    CHECK(h.outgoing(b, 8) == 3);

    h.take_back();
    CHECK_FALSE(h.active());
    CHECK_FALSE(h.finished());
    CHECK(h.read(b, 8, 0) == -1);
    CHECK(h.write(ABC, 3) == -1);
}

TEST_CASE("take_back wakes a blocked read")
{
    CradleHandoff h;
    h.start(nullptr, 0);
    h.accept();
    std::atomic<int> result{99};
    long took = -1;
    std::thread t([&] {
        auto s = Clock::now();
        uint8_t rb[8];
        result = h.read(rb, 8, 5000);
        took = ms_since(s);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    h.take_back();
    t.join();
    CHECK(result == -1);
    CHECK(took < 1000);
}

TEST_CASE("finish releases a waiting set_baud_rate")
{
    CradleHandoff h;
    h.start(nullptr, 0);
    h.accept();
    auto s = Clock::now();
    std::thread t([&] { h.set_baud_rate(19200); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    h.finish();
    t.join();
    CHECK(ms_since(s) < 800);
}

TEST_CASE("received while idle is dropped")
{
    CradleHandoff h;
    h.received(ABC, 3);
    h.start(nullptr, 0);
    h.accept();
    uint8_t b[8];
    CHECK(h.read(b, 8, 20) == 0);
}
