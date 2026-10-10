#ifndef UARTCRADLE_H
#define UARTCRADLE_H

// A serial cradle (or a serial cable to one) on a UART. The Palm opens its
// serial port the same way for an app and for HotSync, so this cradle can't
// tell the two apart; the bus listens for a HotSync at its opening rate.

#include "UARTChannel.h"
#include "PalmCradle.h"

class UARTCradle : public UARTChannel, public PalmCradle
{
public:
    PalmUse palmUse() override { return PalmUse::UNKNOWN; }
    uint32_t hotsyncBaudrate() override { return PALM_HOTSYNC_SERIAL_BAUD; }
};

#endif /* UARTCRADLE_H */
