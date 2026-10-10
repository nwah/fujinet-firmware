#ifndef RS232HOTSYNC_H
#define RS232HOTSYNC_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>

// A Palm cradle on the RS232 bus line. The HotSync side (its own thread) sees
// a byte link it can claim and release; systemBus::service() does every read,
// write and baud change on the port while the line is lent.
class rs232HotSync
{
public:
    using clock = std::chrono::steady_clock;
    static constexpr uint32_t CMP_BAUD = 9600; // every HotSync opens here

    // turn: how long a HotSync-side call waits for the bus to act (default 5 s:
    // the bus serves the line once per main-loop pass, and a 1 KB PADP fragment
    // takes over a second at 9600 baud). quiet: how long without FujiBus before
    // the host counts as gone (default 60 s; Palm apps pause between requests).
    explicit rs232HotSync(std::chrono::milliseconds turn = std::chrono::milliseconds(5000),
                          std::chrono::milliseconds quiet = std::chrono::milliseconds(60000));

    // ---- HotSync thread ----
    // False while a host is talking FujiBus. Stray non-FujiBus bytes with no
    // packets since the last call mean the device left its app to HotSync, so
    // they end the quiet period at once.
    bool host_active() { return host_active_at(clock::now()); }
    bool host_active_at(clock::time_point now);
    // Waits until the bus has lent the line at CMP_BAUD. False on timeout (then call release()).
    bool claim();
    // Waits until the bus has the line back at its own rate; drops buffered bytes.
    void release();
    // Up to len bytes, waiting at most timeout_ms for the first; count, 0 on
    // timeout, -1 if the line is not lent.
    int read(uint8_t *buf, size_t len, uint32_t timeout_ms);
    // Returns once the bytes are on the wire (as a UART write+flush would): len,
    // 0 if the bus did not send them within the turn, -1 if the line is not lent.
    int write(const uint8_t *buf, size_t len);
    // Waits until the bus has switched (or the turn runs out).
    void set_baud_rate(uint32_t baud);

    // ---- systemBus::service() only ----
    void bus_packet_handled() { ++_packets; }
    void bus_stray_bytes(size_t count) { _stray += count; }
    // The baud rate to lend the line at, or 0 to keep it on FujiBus.
    uint32_t line_wanted();
    void line_received(const uint8_t *buf, size_t len);
    size_t line_outgoing(uint8_t *buf, size_t len);
    // After a pass with everything outgoing sent; baud 0 once it is FujiBus again.
    void line_served(uint32_t baud);

private:
    const std::chrono::milliseconds _turn;
    const std::chrono::milliseconds _quiet;

    std::atomic<size_t> _packets{0};
    std::atomic<size_t> _stray{0};
    size_t _seen_packets = 0;
    size_t _seen_stray = 0;
    clock::time_point _last_packet;

    std::mutex _lock;
    std::condition_variable _changed;
    uint32_t _wanted_baud = 0;
    uint32_t _served_baud = 0;
    bool _sending = false;
    std::deque<uint8_t> _in;
    std::deque<uint8_t> _out;
};

#endif /* RS232HOTSYNC_H */
