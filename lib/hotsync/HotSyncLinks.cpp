#include "HotSyncLinks.h"

#include "IOChannel.h"
#include "global_types.h"

#include <algorithm>
#include <chrono>
#include <thread>

static constexpr auto POLL_INTERVAL = std::chrono::milliseconds(2);
// The device is still reprogramming its UART right after the CMP exchange; a
// request sent at once is lost and costs a two-second PADP retry.
static constexpr auto BAUD_SETTLE = std::chrono::milliseconds(50);

// Waits until has_data() or the timeout, which is the error case.
template <typename Pred>
static success_is_true wait_for(Pred has_data, uint32_t timeout_ms)
{
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!has_data())
    {
        if (std::chrono::steady_clock::now() >= deadline)
            RETURN_ERROR_AS_FALSE();
        std::this_thread::sleep_for(POLL_INTERVAL);
    }
    RETURN_SUCCESS_AS_TRUE();
}

HotSyncTcpLink::HotSyncTcpLink(fnTcpClient client) : _client(std::move(client))
{
    _client.setNoDelay(true);
}

HotSyncTcpLink::~HotSyncTcpLink()
{
    _client.stop();
}

int HotSyncTcpLink::read(uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    bool closed = false;
    success_is_true ready = wait_for([&] {
        if (_client.available() > 0)
            return true;
        closed = !_client.connected();
        return closed;
    }, timeout_ms);
    if (closed)
        return -1;
    if (ready.is_error())
        return 0;
    return _client.read(buf, std::min(len, _client.available()));
}

int HotSyncTcpLink::write(const uint8_t *buf, size_t len)
{
    return static_cast<int>(_client.write(buf, len));
}

int HotSyncChannelLink::read(uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    if (wait_for([&] { return _channel.available() > 0; }, timeout_ms).is_error())
        return 0;
    return static_cast<int>(_channel.read(buf, std::min(len, _channel.available())));
}

int HotSyncChannelLink::write(const uint8_t *buf, size_t len)
{
    size_t wrote = _channel.write(buf, len);
    _channel.flushOutput();
    return static_cast<int>(wrote);
}

int HotSyncCradleLink::read(uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    if (_has_deadline)
    {
        auto now = std::chrono::steady_clock::now();
        if (now >= _deadline)
            return 0;
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(_deadline - now);
        timeout_ms = std::min<uint32_t>(timeout_ms, static_cast<uint32_t>(left.count()));
    }
    int got = _line.read(buf, len, timeout_ms);
    if (got > 0)
        _received += got;
    return got;
}

void HotSyncCradleLink::set_baud_rate(uint32_t baud)
{
    _line.set_baud_rate(baud);
    std::this_thread::sleep_for(BAUD_SETTLE);
}

void HotSyncCradleLink::set_read_deadline(std::chrono::steady_clock::time_point deadline)
{
    _has_deadline = true;
    _deadline = deadline;
}

void HotSyncCradleLink::clear_read_deadline()
{
    _has_deadline = false;
}

size_t HotSyncCradleLink::take_received_count()
{
    size_t count = _received;
    _received = 0;
    return count;
}
