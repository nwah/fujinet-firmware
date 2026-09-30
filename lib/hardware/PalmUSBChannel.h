#ifndef PALMUSBCHANNEL_H
#define PALMUSBCHANNEL_H

#ifdef CONFIG_USB_PALM_HOST_ENABLED

// FujiBus to a Palm handheld in its USB cradle (developed on a Handspring
// Visor), with the ESP32-S3 as the USB host (the Palm's end is its "USB
// Library", used with the Ser* API).
//
// The Palm is a vendor-class device, not CDC-ACM: after an optional
// Handspring connection-info handshake it is a raw byte stream on a pair of
// bulk endpoints. It only enumerates while a Palm app has the USB Library
// open, and drops off the bus when the app closes it, so unlike ACMChannel
// begin() does not wait for a device: the channel is simply down (writes
// are dropped, nothing arrives) until the Palm shows up.

#include "IOChannel.h"
#include "RS232ChannelProtocol.h"

#include <atomic>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <usb/usb_host.h>

#define PALMUSB_IN_TRANSFERS 4

class PalmUSBChannel : public IOChannel, public RS232ChannelProtocol
{
private:
    usb_host_client_handle_t _client = nullptr;
    QueueHandle_t _events = nullptr;   // client events for the worker task
    QueueHandle_t _rxQueue = nullptr;  // received bytes for updateFIFO()
    SemaphoreHandle_t _ctrlDone = nullptr, _outDone = nullptr;

    usb_device_handle_t _dev = nullptr;
    uint8_t _interface = 0, _inEp = 0, _outEp = 0;
    uint16_t _inMps = 0;
    std::atomic<bool> _connected{false};
    std::atomic<int> _inFlight{0};
    std::atomic<bool> _outPending{false};

    usb_transfer_t *_in[PALMUSB_IN_TRANSFERS] = {};
    usb_transfer_t *_out = nullptr;
    usb_transfer_t *_ctrl = nullptr;

    // Which bulk endpoint pair carries the data. With the USB Library open
    // the Palm reports two generic ports; port 1 is the one the Palm side
    // of FujiNet uses (see fujinet-palm tools/visorbridge.js).
    uint8_t _endpointNumber = 1;

    uint16_t _expected_vid = 0x082D, _expected_pid = 0x0100;
    UBaseType_t _service_priority = 20;

    void openDevice(uint8_t address);
    void closeDevice();
    bool findEndpoints(const usb_config_desc_t *config);
    bool vendorRequestIn(uint8_t request, uint16_t index, uint16_t length);

protected:
    void updateFIFO() override;
    size_t dataOut(const void *buffer, size_t length) override;

public:
    void begin();
    void end() override;

    // Accept a different device (e.g. another Handspring model). Call
    // before begin(); 0 = don't check that field.
    void setExpectedDevice(uint16_t vid, uint16_t pid) { _expected_vid = vid; _expected_pid = pid; }

    // FreeRTOS priority for the USB tasks, as ACMChannel
    void setServicePriority(UBaseType_t priority);

    bool connected() { return _connected; }

    void flushOutput() override {}

    uint32_t getBaudrate() override { return 0; }
    void setBaudrate(uint32_t baud) override {}

    // No modem control lines on this link
    bool getDTR() override { return _connected; }
    void setDSR(bool state) override {}
    bool getRTS() override { return true; }
    void setCTS(bool state) override {}
    void setDCD(bool state) override {}
    bool getDCD() override { return _connected; }
    void setRI(bool state) override {}
    bool getRI() override { return false; }

    // Public for the C callbacks
    void clientEvent(const usb_host_client_event_msg_t *event);
    void inDone(usb_transfer_t *transfer);
    void outDone(usb_transfer_t *transfer);
    void clientTask();
    void workerTask();
};

#endif /* CONFIG_USB_PALM_HOST_ENABLED */

#endif /* PALMUSBCHANNEL_H */
