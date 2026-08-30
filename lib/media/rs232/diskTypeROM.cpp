#ifdef BUILD_RS232 // temporary

#include "diskTypeROM.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "../../include/debug.h"
#include "../../include/fujiCommandID.h"
#include "../../fuji/fujiDisk.h"
#include "../../fuji/fujiHost.h"

#include "bus.h"
#include "compat_string.h"
#include "fujiCommandID.h"

#ifdef BUILD_MSX
#include "fujiROMType.h"
#else
#define ROM_PUSH_STREAM_CFG 1
#define ROM_PUSH_STREAM_ROM 0
#endif

error_is_true MediaTypeROM::read(uint32_t sectornum, uint32_t *readcount)
{
    Debug_print("ROM READ not supported\r\n");
    RETURN_ERROR_AS_TRUE();
}

error_is_true MediaTypeROM::write(uint32_t sectornum, bool verify)
{
    Debug_print("ROM WRITE not supported\r\n");
    RETURN_ERROR_AS_TRUE();
}

error_is_true MediaTypeROM::format(uint32_t *responsesize)
{
    RETURN_ERROR_AS_TRUE();
}

void MediaTypeROM::status(uint8_t statusbuff[4])
{
    memset(statusbuff, 0, 4);
}

#ifdef BUILD_MSX

// rom_type_for: the MSX Pico firmware needs to know the cartridge's mapper
// (plain 16K/32K, ASCII8, ASCII16, Konami, Konami SCC, ...) before it can
// bank-switch the image correctly; unlike the Intellivision side there is
// no separate .cfg sibling file to carry this. Until there's a real ROM
// database to look it up in, we recognize a filename suffix convention
// (e.g. "Game [Konami].rom") as a stopgap -- the mapper can't be inferred
// from file size alone, since e.g. plain 16K/32K and ASCII16 titles overlap
// in size with Konami/ASCII8 ones.
static bool ends_with_ci(const char *filename, size_t filename_len, const char *suffix)
{
    size_t suffix_len = strlen(suffix);
    if (filename_len < suffix_len)
        return false;
    return strncasecmp(filename + (filename_len - suffix_len), suffix, suffix_len) == 0;
}

static fujiROMType_t rom_type_for(const char *filename)
{
    if (filename == nullptr)
        return ROM_TYPE_MSX_PLAIN;

    size_t len = strlen(filename);
    if (ends_with_ci(filename, len, "[Konami].rom"))
        return ROM_TYPE_MSX_KONAMI;
    if (ends_with_ci(filename, len, "[KonamiSCC].rom"))
        return ROM_TYPE_MSX_KONAMI_SCC;
    if (ends_with_ci(filename, len, "[ASCII8].rom"))
        return ROM_TYPE_MSX_ASCII8;
    if (ends_with_ci(filename, len, "[ASCII16].rom"))
        return ROM_TYPE_MSX_ASCII16;

    return ROM_TYPE_MSX_PLAIN;
}

// push_rom_msx: the MSX Pico firmware (fujiversal) speaks a wire shape for
// NETCMD_OPEN that is incompatible with the Intellivision cart's -- it
// decodes a real param list instead of a payload struct. Its FUJICMD_OPEN
// handler does `packet->param(0)` / `packet->param(1)` with no bounds
// check against std::vector _params, so sending it the Intellivision
// payload-struct OPEN (which carries no params at all) is out-of-bounds
// UB, not a benign zero -- the two shapes can't share push_stream().
//
// OPEN params here are (segment 0, ROM/mapper type); NETCMD_WRITE frames
// are payload bytes exactly like the Intellivision side.
//
// Unlike push_stream(), a failed transfer must NOT send NETCMD_CLOSE:
// the MSX adapter's FUJICMD_CLOSE handler sets user_rom_closed = true
// unconditionally (it doesn't look at any payload), and that flag is what
// the MSX polls as "ROM is ready, boot it" -- a CLOSE here would boot a
// half-written image. Sending FUJICMD_RESET instead isn't an option
// either: that handler never calls sendReplyPacket(), so sendCommand()
// would block forever waiting for an ACK that's never coming. So on
// failure we just send nothing further and return false; the adapter is
// left with user_rom_closed still false, the MSX never boots the partial
// image, and the next OPEN resets the stream state anyway.
static bool push_rom_msx(fnFile *f, uint32_t expected_size, fujiROMType_t rom_type)
{
    uint8_t buf[DISK_SECTORBUF_SIZE];

    auto reply = SYSTEM_BUS.sendCommand(FUJI_DEVICEID::DBC, CMD::NET_OPEN,
                                        (uint16_t)0, (uint8_t)rom_type);
    if (!reply || reply->command() != CMD::FUJI_ACK)
    {
        Debug_printv("MediaTypeROM: failed to open DBC stream (%lu bytes, type 0x%02x)\n",
                     (unsigned long)expected_size, (unsigned)rom_type);
        return false;
    }

    bool ok = true;
    uint32_t sent = 0;
    size_t got;
    while ((got = fnio::fread(buf, 1, sizeof(buf), f)) > 0)
    {
        reply = SYSTEM_BUS.sendCommand(FUJI_DEVICEID::DBC, CMD::NET_WRITE,
                                       std::string((char *)buf, got));
        if (!reply || reply->command() != CMD::FUJI_ACK)
        {
            Debug_printv("MediaTypeROM: failed to send ROM block\n");
            ok = false;
            break;
        }
        sent += got;
    }

    // fread() can't distinguish EOF from error -- the byte count is the signal.
    //
    // expected_size of 0 means the host could not tell us how big the file is,
    // not that the file is empty: tnfs_lseek() answers SEEK_END out of a size
    // captured by a tnfs_stat() run before the open, and when that stat misses
    // while the open succeeds the size stays 0 for the life of the handle.
    // The reads themselves are unaffected -- tnfs_read() goes until the server
    // says EOF -- so what arrived is real even when nothing predicted it. In
    // that case what we read is the only size there is, and the check becomes
    // "did anything arrive at all". Zero bytes still fails: an empty user ROM
    // that got CLOSEd would set the adapter's ready flag and boot the MSX into
    // an empty cartridge.
    if (ok && (expected_size == 0 ? sent == 0 : sent != expected_size))
    {
        Debug_printv("MediaTypeROM: short transfer: %lu of %lu bytes\n",
                     (unsigned long)sent, (unsigned long)expected_size);
        ok = false;
    }

    if (!ok)
    {
        // No CLOSE, no RESET -- see the function comment above.
        Debug_printv("MediaTypeROM: aborting without CLOSE so a partial ROM can't boot\n");
        return false;
    }

    reply = SYSTEM_BUS.sendCommand(FUJI_DEVICEID::DBC, CMD::NET_CLOSE);
    if (!reply || reply->command() != CMD::FUJI_ACK)
    {
        Debug_printv("MediaTypeROM: ROM close failed/rejected\n");
        return false;
    }
    return true;
}

#else

// push_stream: reads `f` from its current position in DISK_SECTORBUF_SIZE
// chunks and relays each one to the RP2040 as CMD::NET_WRITE frames on DBC
// stream `stream_id` (0 = ROM, 1 = a .cfg sibling -- the RP2040's
// dbc_inbound_handler() demuxes on this same id). Sent as PAYLOAD bytes,
// not params: FujiBusPacket::processArg(uint16_t) encodes bare integer
// arguments as wire params, but the RP2040's minimal fujibus.c client
// parses the descriptor chain only far enough to skip past it to find the
// payload -- it never surfaces decoded param values. The payload path is
// the one it actually exposes to callers (fb_reply_t.data/data_len), so
// that's what carries the OPEN header here.
//
// OPEN payload is the stream id followed by the stream's total size as 4
// little-endian bytes. The RP2040 uses the size to refuse a ROM too large
// for its cart.ROM[] before we drag the whole thing over TNFS, and to draw
// an exact progress bar; an older RP2040 build reads data[0] and ignores
// the rest, so this stays compatible in both directions.
//
// Always sends CMD::NET_CLOSE so the RP2040's stream state doesn't wedge; a
// failed transfer's CLOSE carries a 0x01 abort payload so partial data
// isn't booted.
static bool push_stream(fnFile *f, uint16_t stream_id, uint32_t expected_size)
{
    uint8_t buf[DISK_SECTORBUF_SIZE];

    struct { uint8_t id; u32le_t size; } open_hdr;
    static_assert(sizeof(open_hdr) == 5, "OPEN header must not be padded");
    open_hdr.id = (uint8_t)stream_id;
    open_hdr.size = expected_size;

    auto reply = SYSTEM_BUS.sendCommand(FUJI_DEVICEID::DBC, CMD::NET_OPEN,
                                        std::string((const char *)&open_hdr, sizeof(open_hdr)));
    if (!reply || reply->command() != CMD::FUJI_ACK)
    {
        Debug_printv("MediaTypeROM: failed to open DBC stream %u (%lu bytes)\n",
                     stream_id, (unsigned long)expected_size);
        return false;
    }

    bool ok = true;
    uint32_t sent = 0;
    size_t got;
    while ((got = fnio::fread(buf, 1, sizeof(buf), f)) > 0)
    {
        reply = SYSTEM_BUS.sendCommand(FUJI_DEVICEID::DBC, CMD::NET_WRITE,
                                       std::string((char *)buf, got));
        if (!reply || reply->command() != CMD::FUJI_ACK)
        {
            Debug_printv("MediaTypeROM: failed to send stream %u block\n", stream_id);
            ok = false;
            break;
        }
        sent += got;
    }

    // fread() can't distinguish EOF from error -- the byte count is the signal
    if (ok && sent != expected_size)
    {
        Debug_printv("MediaTypeROM: stream %u short transfer: %lu of %lu bytes\n",
                     stream_id, (unsigned long)sent, (unsigned long)expected_size);
        ok = false;
    }

    if (ok)
        reply = SYSTEM_BUS.sendCommand(FUJI_DEVICEID::DBC, CMD::NET_CLOSE);
    else
        reply = SYSTEM_BUS.sendCommand(FUJI_DEVICEID::DBC, CMD::NET_CLOSE,
                                       std::string(1, '\x01'));
    if (!reply || reply->command() != CMD::FUJI_ACK)
    {
        Debug_printv("MediaTypeROM: stream %u close failed/rejected\n", stream_id);
        ok = false;
    }
    return ok;
}

// mount()'s `filename` arrives already host-prefixed (fnfile_open rewrites
// disk.filename in place); fujiHost APIs prefix again, so strip it first.
// Only used by the .cfg-sibling lookup below, which is an Intellivision-only
// concept -- compiled out under BUILD_MSX along with it, so it doesn't sit
// around as an unused static function.
static const char *strip_host_prefix(fujiHost *host, const char *filename)
{
    const char *pfx = host->get_prefix();
    if (pfx == nullptr || pfx[0] == '\0')
        return filename;

    size_t plen = strlen(pfx);
    if (strncmp(filename, pfx, plen) != 0)
        return filename; // not prefixed after all

    const char *p = filename + plen;
    // skip the separator util_concat_paths() inserted
    if (pfx[plen - 1] != '/' && pfx[plen - 1] != '\\' && (*p == '/' || *p == '\\'))
        p++;
    return p;
}

#endif // BUILD_MSX

mediatype_t MediaTypeROM::mount(fnFile *f, uint32_t disksize, fujiHost *host, const char *filename)
{
    Debug_printv("MediaTypeROM MOUNT %s (%lu bytes)\n",
                 filename ? filename : "?", (unsigned long)disksize);

    _disk_fileh = f;
    _disk_image_size = disksize;
    _disktype = MEDIATYPE_ROM;

#ifdef BUILD_MSX

    // No .cfg-sibling concept on MSX -- the mapper comes from rom_type_for()
    // below instead, so there's nothing here but the ROM push itself. (Doing
    // the sibling probe anyway would just cost two pointless TNFS round
    // trips per mount for a file that can never exist.)
    fnio::fseek(f, 0, SEEK_SET);
    if (!push_rom_msx(f, _disk_image_size, rom_type_for(filename)))
    {
        Debug_printv("MediaTypeROM: ROM push failed\n");
        return MEDIATYPE_UNKNOWN;
    }

#else

    // Push the .cfg sibling first so the mapping is known before the ROM's
    // CLOSE boots. Missing sibling: fine. Existing sibling that fails to
    // open/push: fail the mount -- booting without it produces hangs.
    if (host != nullptr && filename != nullptr)
    {
        char cfgpath[MAX_FILENAME_LEN];
        strlcpy(cfgpath, strip_host_prefix(host, filename), sizeof(cfgpath));

        // replace the basename's extension only
        char *base = cfgpath;
        for (char *p = cfgpath; *p != '\0'; p++)
            if (*p == '/' || *p == '\\')
                base = p + 1;
        char *dot = strrchr(base, '.');
        if (dot != nullptr)
            strlcpy(dot, ".cfg", sizeof(cfgpath) - (dot - cfgpath));
        else
            strlcat(cfgpath, ".cfg", sizeof(cfgpath));

        bool cfg_found = host->file_exists(cfgpath);
        if (!cfg_found)
        {
            // case-sensitive hosts may carry the sibling as .CFG
            size_t len = strlen(cfgpath);
            memcpy(cfgpath + len - 4, ".CFG", 4);
            cfg_found = host->file_exists(cfgpath);
        }

        if (!cfg_found)
        {
            // Not an error -- a .bin with no memory map boots against the
            // emulator's size-guess table -- but for the titles that need one
            // the result is a wrong map, i.e. a game that boots to garbage
            // with nothing anywhere saying why. Say it here.
            Debug_printv("MediaTypeROM: no .cfg sibling for %s (tried \"%s\" in both "
                         "casings) -- booting with a default memory map\n",
                         filename, cfgpath);
        }
        else
        {
            char resolved[MAX_FILENAME_LEN];
            strlcpy(resolved, cfgpath, sizeof(resolved));
            fnFile *cfgf = host->fnfile_open(cfgpath, resolved, sizeof(resolved), "rb");
            if (cfgf == nullptr)
            {
                Debug_printv("MediaTypeROM: .cfg sibling exists but failed to open: %s\n", cfgpath);
                return MEDIATYPE_UNKNOWN;
            }
            long cfgsize = host->file_size(cfgf);
            bool cfg_ok = cfgsize >= 0 &&
                          push_stream(cfgf, ROM_PUSH_STREAM_CFG, (uint32_t)cfgsize);
            fnio::fclose(cfgf);
            if (!cfg_ok)
            {
                Debug_printv("MediaTypeROM: .cfg push failed: %s\n", cfgpath);
                return MEDIATYPE_UNKNOWN;
            }
        }
    }

    fnio::fseek(f, 0, SEEK_SET);
    if (!push_stream(f, ROM_PUSH_STREAM_ROM, _disk_image_size))
    {
        Debug_printv("MediaTypeROM: ROM push failed\n");
        return MEDIATYPE_UNKNOWN;
    }

#endif // BUILD_MSX

    return _disktype;
}

#endif // BUILD_RS232
