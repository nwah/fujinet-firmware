#ifndef CRADLEHANDOFF_H
#define CRADLEHANDOFF_H

// How the bus hands a HotSync it found on its cradle line (PalmCradle.h) to
// the HotSync service, which runs on its own thread. The bus decides when a
// HotSync starts and ends, and keeps doing all the line I/O: it passes the
// Palm's bytes in here, sends what the service queued, and switches the rate
// when the service asks. The service never touches the line.
//
// A handoff is idle, then handed over (start), then finished by the service
// (finish), then taken back by the bus (take_back), and idle again.
// Standard library only, so it is unit-tested on the host.

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>

class CradleHandoff
{
public:
    // ---- HotSync service thread ----

    // True once the bus has handed over a HotSync that the service hasn't
    // accepted yet; the service then syncs and calls finish(). Doesn't block.
    bool accept();

    // Reads up to len of the Palm's bytes, waiting at most timeout_ms for the
    // first. Returns the count, 0 on timeout, or -1 when nothing is handed over.
    int read(uint8_t *buf, size_t len, uint32_t timeout_ms);

    // Queues bytes for the bus to send. Returns len, or -1 when nothing is
    // handed over.
    int write(const uint8_t *buf, size_t len);

    // Asks the bus to switch the line to baud once everything written so far
    // has gone out. Returns when the bus has done it, or after a second.
    void set_baud_rate(uint32_t baud);

    // The sync is over; the bus takes the line back.
    void finish();

    // ---- Bus ----

    // Hands over a HotSync, with the bytes of it the bus has already read
    // (len may be 0). Returns false if one is already handed over.
    bool start(const uint8_t *first, size_t len);

    // Handed over and not yet taken back.
    bool active();
    // The service has accepted the HotSync.
    bool accepted();
    // The service has called finish().
    bool finished();

    // Bytes from the Palm.
    void received(const uint8_t *buf, size_t len);
    // Takes up to max queued bytes to send; returns the count.
    size_t outgoing(uint8_t *buf, size_t max);

    // True, with the rate, when the service wants a rate change and every
    // byte it wrote before asking has been taken by outgoing().
    bool baud_wanted(uint32_t &baud);
    // The bus has switched (or, on a line with no rate, ignored the request).
    void baud_switched();

    // Ends the handoff, accepted or not. Pending bytes are dropped, and the
    // service's reads and writes return -1 until the next start().
    void take_back();

private:
    enum class State { IDLE, HANDED_OVER, ACCEPTED, FINISHED };

    std::mutex _lock;
    std::condition_variable _changed;
    State _state = State::IDLE;
    std::deque<uint8_t> _in, _out;
    bool _baud_pending = false;
    uint32_t _baud = 0;
    // Bytes queued before the rate change; the bus sends these first.
    size_t _out_before_baud = 0;
};

#endif /* CRADLEHANDOFF_H */
