#ifdef BUILD_RS232

#include "rs232HotSync.h"

#include <algorithm>

rs232HotSync::rs232HotSync(std::chrono::milliseconds turn, std::chrono::milliseconds quiet)
    : _turn(turn), _quiet(quiet), _last_packet(clock::now())
{
}

bool rs232HotSync::host_active_at(clock::time_point now)
{
    size_t packets = _packets;
    size_t stray = _stray;
    if (packets != _seen_packets)
    {
        _seen_packets = packets;
        _last_packet = now;
    }
    else if (stray != _seen_stray)
    {
        // Noise and no packets: the device has left its app to HotSync.
        _last_packet = now - _quiet;
    }
    _seen_stray = stray;
    return now - _last_packet < _quiet;
}

bool rs232HotSync::claim()
{
    std::unique_lock<std::mutex> lock(_lock);
    _in.clear();
    _out.clear();
    _wanted_baud = CMP_BAUD;
    return _changed.wait_for(lock, _turn, [&] { return _served_baud == _wanted_baud; });
}

void rs232HotSync::release()
{
    std::unique_lock<std::mutex> lock(_lock);
    _wanted_baud = 0;
    _out.clear();
    _changed.wait_for(lock, _turn, [&] { return _served_baud == 0; });
    _in.clear();
}

int rs232HotSync::read(uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    std::unique_lock<std::mutex> lock(_lock);
    if (_wanted_baud == 0)
        return -1;
    if (!_changed.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                           [&] { return !_in.empty() || _wanted_baud == 0; }))
        return 0;
    if (_wanted_baud == 0)
        return -1;
    size_t count = std::min(len, _in.size());
    std::copy_n(_in.begin(), count, buf);
    _in.erase(_in.begin(), _in.begin() + count);
    return static_cast<int>(count);
}

int rs232HotSync::write(const uint8_t *buf, size_t len)
{
    std::unique_lock<std::mutex> lock(_lock);
    if (_wanted_baud == 0)
        return -1;
    _out.insert(_out.end(), buf, buf + len);
    // Returns once the bytes are on the wire, as a UART write and flush would.
    if (!_changed.wait_for(lock, _turn, [&] { return _out.empty() && !_sending; }))
        return 0;
    return static_cast<int>(len);
}

void rs232HotSync::set_baud_rate(uint32_t baud)
{
    std::unique_lock<std::mutex> lock(_lock);
    _wanted_baud = baud;
    _changed.wait_for(lock, _turn, [&] { return _served_baud == baud; });
}

uint32_t rs232HotSync::line_wanted()
{
    std::lock_guard<std::mutex> lock(_lock);
    return _wanted_baud;
}

void rs232HotSync::line_received(const uint8_t *buf, size_t len)
{
    std::lock_guard<std::mutex> lock(_lock);
    _in.insert(_in.end(), buf, buf + len);
    _changed.notify_all();
}

size_t rs232HotSync::line_outgoing(uint8_t *buf, size_t len)
{
    std::lock_guard<std::mutex> lock(_lock);
    size_t count = std::min(len, _out.size());
    std::copy_n(_out.begin(), count, buf);
    _out.erase(_out.begin(), _out.begin() + count);
    if (count)
        _sending = true;
    return count;
}

void rs232HotSync::line_served(uint32_t baud)
{
    std::lock_guard<std::mutex> lock(_lock);
    _served_baud = baud;
    _sending = false;
    _changed.notify_all();
}

#endif /* BUILD_RS232 */
