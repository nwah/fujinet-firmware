#ifndef HOTSYNC_BUS_CRADLE_H
#define HOTSYNC_BUS_CRADLE_H

#include "HotSyncLink.h"

// A cradle on a line the platform bus owns. The bus serves FujiBus on it and
// watches for a Palm starting a HotSync; when one does, the bus hands it over
// and this link carries it, the bus still doing the line I/O. The service
// syncs, then calls finish() and the bus takes the line back.
class HotSyncBusCradle : public HotSyncLink
{
public:
    // True when the bus has handed over a HotSync. Doesn't block.
    virtual bool accept() = 0;
    virtual void finish() = 0;
};

// Adapts a bus's handoff class (CradleHandoff on RS232), so lib/hotsync
// depends on no bus. Handoff provides accept(), finish(), read(), write() and
// set_baud_rate() with HotSyncLink's meanings.
template <class Handoff>
class HotSyncBusCradleOf : public HotSyncBusCradle
{
public:
    explicit HotSyncBusCradleOf(Handoff &handoff) : _handoff(handoff) {}

    bool accept() override { return _handoff.accept(); }
    void finish() override { _handoff.finish(); }

    int read(uint8_t *buf, size_t len, uint32_t timeout_ms) override
    {
        return _handoff.read(buf, len, timeout_ms);
    }
    int write(const uint8_t *buf, size_t len) override { return _handoff.write(buf, len); }
    void set_baud_rate(uint32_t baud) override { _handoff.set_baud_rate(baud); }

private:
    Handoff &_handoff;
};

#endif // HOTSYNC_BUS_CRADLE_H
