#ifndef PALMCRADLE_H
#define PALMCRADLE_H

// A Palm handheld's cradle, as the bus line FujiNet reaches the Palm on. The
// line carries FujiBus while a Palm app talks to FujiNet, and HotSync while
// the Palm's HotSync app runs. The bus that owns the line decides which, and
// does all the I/O; a cradle only tells the bus what it knows about the Palm.
//
// Every channel a Palm can sit on implements this beside IOChannel:
// UARTCradle for a serial cradle, PalmUSBChannel for a USB one.

#include <stdint.h>

// A serial HotSync always opens at this rate (CMP), whatever the bus runs at.
#define PALM_HOTSYNC_SERIAL_BAUD 9600

class PalmCradle
{
public:
    // What the Palm has the line open for.
    enum class PalmUse
    {
        NONE,    // no Palm, or nothing open on it
        APP,     // a Palm app, which talks FujiBus
        HOTSYNC, // the HotSync app
        UNKNOWN, // something may be open, and the cradle can't tell what
    };

    virtual ~PalmCradle() = default;

    virtual PalmUse palmUse() = 0;

    // The rate a HotSync opens at, or 0 when the line has no rate (USB). When
    // it differs from the bus rate, the bus has to switch to it to hear a
    // HotSync start.
    virtual uint32_t hotsyncBaudrate() = 0;
};

#endif /* PALMCRADLE_H */
