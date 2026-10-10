#ifndef HOTSYNC_SERVICE_H
#define HOTSYNC_SERVICE_H

// Makes FujiNet a HotSync server. A Palm device syncs with it over:
//   - network HotSync (NetSync, TCP 14238);
//   - serial-over-TCP, which POSE and CloudpilotEmu use for their serial port;
//   - a serial cradle, on a FujiNet-PC serial device or on the bus line
//     (HotSyncBusCradle.h), where Palm apps also reach FujiNet.
// Each sync installs the files queued in <root>/install and backs databases
// up to <root>/backup/<user>.

#include "HotSyncBusCradle.h"
#include "HotSyncSession.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

class FileSystem;
class fnTcpServer;
class HotSyncLink;

struct HotSyncServiceConfig {
    std::string root = "/palm";
    std::string user_name = "FujiNet";
    HotSyncBackup backup = HotSyncBackup::FLAGGED;
    // 0 disables a listener.
    uint16_t netsync_port = 14238;
    uint16_t emulator_port = 6416;
    // A host device path on FujiNet-PC, or HOTSYNC_BUS_SERIAL_PORT for a cradle
    // on the bus line. Empty disables it.
    std::string serial_port;
};

constexpr const char *HOTSYNC_BUS_SERIAL_PORT = "bus";

class HotSyncService
{
public:
    // Defined in the .cpp, where SerialCradle is complete.
    HotSyncService();
    ~HotSyncService();

    // bus_cradle is the bus's cradle line, for serial_port=bus; nullptr where
    // the bus has none.
    void start(const HotSyncServiceConfig &config, FileSystem &fs,
               HotSyncBusCradle *bus_cradle = nullptr);
    void stop();
    bool running() const { return _thread.joinable(); }
    // One line describing the most recent sync, for the web UI.
    std::string last_result();

private:
    enum class Transport { NETSYNC, SERIAL_OVER_TCP };
    struct SerialCradle;

    void run();
    void open_listeners();
    void close_listeners();
    void poll_listener(fnTcpServer *server, Transport transport);
    void open_cradle();
    void poll_serial();
    void serve_bus_cradle();
    void listen_for_hotsync(SerialCradle &cradle);
    void sync(HotSyncLink &link, Transport transport);
    HotSyncOptions session_options() const;
    void record(const HotSyncReport &report);

    HotSyncServiceConfig _config;
    FileSystem *_fs = nullptr;
    std::thread _thread;
    std::atomic<bool> _stopping{false};
    std::unique_ptr<fnTcpServer> _netsync_server;
    std::unique_ptr<fnTcpServer> _emulator_server;
    std::mutex _result_lock;
    std::string _last_result = "No HotSync yet";

    std::unique_ptr<SerialCradle> _cradle;
    HotSyncBusCradle *_bus_cradle = nullptr;
};

class fnConfig;
HotSyncServiceConfig hotsync_config_from(fnConfig &config);

#endif // HOTSYNC_SERVICE_H
