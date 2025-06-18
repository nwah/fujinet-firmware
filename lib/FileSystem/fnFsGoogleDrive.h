#ifndef FN_FSGOOGLEDRIVE_H
#define FN_FSGOOGLEDRIVE_H

#include <cstddef>
#include <stdint.h>
#include <string>

#ifdef ESP_PLATFORM
#include "fnHttpClient.h"
#define HTTP_CLIENT_CLASS fnHttpClient
#else
#include "mgHttpClient.h"
#define HTTP_CLIENT_CLASS mgHttpClient
#endif

#include "fnFS.h"
#include "fnDirCache.h"
#include "fnjson.h"

struct GoogleDriveFileEntry {
    std::string id;
    std::string name;
    std::string mimeType;
    uint64_t size;
    std::string modifiedTime;
    bool isFolder;
    std::string parentId;
};

class FileSystemGoogleDrive : public FileSystem
{
private:
    // OAuth credentials
    std::string _client_id;
    std::string _client_secret;
    std::string _access_code;
    std::string _access_token;
    std::string _refresh_token;

    // HTTP client
    HTTP_CLIENT_CLASS *_http;

    // JSON parser
    FNJSON *_json;

    // directory cache
    char _last_dir[MAX_PATHLEN];
    DirCache _dircache;

    // Google Drive API base URL
    static const char* GDRIVE_API_BASE;
    static const char* OAUTH_TOKEN_URL;

    // Internal methods
    bool exchange_oauth_code();
    bool refresh_access_token();
    std::string get_auth_header();
    bool make_api_request(const std::string& endpoint, const std::string& method = "GET", const std::string& body = "");
    std::string get_folder_id(const char* path);
    std::string get_file_id(const char* path);
    bool list_files_in_folder(const std::string& folder_id);
    bool download_file(const std::string& file_id, const char* local_path);
    bool upload_file(const char* local_path, const std::string& parent_folder_id, const std::string& filename);
    bool delete_file(const std::string& file_id);
    bool create_folder(const std::string& name, const std::string& parent_folder_id);
    std::string url_encode(const std::string& value);
    std::string build_file_list_query(const std::string& folder_id);

public:
    FileSystemGoogleDrive();
    ~FileSystemGoogleDrive();

    bool start(const char* client_id, const char* client_secret, const char* access_code);

    fsType type() override { return FSTYPE_GOOGLEDRIVE; };
    const char *typestring() override { return type_to_string(FSTYPE_GOOGLEDRIVE); };

    FILE *file_open(const char *path, const char *mode = FILE_READ) override;
#ifndef FNIO_IS_STDIO
    FileHandler *filehandler_open(const char *path, const char *mode = FILE_READ) override;
#endif

    bool exists(const char *path) override;

    bool remove(const char *path) override;

    bool rename(const char *pathFrom, const char *pathTo) override;

    bool is_dir(const char *path) override;
    bool mkdir(const char* path) override;
    bool rmdir(const char* path) override;
    bool dir_exists(const char* path) override;

    bool dir_open(const char *path, const char *pattern, uint16_t diropts) override;
    fsdir_entry *dir_read() override;
    void dir_close() override;
    uint16_t dir_tell() override;
    bool dir_seek(uint16_t pos) override;

#ifndef FNIO_IS_STDIO
    FileHandler *cache_file(const char *path, const char *mode);
#endif

private:
    // Path parsing helpers
    std::vector<std::string> split_path(const char* path);
    std::string join_path(const std::vector<std::string>& components);
};

#endif // FN_FSGOOGLEDRIVE_H