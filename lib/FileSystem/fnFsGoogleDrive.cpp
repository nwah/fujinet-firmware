#include "fnFsGoogleDrive.h"

#include <ctime>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <vector>

#include "compat_string.h"

#include "../../include/debug.h"

#include "fnSystem.h"
#include "fnFileCache.h"

// Google Drive API constants
const char* FileSystemGoogleDrive::GDRIVE_API_BASE = "https://www.googleapis.com/drive/v3";
const char* FileSystemGoogleDrive::OAUTH_TOKEN_URL = "https://oauth2.googleapis.com/token";

// HTTP timeout in ms
#define HTTP_GET_TIMEOUT 30000
#define COPY_BLK_SIZE 4096

#ifdef ESP_PLATFORM
#define HEAP_DEBUG() Debug_printv("free heap/low: %lu/%lu", esp_get_free_heap_size(), esp_get_free_internal_heap_size())
#else
#define HEAP_DEBUG()
#endif

FileSystemGoogleDrive::FileSystemGoogleDrive()
{
    Debug_printf("FileSystemGoogleDrive::ctor\n");
    _http = nullptr;
    _json = nullptr;
    // invalidate _last_dir
    _last_dir[0] = '\0';
}

FileSystemGoogleDrive::~FileSystemGoogleDrive()
{
    Debug_printf("FileSystemGoogleDrive::dtor\n");
    if (_started)
    {
        _dircache.clear();
    }
    if (_http != nullptr)
        delete _http;
    if (_json != nullptr)
        delete _json;
}

bool FileSystemGoogleDrive::start(const char* client_id, const char* client_secret, const char* access_code)
{
    if (_started)
        return false;

    if (client_id == nullptr || client_secret == nullptr || access_code == nullptr)
        return false;

    _client_id = client_id;
    _client_secret = client_secret;
    _access_code = access_code;

    if (_http != nullptr)
    {
        delete _http;
        _http = nullptr;
    }

    if (_json != nullptr)
    {
        delete _json;
        _json = nullptr;
    }

    _http = new HTTP_CLIENT_CLASS();
    if (_http == nullptr)
    {
        Debug_println("FileSystemGoogleDrive::start() - failed to create HTTP client");
        return false;
    }

    _json = new FNJSON();
    if (_json == nullptr)
    {
        Debug_println("FileSystemGoogleDrive::start() - failed to create JSON parser");
        return false;
    }

    // Exchange OAuth access code for access token
    if (!exchange_oauth_code())
    {
        Debug_println("FileSystemGoogleDrive::start() - OAuth token exchange failed");
        return false;
    }

    Debug_println("FileSystemGoogleDrive started");
    return _started = true;
}

bool FileSystemGoogleDrive::exchange_oauth_code()
{
    std::string post_data = "code=" + url_encode(_access_code) +
                           "&client_id=" + url_encode(_client_id) +
                           "&client_secret=" + url_encode(_client_secret) +
                           "&redirect_uri=" + url_encode("urn:ietf:wg:oauth:2.0:oob") +
                           "&grant_type=authorization_code";

    if (!_http->begin(OAUTH_TOKEN_URL))
    {
        Debug_println("FileSystemGoogleDrive::exchange_oauth_code - failed to start HTTP client");
        return false;
    }

    _http->set_header("Content-Type", "application/x-www-form-urlencoded");
    
    int response_code = _http->POST(post_data.c_str(), post_data.length());
    if (response_code != 200)
    {
        Debug_printf("FileSystemGoogleDrive::exchange_oauth_code - POST failed with code %d\n", response_code);
        return false;
    }

    // Read response
    std::string response_body;
    int available;
    char buffer[512];
    
    while (_http->available() > 0)
    {
        available = _http->available();
        if (available > sizeof(buffer) - 1)
            available = sizeof(buffer) - 1;
        
        int bytes_read = _http->read((uint8_t*)buffer, available);
        if (bytes_read > 0)
        {
            buffer[bytes_read] = '\0';
            response_body += buffer;
        }
    }

    // Parse JSON response
    cJSON* json = cJSON_Parse(response_body.c_str());
    if (json == nullptr)
    {
        Debug_println("FileSystemGoogleDrive::exchange_oauth_code - failed to parse JSON response");
        return false;
    }

    cJSON* access_token = cJSON_GetObjectItem(json, "access_token");
    cJSON* refresh_token = cJSON_GetObjectItem(json, "refresh_token");
    
    if (access_token == nullptr || !cJSON_IsString(access_token))
    {
        Debug_println("FileSystemGoogleDrive::exchange_oauth_code - no access_token in response");
        cJSON_Delete(json);
        return false;
    }

    _access_token = access_token->valuestring;
    
    if (refresh_token != nullptr && cJSON_IsString(refresh_token))
    {
        _refresh_token = refresh_token->valuestring;
    }

    cJSON_Delete(json);
    Debug_println("FileSystemGoogleDrive::exchange_oauth_code - OAuth token exchange successful");
    return true;
}

bool FileSystemGoogleDrive::refresh_access_token()
{
    if (_refresh_token.empty())
        return false;

    std::string post_data = "refresh_token=" + url_encode(_refresh_token) +
                           "&client_id=" + url_encode(_client_id) +
                           "&client_secret=" + url_encode(_client_secret) +
                           "&grant_type=refresh_token";

    if (!_http->begin(OAUTH_TOKEN_URL))
    {
        Debug_println("FileSystemGoogleDrive::refresh_access_token - failed to start HTTP client");
        return false;
    }

    _http->set_header("Content-Type", "application/x-www-form-urlencoded");
    
    int response_code = _http->POST(post_data.c_str(), post_data.length());
    if (response_code != 200)
    {
        Debug_printf("FileSystemGoogleDrive::refresh_access_token - POST failed with code %d\n", response_code);
        return false;
    }

    // Read and parse response similar to exchange_oauth_code
    std::string response_body;
    int available;
    char buffer[512];
    
    while (_http->available() > 0)
    {
        available = _http->available();
        if (available > sizeof(buffer) - 1)
            available = sizeof(buffer) - 1;
        
        int bytes_read = _http->read((uint8_t*)buffer, available);
        if (bytes_read > 0)
        {
            buffer[bytes_read] = '\0';
            response_body += buffer;
        }
    }

    cJSON* json = cJSON_Parse(response_body.c_str());
    if (json == nullptr)
        return false;

    cJSON* access_token = cJSON_GetObjectItem(json, "access_token");
    if (access_token == nullptr || !cJSON_IsString(access_token))
    {
        cJSON_Delete(json);
        return false;
    }

    _access_token = access_token->valuestring;
    cJSON_Delete(json);
    return true;
}

std::string FileSystemGoogleDrive::get_auth_header()
{
    return "Bearer " + _access_token;
}

bool FileSystemGoogleDrive::make_api_request(const std::string& endpoint, const std::string& method, const std::string& body)
{
    std::string url = std::string(GDRIVE_API_BASE) + endpoint;
    
    if (!_http->begin(url))
    {
        Debug_printf("FileSystemGoogleDrive::make_api_request - failed to start HTTP client for %s\n", url.c_str());
        return false;
    }

    _http->set_header("Authorization", get_auth_header().c_str());
    
    int response_code;
    if (method == "GET")
    {
        response_code = _http->GET();
    }
    else if (method == "POST")
    {
        if (!body.empty())
            _http->set_header("Content-Type", "application/json");
        response_code = _http->POST(body.c_str(), body.length());
    }
    else if (method == "DELETE")
    {
        response_code = _http->DELETE();
    }
    else
    {
        Debug_printf("FileSystemGoogleDrive::make_api_request - unsupported method %s\n", method.c_str());
        return false;
    }

    if (response_code == 401)
    {
        // Try refreshing access token once
        if (refresh_access_token())
        {
            _http->set_header("Authorization", get_auth_header().c_str());
            if (method == "GET")
                response_code = _http->GET();
            else if (method == "POST")
                response_code = _http->POST(body.c_str(), body.length());
            else if (method == "DELETE")
                response_code = _http->DELETE();
        }
    }

    return (response_code >= 200 && response_code < 300);
}

std::vector<std::string> FileSystemGoogleDrive::split_path(const char* path)
{
    std::vector<std::string> components;
    if (path == nullptr || path[0] == '\0')
        return components;

    std::string path_str(path);
    if (path_str[0] == '/')
        path_str = path_str.substr(1);

    std::stringstream ss(path_str);
    std::string component;
    
    while (std::getline(ss, component, '/'))
    {
        if (!component.empty())
            components.push_back(component);
    }
    
    return components;
}

std::string FileSystemGoogleDrive::join_path(const std::vector<std::string>& components)
{
    if (components.empty())
        return "/";
    
    std::string result = "";
    for (const auto& component : components)
    {
        result += "/" + component;
    }
    return result;
}

std::string FileSystemGoogleDrive::get_folder_id(const char* path)
{
    if (path == nullptr || path[0] == '\0' || strcmp(path, "/") == 0)
        return "root";

    std::vector<std::string> components = split_path(path);
    std::string current_folder_id = "root";

    for (const auto& component : components)
    {
        std::string query = "/files?q=" + url_encode("name='" + component + "' and '" + current_folder_id + "' in parents and mimeType='application/vnd.google-apps.folder' and trashed=false");
        
        if (!make_api_request(query))
            return "";

        // Parse response to get folder ID
        std::string response_body;
        int available;
        char buffer[1024];
        
        while (_http->available() > 0)
        {
            available = _http->available();
            if (available > sizeof(buffer) - 1)
                available = sizeof(buffer) - 1;
            
            int bytes_read = _http->read((uint8_t*)buffer, available);
            if (bytes_read > 0)
            {
                buffer[bytes_read] = '\0';
                response_body += buffer;
            }
        }

        cJSON* json = cJSON_Parse(response_body.c_str());
        if (json == nullptr)
            return "";

        cJSON* files = cJSON_GetObjectItem(json, "files");
        if (!cJSON_IsArray(files) || cJSON_GetArraySize(files) == 0)
        {
            cJSON_Delete(json);
            return "";
        }

        cJSON* first_file = cJSON_GetArrayItem(files, 0);
        cJSON* id = cJSON_GetObjectItem(first_file, "id");
        if (!cJSON_IsString(id))
        {
            cJSON_Delete(json);
            return "";
        }

        current_folder_id = id->valuestring;
        cJSON_Delete(json);
    }

    return current_folder_id;
}

std::string FileSystemGoogleDrive::get_file_id(const char* path)
{
    std::vector<std::string> components = split_path(path);
    if (components.empty())
        return "";

    std::string filename = components.back();
    components.pop_back();
    
    std::string parent_folder_id = get_folder_id(join_path(components).c_str());
    if (parent_folder_id.empty())
        return "";

    std::string query = "/files?q=" + url_encode("name='" + filename + "' and '" + parent_folder_id + "' in parents and trashed=false");
    
    if (!make_api_request(query))
        return "";

    // Parse response to get file ID
    std::string response_body;
    int available;
    char buffer[1024];
    
    while (_http->available() > 0)
    {
        available = _http->available();
        if (available > sizeof(buffer) - 1)
            available = sizeof(buffer) - 1;
        
        int bytes_read = _http->read((uint8_t*)buffer, available);
        if (bytes_read > 0)
        {
            buffer[bytes_read] = '\0';
            response_body += buffer;
        }
    }

    cJSON* json = cJSON_Parse(response_body.c_str());
    if (json == nullptr)
        return "";

    cJSON* files = cJSON_GetObjectItem(json, "files");
    if (!cJSON_IsArray(files) || cJSON_GetArraySize(files) == 0)
    {
        cJSON_Delete(json);
        return "";
    }

    cJSON* first_file = cJSON_GetArrayItem(files, 0);
    cJSON* id = cJSON_GetObjectItem(first_file, "id");
    if (!cJSON_IsString(id))
    {
        cJSON_Delete(json);
        return "";
    }

    std::string file_id = id->valuestring;
    cJSON_Delete(json);
    return file_id;
}

std::string FileSystemGoogleDrive::url_encode(const std::string& value)
{
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;

    for (char c : value)
    {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
        {
            escaped << c;
        }
        else
        {
            escaped << '%' << std::setw(2) << int((unsigned char)c);
        }
    }

    return escaped.str();
}

bool FileSystemGoogleDrive::exists(const char* path)
{
    if (!_started)
        return false;

    return !get_file_id(path).empty();
}

bool FileSystemGoogleDrive::remove(const char* path)
{
    if (!_started)
        return false;

    std::string file_id = get_file_id(path);
    if (file_id.empty())
        return false;

    return make_api_request("/files/" + file_id, "DELETE");
}

bool FileSystemGoogleDrive::rename(const char* pathFrom, const char* pathTo)
{
    if (!_started)
        return false;

    std::string file_id = get_file_id(pathFrom);
    if (file_id.empty())
        return false;

    std::vector<std::string> to_components = split_path(pathTo);
    if (to_components.empty())
        return false;

    std::string new_name = to_components.back();
    
    std::string json_body = "{\"name\":\"" + new_name + "\"}";
    
    return make_api_request("/files/" + file_id, "POST", json_body);
}

FILE* FileSystemGoogleDrive::file_open(const char* path, const char* mode)
{
    Debug_printf("FileSystemGoogleDrive::file_open() - ERROR! Use filehandler_open() instead\n");
    return nullptr;
}

#ifndef FNIO_IS_STDIO
FileHandler* FileSystemGoogleDrive::filehandler_open(const char* path, const char* mode)
{
    FileHandler* fh = cache_file(path, mode);
    return fh;
}

FileHandler* FileSystemGoogleDrive::cache_file(const char* path, const char* mode)
{
    if (!_started)
        return nullptr;

    // Try cache first
    FileHandler* fh = FileCache::open("googledrive://", path, mode);
    if (fh != nullptr)
        return fh; // cache hit

    HEAP_DEBUG();

    // Create new cache file
    fc_handle* fc = FileCache::create("googledrive://", path);
    if (fc == nullptr)
        return nullptr;

    std::string file_id = get_file_id(path);
    if (file_id.empty())
    {
        FileCache::remove(fc);
        return nullptr;
    }

    // Download file from Google Drive
    std::string download_url = std::string(GDRIVE_API_BASE) + "/files/" + file_id + "?alt=media";
    
    if (!_http->begin(download_url))
    {
        Debug_println("FileSystemGoogleDrive::cache_file - failed to start HTTP client");
        FileCache::remove(fc);
        return nullptr;
    }

    _http->set_header("Authorization", get_auth_header().c_str());
    
    if (_http->GET() > 399)
    {
        Debug_println("FileSystemGoogleDrive::cache_file - GET failed");
        FileCache::remove(fc);
        return nullptr;
    }

    // Download file data
    int tmout_counter = 1 + HTTP_GET_TIMEOUT / 50;
    bool cancel = false;
    int available;

#ifdef ESP_PLATFORM
    uint8_t* buf = (uint8_t*)heap_caps_malloc(COPY_BLK_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    uint8_t* buf = (uint8_t*)malloc(COPY_BLK_SIZE);
#endif
    if (buf == nullptr)
    {
        Debug_println("FileSystemGoogleDrive::cache_file - failed to allocate buffer");
        FileCache::remove(fc);
        return nullptr;
    }

    Debug_println("Downloading file data");
    while (!cancel)
    {
        available = _http->available();
        if (_http->is_transaction_done() && available == 0)
            break;

        if (available == 0)
        {
            if (--tmout_counter == 0)
            {
                Debug_println("FileSystemGoogleDrive::cache_file - Timeout");
                cancel = true;
                break;
            }
            fnSystem.delay(50);
        }
        else if (available > 0)
        {
            while (available > 0)
            {
                int to_read = (available > COPY_BLK_SIZE) ? COPY_BLK_SIZE : available;
                int from_read = _http->read(buf, to_read);
                if (from_read != to_read)
                {
                    Debug_println("FileSystemGoogleDrive::cache_file - HTTP read failed");
                    cancel = true;
                    break;
                }
                
                if (FileCache::write(fc, buf, to_read) < to_read)
                {
                    Debug_printf("FileSystemGoogleDrive::cache_file - Cache write failed\n");
                    cancel = true;
                    break;
                }
                
                available = _http->available();
            }
            tmout_counter = 1 + HTTP_GET_TIMEOUT / 50;
        }
        else if (available < 0)
        {
            Debug_println("FileSystemGoogleDrive::cache_file - something went wrong");
            cancel = true;
        }
    }

    free(buf);

    if (cancel)
    {
        Debug_println("Download cancelled");
        FileCache::remove(fc);
        fh = nullptr;
    }
    else
    {
        Debug_println("File downloaded successfully");
        fh = FileCache::reopen(fc, mode);
    }

    HEAP_DEBUG();
    return fh;
}
#endif // !FNIO_IS_STDIO

bool FileSystemGoogleDrive::is_dir(const char* path)
{
    if (!_started)
        return false;

    return !get_folder_id(path).empty();
}

bool FileSystemGoogleDrive::mkdir(const char* path)
{
    if (!_started)
        return false;

    std::vector<std::string> components = split_path(path);
    if (components.empty())
        return false;

    std::string folder_name = components.back();
    components.pop_back();
    
    std::string parent_folder_id = get_folder_id(join_path(components).c_str());
    if (parent_folder_id.empty())
        return false;

    std::string json_body = "{\"name\":\"" + folder_name + "\",\"mimeType\":\"application/vnd.google-apps.folder\",\"parents\":[\"" + parent_folder_id + "\"]}";
    
    return make_api_request("/files", "POST", json_body);
}

bool FileSystemGoogleDrive::rmdir(const char* path)
{
    if (!_started)
        return false;

    std::string folder_id = get_folder_id(path);
    if (folder_id.empty() || folder_id == "root")
        return false;

    return make_api_request("/files/" + folder_id, "DELETE");
}

bool FileSystemGoogleDrive::dir_exists(const char* path)
{
    return is_dir(path);
}

bool FileSystemGoogleDrive::dir_open(const char* path, const char* pattern, uint16_t diropts)
{
    if (!_started)
        return false;

    Debug_printf("FileSystemGoogleDrive::dir_open(\"%s\", \"%s\", %u)\n", path ? path : "", pattern ? pattern : "", diropts);
    HEAP_DEBUG();

    if (path == nullptr)
        return false;

    if (strcmp(_last_dir, path) == 0 && !_dircache.empty())
    {
        Debug_printf("Use directory cache\n");
    }
    else
    {
        Debug_printf("Fill directory cache\n");
        _dircache.clear();
        _last_dir[0] = '\0';

        std::string folder_id = get_folder_id(path);
        if (folder_id.empty())
            return false;

        std::string query = "/files?q=" + url_encode("'" + folder_id + "' in parents and trashed=false") + "&fields=files(id,name,mimeType,size,modifiedTime)";
        
        if (!make_api_request(query))
            return false;

        // Read response
        std::string response_body;
        int available;
        char buffer[4096];
        
        while (_http->available() > 0)
        {
            available = _http->available();
            if (available > sizeof(buffer) - 1)
                available = sizeof(buffer) - 1;
            
            int bytes_read = _http->read((uint8_t*)buffer, available);
            if (bytes_read > 0)
            {
                buffer[bytes_read] = '\0';
                response_body += buffer;
            }
        }

        // Parse JSON response
        cJSON* json = cJSON_Parse(response_body.c_str());
        if (json == nullptr)
            return false;

        cJSON* files = cJSON_GetObjectItem(json, "files");
        if (!cJSON_IsArray(files))
        {
            cJSON_Delete(json);
            return false;
        }

        // Remember last visited directory
        strlcpy(_last_dir, path, MAX_PATHLEN);

        // Process each file entry
        int num_files = cJSON_GetArraySize(files);
        for (int i = 0; i < num_files; i++)
        {
            cJSON* file = cJSON_GetArrayItem(files, i);
            cJSON* name = cJSON_GetObjectItem(file, "name");
            cJSON* mimeType = cJSON_GetObjectItem(file, "mimeType");
            cJSON* size = cJSON_GetObjectItem(file, "size");
            cJSON* modifiedTime = cJSON_GetObjectItem(file, "modifiedTime");

            if (!cJSON_IsString(name))
                continue;

            fsdir_entry* fs_de = &_dircache.new_entry();
            
            // File name
            strlcpy(fs_de->filename, name->valuestring, sizeof(fs_de->filename));
            
            // Check if it's a directory
            fs_de->isDir = (cJSON_IsString(mimeType) && 
                           strcmp(mimeType->valuestring, "application/vnd.google-apps.folder") == 0);
            
            // File size
            if (cJSON_IsString(size))
            {
                fs_de->size = static_cast<uint32_t>(std::stoull(size->valuestring));
            }
            else
            {
                fs_de->size = 0;
            }
            
            // Modified time
            fs_de->modified_time = 0;
            if (cJSON_IsString(modifiedTime))
            {
                struct tm tm;
                memset(&tm, 0, sizeof(struct tm));
                std::istringstream ss(modifiedTime->valuestring);
                ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
                if (!ss.fail())
                {
                    tm.tm_isdst = -1;
                    fs_de->modified_time = mktime(&tm);
                }
            }

            if (fs_de->isDir)
            {
                Debug_printf(" add entry: \"%s\"\tDIR\n", fs_de->filename);
            }
            else
            {
                Debug_printf(" add entry: \"%s\"\t%lu\n", fs_de->filename, fs_de->size);
            }
        }

        cJSON_Delete(json);
    }

    // Apply pattern matching filter and sort entries
    _dircache.apply_filter(pattern, diropts);

    HEAP_DEBUG();
    return true;
}

fsdir_entry* FileSystemGoogleDrive::dir_read()
{
    return _dircache.read();
}

void FileSystemGoogleDrive::dir_close()
{
    // _dircache.clear();
}

uint16_t FileSystemGoogleDrive::dir_tell()
{
    return _dircache.tell();
}

bool FileSystemGoogleDrive::dir_seek(uint16_t pos)
{
    return _dircache.seek(pos);
}