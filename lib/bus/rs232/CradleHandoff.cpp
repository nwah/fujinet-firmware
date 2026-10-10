#ifdef BUILD_RS232

#include "CradleHandoff.h"

#include <algorithm>
#include <chrono>

bool CradleHandoff::accept()
{
    std::lock_guard<std::mutex> lk(_lock);
    if (_state != State::HANDED_OVER)
        return false;
    _state = State::ACCEPTED;
    _changed.notify_all();
    return true;
}

int CradleHandoff::read(uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    std::unique_lock<std::mutex> lk(_lock);
    auto live = [this]
    { return _state == State::HANDED_OVER || _state == State::ACCEPTED; };
    if (!live())
        return -1;
    _changed.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                      [&] { return !_in.empty() || !live(); });
    if (!live())
        return -1;
    size_t n = std::min(len, _in.size());
    for (size_t i = 0; i < n; i++)
    {
        buf[i] = _in.front();
        _in.pop_front();
    }
    return (int)n;
}

int CradleHandoff::write(const uint8_t *buf, size_t len)
{
    std::lock_guard<std::mutex> lk(_lock);
    if (_state != State::HANDED_OVER && _state != State::ACCEPTED)
        return -1;
    _out.insert(_out.end(), buf, buf + len);
    _changed.notify_all();
    return (int)len;
}

void CradleHandoff::set_baud_rate(uint32_t baud)
{
    std::unique_lock<std::mutex> lk(_lock);
    auto live = [this]
    { return _state == State::HANDED_OVER || _state == State::ACCEPTED; };
    if (!live())
        return;
    _baud_pending = true;
    _baud = baud;
    _out_before_baud = _out.size();
    _changed.notify_all();
    _changed.wait_for(lk, std::chrono::milliseconds(1000),
                      [&] { return !_baud_pending || !live(); });
}

void CradleHandoff::finish()
{
    std::lock_guard<std::mutex> lk(_lock);
    if (_state == State::HANDED_OVER || _state == State::ACCEPTED)
        _state = State::FINISHED;
    _baud_pending = false;
    _changed.notify_all();
}

bool CradleHandoff::start(const uint8_t *first, size_t len)
{
    std::lock_guard<std::mutex> lk(_lock);
    if (_state != State::IDLE)
        return false;
    _in.clear();
    _out.clear();
    _baud_pending = false;
    _baud = 0;
    _out_before_baud = 0;
    if (len)
        _in.insert(_in.end(), first, first + len);
    _state = State::HANDED_OVER;
    _changed.notify_all();
    return true;
}

bool CradleHandoff::active()
{
    std::lock_guard<std::mutex> lk(_lock);
    return _state != State::IDLE;
}

bool CradleHandoff::accepted()
{
    std::lock_guard<std::mutex> lk(_lock);
    return _state == State::ACCEPTED || _state == State::FINISHED;
}

bool CradleHandoff::finished()
{
    std::lock_guard<std::mutex> lk(_lock);
    return _state == State::FINISHED;
}

void CradleHandoff::received(const uint8_t *buf, size_t len)
{
    std::lock_guard<std::mutex> lk(_lock);
    if (_state != State::HANDED_OVER && _state != State::ACCEPTED)
        return;
    _in.insert(_in.end(), buf, buf + len);
    _changed.notify_all();
}

size_t CradleHandoff::outgoing(uint8_t *buf, size_t max)
{
    std::lock_guard<std::mutex> lk(_lock);
    if (_state == State::IDLE)
        return 0;
    size_t n = std::min(max, _out.size());
    if (_baud_pending)
        n = std::min(n, _out_before_baud);
    for (size_t i = 0; i < n; i++)
    {
        buf[i] = _out.front();
        _out.pop_front();
    }
    if (_baud_pending)
        _out_before_baud -= n;
    if (n)
        _changed.notify_all();
    return n;
}

bool CradleHandoff::baud_wanted(uint32_t &baud)
{
    std::lock_guard<std::mutex> lk(_lock);
    if (!_baud_pending || _out_before_baud != 0)
        return false;
    baud = _baud;
    return true;
}

void CradleHandoff::baud_switched()
{
    std::lock_guard<std::mutex> lk(_lock);
    _baud_pending = false;
    _changed.notify_all();
}

void CradleHandoff::take_back()
{
    std::lock_guard<std::mutex> lk(_lock);
    _state = State::IDLE;
    _in.clear();
    _out.clear();
    _baud_pending = false;
    _out_before_baud = 0;
    _changed.notify_all();
}

#endif /* BUILD_RS232 */
