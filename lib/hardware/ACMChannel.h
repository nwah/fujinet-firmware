#ifndef ACMCHANNEL_H
#define ACMCHANNEL_H

#ifdef CONFIG_USB_CDC_ACM_HOST_ENABLED

#include "IOChannel.h"
#include "ESP32UARTChannel.h"
#include "RS232ChannelProtocol.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <usb/cdc_acm_host.h>

// struct ChannelConfig
// {
//     std::string device;
//     int baud_rate;
//     uint32_t read_timeout_ms = IOCHANNEL_DEFAULT_TIMEOUT;
//     uint32_t discard_timeout_ms = IOCHANNEL_DEFAULT_TIMEOUT;

//     ChannelConfig& baud(int baud) {
//         baud_rate = baud; return *this;
//     }
//     ChannelConfig& deviceID(std::string path) {
//         device = path; return *this;
//     }
//     ChannelConfig& readTimeout(uint32_t millis) {
//         read_timeout_ms = millis; return *this;
//     }
//     ChannelConfig& discardTimeout(uint32_t millis) {
//         discard_timeout_ms = millis; return *this;
//     }
// };

class ACMChannel : public IOChannel, public RS232ChannelProtocol
{
private:
    SemaphoreHandle_t device_disconnected_sem;
    cdc_acm_dev_hdl_t cdc_dev = NULL;
    QueueHandle_t rxQueue;

protected:
    bool getPin(int pin);
    void setPin(int pin, bool state);

    void updateFIFO() override;
    size_t dataOut(const void *buffer, size_t length) override;

public:
    void begin(const ChannelConfig& conf);
    void end() override;

    void flushOutput() override;

    uint32_t getBaudrate() override { return 0; }
    void setBaudrate(uint32_t baud) override { return; }

    // FujiNet acts as modem (DCE), computer serial ports are DTE.
    // API names follow the modem (DCE) view, but the actual RS-232 pin differs.
    bool getDTR() override;               // modem DTR input  → reads RS-232 DTR pin
    void setDSR(bool state) override;     // modem DSR output → drives RS-232 DSR pin
    bool getRTS() override;               // modem RTS input  → reads RS-232 RTS pin
    void setCTS(bool state) override;     // modem CTS output → drives RS-232 CTS pin
    void setDCD(bool state) override;     // modem DCD output → drives RS-232 DCD pin
    void setRI(bool state) override;      // modem RI output  → drives RS-232 RI pin

    bool getDCD() override { return 0; }; // DCD is not an input on DCE
    bool getRI() override { return 0; };  // RI is not an input on DCE

    // public because forwarder function needs it
    void eventReceived(const cdc_acm_host_dev_event_data_t *event);
    void dataReceived(const uint8_t *data, size_t length);
};

#endif /* CONFIG_USB_CDC_ACM_HOST_ENABLED */

#endif /* ACMCHANNEL_H */
