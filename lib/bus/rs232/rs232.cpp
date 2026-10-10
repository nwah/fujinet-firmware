#ifdef BUILD_RS232

#include "rs232.h"
#include "FujiBusPacket.h"

#include "../../include/debug.h"

#include "rs232/rs232Fuji.h"
#include "rs232/rs232Network.h"
#include "modem.h"
#include "siocpm.h"
#include "CradleHandoff.h"

#include <algorithm>

#include "fnSystem.h"
#include "fnConfig.h"
#include "fnWiFi.h"
#include "fnDNS.h"
#include "led.h"
#include "utils.h"
#include "fuji_endian.h"

#ifdef ESP_PLATFORM
#define SERIAL_DEVICE FN_UART_BUS
#else /* !ESP_PLATFORM */
#define SERIAL_DEVICE Config.get_serial_port()
#endif /* ESP_PLATFORM */

#if FUJINET_OVER_USB
// run USB just above the WiFi task (prio 23) until association, per drivewire.cpp
#define RS232_USB_BOOT_PRIORITY 24
#define RS232_USB_RUN_PRIORITY  20
#endif

using namespace std::chrono_literals;

// A Palm repeats its WAKEUP for longer than a listen window plus the gap after it.
static constexpr auto CRADLE_LISTEN_WINDOW = 1500ms;  // at the HotSync rate
static constexpr auto CRADLE_BUS_WINDOW    = 1500ms;  // at the bus rate between windows
static constexpr auto CRADLE_IDLE          = 60s;     // no FujiBus this long: listen anyway
static constexpr auto CRADLE_ACCEPT_TIME   = 5s;      // for the service to pick a HotSync up
static constexpr const char SLP_PREAMBLE[] = "\xBE\xEF\xED";

// Helper functions outside the class defintions

// Calculate 8-bit checksum
uint8_t rs232_checksum(uint8_t *buf, unsigned short len)
{
    unsigned int chk = 0;

    for (int i = 0; i < len; i++)
        chk = ((chk + buf[i]) >> 8) + ((chk + buf[i]) & 0xff);

    return chk;
}

void systemBus::transaction_accept(transState_t expectMoreData)
{
    assert(_transaction_state == TRANS_STATE::INVALID);
    _activePacketDataPosition = 0;
    _transaction_state = expectMoreData;
}

void systemBus::transaction_success()
{
    assert(_transaction_state == TRANS_STATE::NO_GET || _transaction_state == TRANS_STATE::DID_GET);
    sendReplyPacket(_activeDev->id(), true, nullptr, 0);
    _transaction_state = TRANS_STATE::INVALID;
}

void systemBus::transaction_error()
{
    sendReplyPacket(_activeDev->id(), false, nullptr, 0);
    _transaction_state = TRANS_STATE::INVALID;
}

success_is_true systemBus::transaction_get(void *data, size_t len)
{
    assert(_transaction_state == TRANS_STATE::WILL_GET);
    _transaction_state = TRANS_STATE::DID_GET;

    if (!len)
        RETURN_SUCCESS_AS_TRUE();

    auto optional_data = _activePacket->data();
    if (!optional_data.has_value())
        RETURN_ERROR_AS_FALSE();
    size_t avail = optional_data.value().size() - _activePacketDataPosition;
    avail = std::min(avail, (size_t) len);
    memcpy(data, optional_data.value().data() + _activePacketDataPosition, avail);
    _activePacketDataPosition += avail;
    RETURN_SUCCESS_AS_TRUE();
}

void systemBus::transaction_send(const void *data, size_t len, bool is_error)
{
    assert(_transaction_state == TRANS_STATE::NO_GET);
    sendReplyPacket(_activeDev->id(), !is_error, data, len);
    _transaction_state = TRANS_STATE::INVALID;
}

// Read and process a command frame from RS232
void systemBus::_rs232_process_cmd()
{
    Debug_printf("rs232_process_cmd()\n");

    ByteBuffer packet;
    int val, count;

    while (1)
    {
        val = _port->read();
        if (val < 0 || val == SLIP_END)
            break;
        packet.push_back(val);
    }
    if (packet.size() && _cradle_watching())
    {
        // A HotSync that opened at the bus rate; hand it the bytes from its preamble on.
        auto it = std::search(packet.begin(), packet.end(),
                              SLP_PREAMBLE, SLP_PREAMBLE + 3);
        if (it != packet.end())
        {
            // The SLIP_END that ended the loop is part of the frame (a PADP flags byte).
            if (val == SLIP_END)
                packet.push_back(SLIP_END);
            _cradle_hand_over(&*it, packet.end() - it);
            return;
        }
        _cradleStray = true;
    }
    if (packet.size())
        _modemDev->tx(packet);
    if (val < 0)
        return;

    auto tempFrame = readBusPacket(val);
    if (!tempFrame)
    {
        Debug_printv("packet fail");
        return;
    }
    _cradleLastFujiBus = std::chrono::steady_clock::now();

    // Turn on the RS232 indicator LED
    fnLedManager.set(eLed::LED_BUS, true);

    Debug_printf("\nCF: dev:%02x cmd:%02x dlen:%d\n",
                 (uint8_t) tempFrame->device(), (uint8_t) tempFrame->command(),
                 tempFrame->data() ? tempFrame->data()->size() : -1);

    _activePacket = tempFrame.get();
    _activeDev = _daisyChain.deviceWithFujiID(tempFrame->device());
    if (_activeDev != nullptr)
        _activeDev->rs232_process(*tempFrame);
    else
    {
        Debug_printf("No such device 0x%02x\n", (uint8_t) tempFrame->device());
        transaction_error();
    }

    fnLedManager.set(eLed::LED_BUS, false);
}

/*
 Primary RS232 serivce loop:
 * If MOTOR line asserted, hand RS232 processing over to the TAPE device
 * If CMD line asserted, try reading CMD frame and sending it to appropriate device
 * If CMD line not asserted but MODEM is active, give it a chance to read incoming data
 * Throw out stray input on RS232 if neither of the above two are true
 * Give NETWORK devices an opportunity to signal available data
 */
void systemBus::service()
{
#if FUJINET_OVER_USB
    // one-shot: drop USB back to normal priority once WiFi is up
    if (_usb_boot_priority && fnWiFi.connected())
    {
        _serial.setServicePriority(RS232_USB_RUN_PRIORITY);
        _usb_boot_priority = false;
    }
#endif

    // Check for any messages in our queue (this should always happen, even if any other special
    // modes disrupt normal RS232 handling - should probably make a separate task for this)
    /*_rs232_process_queue();*/

    if (_cpmDev != nullptr && _cpmDev->cpmActive)
    {
        _cpmDev->rs232_handle_cpm();
        return; // break!
    }

    if (_hotsync && _cradle_service())
        return;

    if (_port->available())
    {
        _rs232_process_cmd();
    }
    // Go check if the modem needs to read data if it's active
    else if (_modemDev != nullptr /*&& _modemDev->modemActive*/ && Config.get_modem_enabled())
    {
        _modemDev->rs232_handle_modem();
    }

    // Handle interrupts from network protocols
    if (!_netDev.empty())
    {
        bool hasUpdate = false;
        for (auto it=_netDev.begin(); it != _netDev.end(); ++it)
        {
            if (it->second->poll_interrupt())
            {
                hasUpdate = true;
                break;
            }
        }
        if (!isBoIP())
        {
#ifdef ESP_PLATFORM
            _serial.setRI(!hasUpdate);
#else /* ! ESP_PLATFORM */
            switch (Config.get_serial_proceed())
            {
            case fnConfig::SERIAL_PROCEED_DTR:
                _serial.setDSR(!hasUpdate); // drives RS-232 DTR
                break;
            case fnConfig::SERIAL_PROCEED_RTS:
                _serial.setCTS(!hasUpdate); // drives RS-232 RTS
                break;
            default:
                break; // SERIAL_PROCEED_NONE / invalid
            }
#endif /* ESP_PLATFORM */
        }
    }
}

// Setup RS232 bus
void systemBus::setup()
{
    // shutdown() latches this true; left set across an in-process restart,
    // TNFS's poll loop bails out on its first check and fakes success.
    shuttingDown = false;

    Debug_printf("RS232 SETUP: Baud rate: %u\n",Config.get_serial_baud());

    // Set up UART
#ifndef FUJINET_OVER_USB
    if (Config.get_boip_enabled())
    {
        Debug_printf("RS232 SETUP: BOIP host: %s\n", Config.get_boip_host().c_str());
        _boip.begin(BoIPConfig()
                    .hostName(Config.get_boip_host())
                    .portNum(Config.get_boip_port())
                    );
        _port = &_boip;
    }
    else {
#if FUJINET_OVER_USB
        _serial.begin();
#else /* ! FUJINET_OVER_USB */
        _serial.begin(ChannelConfig()
                      .baud(Config.get_serial_baud())
                      .readTimeout(200)
                      .deviceID(SERIAL_DEVICE)
#ifdef ESP_PLATFORM
                      .rtsPin(PIN_RS232_RTS)
                      .ctsPin(PIN_RS232_CTS)
                      .dtrPin(PIN_RS232_DTR)
                      .dsrPin(PIN_RS232_DSR)
                      .dcdPin(PIN_RS232_DCD)
                      .riPin(PIN_RS232_RI)
#endif /* ESP_PLATFORM */
                      );
#endif /* FUJINET_OVER_USB */
        _port = &_serial;
    }

#else /* FUJINET_OVER_USB */
    // Which USB device to accept is a property of the board, not of this bus,
    // so the pinmap supplies it: FN_USB_EXPECTED_VID / _PID, either of which may
    // be omitted to mean "don't check that field". Keeps lib/bus board-agnostic
    // instead of accumulating one #ifdef per co-processor board.
#if defined(FN_USB_EXPECTED_VID) || defined(FN_USB_EXPECTED_PID)
#ifndef FN_USB_EXPECTED_VID
#define FN_USB_EXPECTED_VID 0
#endif
#ifndef FN_USB_EXPECTED_PID
#define FN_USB_EXPECTED_PID 0
#endif
    _serial.setExpectedDevice(FN_USB_EXPECTED_VID, FN_USB_EXPECTED_PID);
#endif
    _serial.setServicePriority(RS232_USB_BOOT_PRIORITY);
    _usb_boot_priority = true;
    _serial.begin();
    _port = &_serial;
#endif /* FUJINET_OVER_USB */

    // A Palm can sit on a UART or on the USB Palm cradle, not on BoIP
#if !FUJINET_OVER_USB
    _cradle = (_port == &_serial) ? &_serial : nullptr;
#elif defined(CONFIG_USB_PALM_HOST_ENABLED)
    _cradle = &_serial;
#else
    _cradle = nullptr;
#endif
    if (_hotsync && _cradleMode == CradleMode::HOTSYNC)
        _hotsync->take_back();
    _cradleMode = CradleMode::FUJIBUS;

    Debug_println("RS232 Setup Flush");
    _port->discardInput();
}

bool systemBus::attachHotSync(CradleHandoff *handoff)
{
    if (!_cradle)
        return false;
    auto now = std::chrono::steady_clock::now();
    _hotsync = handoff;
    _cradleLastFujiBus = now;
    _cradleNextListen = now;
    return true;
}

// Is the line one where a HotSync may start? (Not when no Palm, or a Palm app, is on it.)
bool systemBus::_cradle_watching()
{
    if (!_hotsync || !_cradle)
        return false;
    auto use = _cradle->palmUse();
    return use == PalmCradle::PalmUse::UNKNOWN || use == PalmCradle::PalmUse::HOTSYNC;
}

// Drop what has already arrived. Not discardInput(), which waits for the line to
// go quiet and would stall the loop while a Palm keeps sending.
void systemBus::_cradle_drain()
{
    uint8_t buf[64];
    for (int i = 0; i < 64 && _port->available(); ++i)
        _port->read(buf, std::min((size_t) _port->available(), sizeof(buf)));
}

void systemBus::_cradle_hand_over(const uint8_t *first, size_t len)
{
    if (!_hotsync->start(first, len))
    {
        Debug_printf("Cradle: HotSync service busy, not handing over\n");
        return;
    }
    _cradleMode = CradleMode::HOTSYNC;
    _cradleDeadline = std::chrono::steady_clock::now() + CRADLE_ACCEPT_TIME;
    Debug_printf("Cradle: a Palm started a HotSync, handing it to the HotSync service\n");
}

void systemBus::_cradle_take_back()
{
    auto now = std::chrono::steady_clock::now();
    _hotsync->take_back();
    if (_cradle->hotsyncBaudrate() && _serial.getBaudrate() != (uint32_t) _rs232Baud)
    {
        _serial.flushOutput();
        _serial.setBaudrate(_rs232Baud);
    }
    _cradle_drain();
    _cradleMode = CradleMode::FUJIBUS;
    _cradleNextListen = now + CRADLE_BUS_WINDOW;
    _cradleLastFujiBus = now;
    _cradleStray = false;
    Debug_printf("Cradle: HotSync over, serving FujiBus at %d baud\n", _rs232Baud);
}

// The bus decides what the cradle line carries. True when this pass isn't FujiBus.
bool systemBus::_cradle_service()
{
    auto now = std::chrono::steady_clock::now();
    uint8_t buf[256];

    switch (_cradleMode)
    {
    case CradleMode::FUJIBUS:
    {
        auto use = _cradle->palmUse();
        // A USB cradle knows when the HotSync app opened the line: hand over at once.
        if (use == PalmCradle::PalmUse::HOTSYNC)
        {
            if (!_cradleSawHotSyncUse)
            {
                _cradleSawHotSyncUse = true;
                _cradle_hand_over(nullptr, 0);
                return true;
            }
        }
        else
            _cradleSawHotSyncUse = false;

        uint32_t rate = _cradle->hotsyncBaudrate();
        // A HotSync that opens at another rate can't be heard at the bus rate: listen
        // for it at its own rate while no host talks FujiBus, or once bytes that
        // weren't FujiBus show up (a WAKEUP at 9600 reads as garbage at 115200).
        if (use == PalmCradle::PalmUse::UNKNOWN && rate && rate != (uint32_t) _rs232Baud
            && now >= _cradleNextListen
            && (_cradleStray || now - _cradleLastFujiBus >= CRADLE_IDLE))
        {
            _serial.flushOutput();
            _serial.setBaudrate(rate);
            _cradle_drain();
            _cradleStray = false;
            _cradleScan.clear();
            _cradleDeadline = now + CRADLE_LISTEN_WINDOW;
            _cradleMode = CradleMode::LISTENING;
            return true;
        }
        return false;
    }

    case CradleMode::LISTENING:
    {
        // Never block: the window is short and the bus has other work.
        size_t n = std::min((size_t) _port->available(), sizeof(buf));
        if (n)
        {
            n = _port->read(buf, n);
            _cradleScan.append((const char *) buf, n);
        }
        size_t pos = _cradleScan.find(SLP_PREAMBLE, 0, 3);
        if (pos != std::string::npos)
        {
            _cradle_hand_over((const uint8_t *) _cradleScan.data() + pos, _cradleScan.size() - pos);
            return true;
        }
        // Keep a preamble split across reads.
        if (_cradleScan.size() > 2)
            _cradleScan.erase(0, _cradleScan.size() - 2);
        if (now >= _cradleDeadline)
        {
            _serial.setBaudrate(_rs232Baud);
            _cradle_drain();
            _cradleMode = CradleMode::FUJIBUS;
            _cradleNextListen = now + CRADLE_BUS_WINDOW;
        }
        return true;
    }

    case CradleMode::HOTSYNC:
    {
        size_t n = std::min((size_t) _port->available(), sizeof(buf));
        if (n)
        {
            n = _port->read(buf, n);
            _hotsync->received(buf, n);
        }
        while ((n = _hotsync->outgoing(buf, sizeof(buf))) > 0)
            _port->write(buf, n);

        uint32_t baud;
        if (_hotsync->baud_wanted(baud))
        {
            _serial.flushOutput();
            if (_cradle->hotsyncBaudrate())  // USB has no rate
                _serial.setBaudrate(baud);
            _hotsync->baud_switched();
            Debug_printf("Cradle: HotSync at %lu baud\n", (unsigned long) baud);
        }

        // After pumping out, so the service's last bytes are sent.
        if (_hotsync->finished() || (!_hotsync->accepted() && now >= _cradleDeadline))
            _cradle_take_back();
        return true;
    }
    }
    return false;
}

// Add device to RS232 bus
void systemBus::addDevice(virtualDevice *pDevice, fujiDeviceID_t device_id)
{
    if (device_id == FUJI_DEVICEID::FUJINET)
    {
        _fujiDev = dynamic_cast<rs232Fuji *>(pDevice);
    }
    else if (device_id == FUJI_DEVICEID::SERIAL)
    {
        _modemDev = (rs232Modem *)pDevice;
    }
    else if (device_id >= FUJI_DEVICEID::NETWORK && device_id <= FUJI_DEVICEID::NETWORK_LAST)
    {
        _netDev[device_id - FUJI_DEVICEID::NETWORK] = (rs232Network *)pDevice;
    }
    else if (device_id == FUJI_DEVICEID::MIDI)
    {
        _streamDev = (rs232NetStream *)pDevice;
    }
    else if (device_id == FUJI_DEVICEID::CPM)
    {
        _cpmDev = (rs232CPM *)pDevice;
    }
    else if (device_id == FUJI_DEVICEID::PRINTER)
    {
        _printerdev = (rs232Printer *)pDevice;
    }

    _daisyChain.addDevice(pDevice, device_id);
}

// Give devices an opportunity to clean up before a reboot
void systemBus::shutdown()
{
    shuttingDown = true;

    for (auto devicep : _daisyChain)
    {
        Debug_printf("Shutting down device %02x\n", (uint8_t) devicep->id());
        devicep->shutdown();
    }
    Debug_printf("All devices shut down.\n");
}

int systemBus::getBaudrate()
{
    return _rs232Baud;
}

void systemBus::setBaudrate(int baud)
{
    if (_rs232Baud == baud)
    {
        Debug_printf("Baudrate already at %d - nothing to do\n", baud);
        return;
    }

    Debug_printf("Changing baudrate from %d to %d\n", _rs232Baud, baud);
    _rs232Baud = baud;
    _serial.setBaudrate(baud);
}

std::unique_ptr<FujiBusPacket> systemBus::readBusPacket(int first)
{
    ByteBuffer packet;
    int count = 0;

    // Define the logic once in a local lambda
    auto processByte = [&](int val) -> bool
    {
        if (val < 0)
            return false;
        packet.push_back(static_cast<uint8_t>(val));
        if (val == SLIP_END)
            count++;
        return true;
    };

    processByte(first);

    while (count < 2)
    {
        if (!processByte(_port->read()))
            break;
    }

#ifdef DEBUG_RAW_PACKET
    Debug_printv("Received %d:\n%s", packet.size(),
                 util_hexdump(packet.data(), packet.size()).c_str());
#endif // DEBUG_RAW_PACKET
    return FujiBusPacket::fromSerialized(packet);
}

void systemBus::writeBusPacket(FujiBusPacket &packet)
{
    ByteBuffer encoded = packet.serialize();
    _port->write(encoded.data(), encoded.size());
#ifdef DEBUG_RAW_PACKET
    Debug_printv("Sent %d:\n%s", encoded.size(),
                 util_hexdump(encoded.data(), encoded.size()).c_str());
#endif // DEBUG_RAW_PACKET
    return;
}

void systemBus::sendReplyPacket(fujiDeviceID_t source, bool ack, const void *data, size_t length)
{
    // FIXME - check to make sure this wasn't through a bus call
    if (source == _modemDev->id())
    {
        _port->write(data, length);
        return;
    }

    ByteBuffer bb;

    if (ack && data)
    {
        const uint8_t *start = static_cast<const uint8_t*>(data);
        bb.assign(start, start + length);
    }

    FujiBusPacket packet(source, ack ? CMD::FUJI_ACK : CMD::FUJI_NAK, bb);
    writeBusPacket(packet);
    return;
}

fujiDeviceID_t virtualDevice::id()
{
    return SYSTEM_BUS.fujiIDForDevice(this);
}

#endif /* BUILD_RS232 */
