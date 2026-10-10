#ifndef HOTSYNC_SHARED_CRADLE_H
#define HOTSYNC_SHARED_CRADLE_H

#include "HotSyncLink.h"
#include "global_types.h"

// A cradle on a serial line that the platform bus owns. The bus lends the line
// to the cradle for a HotSync window and does the I/O; between windows it
// serves FujiBus, so a Palm app reaches FujiNet as any bus host does.
class HotSyncSharedCradle : public HotSyncLink
{
public:
    // True while a host is talking FujiBus. A Palm running an app cannot
    // HotSync, so the cradle leaves the line alone.
    virtual bool host_active() = 0;

    // Waits until the bus has lent the line at CMP_INITIAL_BAUD_RATE.
    virtual success_is_true claim() = 0;
    // Waits until the bus has the line back at its own rate.
    virtual void release() = 0;
};

// Adapts a bus's cradle class (rs232HotSync on RS232) to HotSyncSharedCradle,
// so lib/hotsync never depends on a bus. Line provides host_active(), claim()
// returning bool, release(), read(), write() and set_baud_rate().
template <class Line>
class HotSyncBusCradle : public HotSyncSharedCradle
{
public:
    explicit HotSyncBusCradle(Line &line) : _line(line) {}

    bool host_active() override { return _line.host_active(); }
    success_is_true claim() override { RETURN_SUCCESS_IF(_line.claim()); }
    void release() override { _line.release(); }

    int read(uint8_t *buf, size_t len, uint32_t timeout_ms) override
    {
        return _line.read(buf, len, timeout_ms);
    }
    int write(const uint8_t *buf, size_t len) override { return _line.write(buf, len); }
    void set_baud_rate(uint32_t baud) override { _line.set_baud_rate(baud); }

private:
    Line &_line;
};

#endif // HOTSYNC_SHARED_CRADLE_H
