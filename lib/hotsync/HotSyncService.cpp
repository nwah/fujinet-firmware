#include "HotSyncService.h"

#include "HotSyncFsStorage.h"
#include "HotSyncLinks.h"
#include "NetSyncTransport.h"
#include "PadpTransport.h"

#include "fnConfig.h"
#include "fnTcpServer.h"
#include "fnWiFi.h"

#include "UARTChannel.h"
#ifdef ESP_PLATFORM
#include <esp_pthread.h>
#endif

#include "../../include/debug.h"

#include <chrono>
#include <ctime>
#include <random>

static constexpr auto IDLE_POLL = std::chrono::milliseconds(50);
static constexpr uint32_t HOTSYNC_TASK_STACK = 8192;
// Identifies FujiNet as "the PC" a device last synced with ("FJNT").
static constexpr uint32_t HOTSYNC_PC_ID = 0x464A4E54;

// Bounds each WAKEUP wait so the thread still sees stop() and the listeners.
static constexpr auto CRADLE_HOTSYNC_WINDOW = std::chrono::milliseconds(1500);

// A Palm cradle on a FujiNet-PC serial device.
struct HotSyncService::SerialCradle {
    std::unique_ptr<UARTChannel> channel;
    std::unique_ptr<HotSyncChannelLink> line;
    std::unique_ptr<HotSyncCradleLink> link;

    ~SerialCradle()
    {
        if (channel)
            channel->end();
    }

    // IOChannel::discardInput() waits for the line to go quiet, which a
    // chattering cradle never does; drop only what has already arrived.
    void drop_pending_input()
    {
        uint8_t scratch[64];
        for (int i = 0; i < 64 && link->read(scratch, sizeof(scratch), 0) > 0; ++i)
            ;
    }
};

HotSyncServiceConfig hotsync_config_from(fnConfig &config)
{
    HotSyncServiceConfig out;
    out.user_name = config.get_hotsync_user();
    std::string backup = config.get_hotsync_backup();
    out.backup = backup == "none" ? HotSyncBackup::NONE
               : backup == "all"  ? HotSyncBackup::ALL
                                  : HotSyncBackup::FLAGGED;
    out.netsync_port = config.get_hotsync_netsync_port();
    out.emulator_port = config.get_hotsync_emulator_port();
    out.serial_port = config.get_hotsync_serial_port();
    return out;
}

HotSyncService::HotSyncService() = default;

HotSyncService::~HotSyncService()
{
    stop();
}

void HotSyncService::start(const HotSyncServiceConfig &config, FileSystem &fs)
{
    if (running())
        return;
    _config = config;
    _fs = &fs;
    _stopping = false;
#ifdef ESP_PLATFORM
    // Core 1 belongs to fnLoop, which never blocks at a higher priority.
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.stack_size = HOTSYNC_TASK_STACK;
    cfg.thread_name = "hotsync";
    cfg.pin_to_core = 0;
    esp_pthread_set_cfg(&cfg);
#endif
    _thread = std::thread(&HotSyncService::run, this);
#ifdef ESP_PLATFORM
    // Later std::threads must not inherit the HotSync name and core.
    esp_pthread_cfg_t defaults = esp_pthread_get_default_config();
    esp_pthread_set_cfg(&defaults);
#endif
    Debug_printf("HotSync: service started, files in %s\r\n", _config.root.c_str());
}

void HotSyncService::stop()
{
    if (!running())
        return;
    _stopping = true;
    _thread.join();
}

std::string HotSyncService::last_result()
{
    std::lock_guard<std::mutex> lock(_result_lock);
    return _last_result;
}

void HotSyncService::run()
{
    HotSyncFsStorage(*_fs, _config.root).create_install_folder();
    open_cradle();
    while (!_stopping)
    {
        if (fnWiFi.connected())
            open_listeners();
        else
            close_listeners();

        poll_listener(_netsync_server.get(), Transport::NETSYNC);
        poll_listener(_emulator_server.get(), Transport::SERIAL_OVER_TCP);
        poll_serial();
        std::this_thread::sleep_for(IDLE_POLL);
    }
    close_listeners();
    _cradle.reset();
}

void HotSyncService::open_listeners()
{
    auto open = [](std::unique_ptr<fnTcpServer> &server, uint16_t port) {
        if (server || port == 0)
            return;
        server = std::make_unique<fnTcpServer>();
        server->begin(port);
        Debug_printf("HotSync: listening on TCP %u\r\n", port);
    };
    open(_netsync_server, _config.netsync_port);
    open(_emulator_server, _config.emulator_port);
}

void HotSyncService::close_listeners()
{
    _netsync_server.reset();
    _emulator_server.reset();
}

void HotSyncService::poll_listener(fnTcpServer *server, Transport transport)
{
    if (server == nullptr || !server->hasClient())
        return;
    HotSyncTcpLink link(server->client());
    sync(link, transport);
}

void HotSyncService::open_cradle()
{
    if (_config.serial_port.empty())
        return;
#ifdef ESP_PLATFORM
    Debug_printf("HotSync: serial_port is only supported on FujiNet-PC\r\n");
#else
    auto cradle = std::make_unique<SerialCradle>();
    cradle->channel = std::make_unique<UARTChannel>();
    UARTChannel &channel = *cradle->channel;
    channel.begin(ChannelConfig()
                      .baud(CMP_INITIAL_BAUD_RATE)
                      .deviceID(_config.serial_port)
                      .readTimeout(10));
    cradle->line = std::make_unique<HotSyncChannelLink>(
        channel, [&channel](uint32_t baud) { channel.setBaudrate(baud); });
    cradle->link = std::make_unique<HotSyncCradleLink>(*cradle->line);
    Debug_printf("HotSync: cradle on %s\r\n", _config.serial_port.c_str());
    _cradle = std::move(cradle);
#endif
}

void HotSyncService::poll_serial()
{
    if (_cradle)
        listen_for_hotsync(*_cradle);
}

void HotSyncService::listen_for_hotsync(SerialCradle &cradle)
{
    auto deadline = std::chrono::steady_clock::now() + CRADLE_HOTSYNC_WINDOW;
    cradle.link->set_baud_rate(CMP_INITIAL_BAUD_RATE);
    cradle.drop_pending_input();
    cradle.link->take_received_count();
    cradle.link->set_read_deadline(deadline);

    PadpTransport padp(*cradle.link);
    success_is_true woke = padp.wait_for_wakeup();
    while (woke.is_error() && std::chrono::steady_clock::now() < deadline)
        woke = padp.wait_for_wakeup();
    // Only the WAKEUP wait is windowed. A WAKEUP late in the window still
    // needs the Palm's ACK of our INIT, and the sync takes as long as it takes.
    cradle.link->clear_read_deadline();
    if (woke.is_success())
        woke = padp.answer_wakeup();

    if (woke.is_error())
    {
        if (size_t count = cradle.link->take_received_count())
            Debug_printf("HotSync: %u bytes from the cradle at 9600 baud, no WAKEUP\r\n",
                         static_cast<unsigned>(count));
        return;
    }
    HotSyncFsStorage storage(*_fs, _config.root);
    record(HotSyncSession(padp, storage, session_options()).run());
}

void HotSyncService::sync(HotSyncLink &link, Transport transport)
{
    std::unique_ptr<DlpTransport> dlp;
    if (transport == Transport::NETSYNC)
        dlp = std::make_unique<NetSyncTransport>(link);
    else
        dlp = std::make_unique<PadpTransport>(link);

    if (dlp->accept().is_error())
    {
        Debug_printf("HotSync: handshake failed\r\n");
        return;
    }
    HotSyncFsStorage storage(*_fs, _config.root);
    record(HotSyncSession(*dlp, storage, session_options()).run());
}

HotSyncOptions HotSyncService::session_options() const
{
    HotSyncOptions options;
    options.default_user_name = _config.user_name;
    options.backup = _config.backup;
    options.pc_id = HOTSYNC_PC_ID;
    options.new_user_id = std::random_device()();

    // Before SNTP sets the clock the date is meaningless; leave the device's.
    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    if (local.tm_year + 1900 >= 2020)
    {
        options.now.year = local.tm_year + 1900;
        options.now.month = local.tm_mon + 1;
        options.now.day = local.tm_mday;
        options.now.hour = local.tm_hour;
        options.now.minute = local.tm_min;
        options.now.second = local.tm_sec;
    }
    return options;
}

void HotSyncService::record(const HotSyncReport &report)
{
    std::string line = "'" + report.user_name + "': " + std::to_string(report.installed) +
                       " installed, " + std::to_string(report.backed_up) + " backed up";
    if (report.install_failures + report.backup_failures > 0)
        line += ", " + std::to_string(report.install_failures + report.backup_failures) + " failed";
    if (report.error != DlpError::NONE)
        line += std::string(" (") + dlp_error_name(report.error) + ")";

    std::lock_guard<std::mutex> lock(_result_lock);
    _last_result = line;
}
