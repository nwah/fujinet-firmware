#include "fnConfig.h"
#include "utils.h"

#include <cstring>

#include "../../include/debug.h"

// [HotSync]: the Palm OS HotSync server in lib/hotsync.

void fnConfig::store_hotsync_enabled(bool enabled)
{
    if (_hotsync.enabled == enabled)
        return;
    _hotsync.enabled = enabled;
    _dirty = true;
}

void fnConfig::store_hotsync_user(const std::string &user)
{
    if (_hotsync.user == user)
        return;
    _hotsync.user = user;
    _dirty = true;
}

void fnConfig::store_hotsync_backup(const std::string &backup)
{
    if (_hotsync.backup == backup)
        return;
    _hotsync.backup = backup;
    _dirty = true;
}

// A port of 0 turns that listener off; anything unparseable keeps the default.
static int parse_port(const std::string &value, int fallback)
{
    int port = atoi(value.c_str());
    return (port < 0 || port > 65535 || (port == 0 && value != "0")) ? fallback : port;
}

void fnConfig::_read_section_hotsync(std::stringstream &ss)
{
    std::string line;
    // Read lines until one starts with '[' which indicates a new section
    while (_read_line(ss, line, '[') >= 0)
    {
        std::string name;
        std::string value;
        if (!_split_name_value(line, name, value))
            continue;

        if (strcasecmp(name.c_str(), "enabled") == 0)
            _hotsync.enabled = util_string_value_is_true(value);
        else if (strcasecmp(name.c_str(), "user") == 0 && !value.empty())
            _hotsync.user = value;
        else if (strcasecmp(name.c_str(), "backup") == 0 && !value.empty())
            _hotsync.backup = value;
        else if (strcasecmp(name.c_str(), "netsync_port") == 0)
            _hotsync.netsync_port = parse_port(value, _hotsync.netsync_port);
        else if (strcasecmp(name.c_str(), "emulator_port") == 0)
            _hotsync.emulator_port = parse_port(value, _hotsync.emulator_port);
        else if (strcasecmp(name.c_str(), "serial_port") == 0)
            _hotsync.serial_port = value;
    }
}
