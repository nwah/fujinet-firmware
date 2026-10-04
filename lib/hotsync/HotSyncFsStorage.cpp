#include "HotSyncFsStorage.h"

#include "fnFS.h"

#include "../../include/debug.h"

std::string HotSyncFsStorage::path(const std::string &folder, const std::string &file_name) const
{
    std::string p = _root + "/" + folder;
    if (!file_name.empty())
        p += "/" + file_name;
    return p;
}

// Uses is_dir(): dir_exists() is a stub that answers true on most filesystems.
success_is_true HotSyncFsStorage::ensure_dir(const std::string &dir)
{
    if (_fs.is_dir(dir.c_str()))
        RETURN_SUCCESS_AS_TRUE();
    // mkdir is not recursive, so build each level in turn.
    for (size_t slash = dir.find('/', 1); ; slash = dir.find('/', slash + 1))
    {
        std::string level = dir.substr(0, slash);
        if (!_fs.is_dir(level.c_str()) && _fs.mkdir(level.c_str()).is_error())
            RETURN_ERROR_AS_FALSE();
        if (slash == std::string::npos)
            break;
    }
    RETURN_SUCCESS_AS_TRUE();
}

success_is_true HotSyncFsStorage::create_install_folder()
{
    return ensure_dir(path("install"));
}

std::vector<std::string> HotSyncFsStorage::pending_installs()
{
    std::vector<std::string> names;
    std::string dir = path("install");
    ensure_dir(dir);
    if (_fs.dir_open(dir.c_str(), "", 0).is_error())
        return names;
    while (fsdir_entry_t *entry = _fs.dir_read())
        if (!entry->isDir)
            names.push_back(entry->filename);
    _fs.dir_close();
    return names;
}

success_is_true HotSyncFsStorage::read_install(const std::string &file_name, ByteBuffer &out)
{
    FILE *f = _fs.file_open(path("install", file_name).c_str(), FILE_READ);
    if (f == nullptr)
        RETURN_ERROR_AS_FALSE();
    long size = FileSystem::filesize(f);
    out.resize(size > 0 ? size : 0);
    size_t got = fread(out.data(), 1, out.size(), f);
    fclose(f);
    RETURN_SUCCESS_IF(size > 0 && got == out.size());
}

success_is_true HotSyncFsStorage::mark_installed(const std::string &file_name)
{
    std::string done = path("installed", file_name);
    if (ensure_dir(path("installed")).is_error())
        RETURN_ERROR_AS_FALSE();
    if (_fs.exists(done.c_str()))
        _fs.remove(done.c_str());
    return _fs.rename(path("install", file_name).c_str(), done.c_str());
}

success_is_true HotSyncFsStorage::write_backup(const std::string &user, const std::string &file_name,
                                               const ByteBuffer &data)
{
    std::string dir = path("backup/" + user);
    if (ensure_dir(dir).is_error())
        RETURN_ERROR_AS_FALSE();
    FILE *f = _fs.file_open((dir + "/" + file_name).c_str(), FILE_WRITE);
    if (f == nullptr)
        RETURN_ERROR_AS_FALSE();
    size_t wrote = fwrite(data.data(), 1, data.size(), f);
    fclose(f);
    RETURN_SUCCESS_IF(wrote == data.size());
}
