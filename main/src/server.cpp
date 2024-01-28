#include "server.h"

#include <sys/stat.h>

#include <esp_err.h>
#include <esp_log.h>
#include <esp_http_server.h>

constexpr const char *TAG = "server";

struct server_implementation
{
    std::string base_path;
    httpd_handle_t httpd_handle;
};

static void file_path_from_uri(const char *uri, const char *base_path, char *file_path, const size_t file_path_max)
{
    const size_t base_path_length = strlen(base_path);
    size_t uri_length = strlen(uri);

    const char *questio_mark = strchr(uri, '?');

    if (questio_mark)
        uri_length = std::min(uri_length, static_cast<size_t>(questio_mark - uri));

    const char *hash_symbol = strchr(uri, '#');

    if (hash_symbol)
        uri_length = std::min(uri_length, static_cast<size_t>(hash_symbol - uri));

    if (base_path_length + uri_length + 1 > file_path_max)
        return;

    strcpy(file_path, base_path);
    strcat(file_path + base_path_length, uri);
}

static esp_err_t add_content_type(httpd_req_t *request, const char *file_path)
{
    char *file_name = strrchr(file_path, '/');

    if (!file_name)
        return ESP_FAIL;

    file_name++;

    char *file_extension = strchr(file_name, '.');

    if (!file_extension)
        return httpd_resp_set_type(request, "application/octet-stream");

    file_extension++;

    const char *content_type = "text/plain";

    if (strcmp(file_extension, "html"))
        content_type = "text/html";
    else if (strcmp(file_extension, "css"))
        content_type = "text/css";
    else if (strcmp(file_extension, "js"))
        content_type = "application/javascript";
    else if (strcmp(file_extension, "wasm"))
        content_type = "application/wasm";
    else if (strcmp(file_extension, "png"))
        content_type = "image/png";
    else if (strcmp(file_extension, "ico"))
        content_type = "image/x-icon";
    else if (strcmp(file_extension, "bin"))
        content_type = "application/octet-stream";

    return httpd_resp_set_type(request, content_type);
}

static esp_err_t get_handler(httpd_req_t *request)
{
    auto server_impl = static_cast<server_implementation *>(request->user_ctx);
    char file_path[CONFIG_LITTLEFS_OBJ_NAME_LEN] = {0};

    file_path_from_uri(request->uri, server_impl->base_path.c_str(), file_path, sizeof(file_path));

    const size_t file_path_length = strlen(file_path);

    if (!file_path_length || file_path[file_path_length - 1] == '/')
    {
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, nullptr);

        return ESP_FAIL;
    }

    {
        struct stat file_stat;

        if (stat(file_path, &file_stat))
        {
            httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, nullptr);

            return ESP_FAIL;
        }
    }

    FILE *file = fopen(file_path, "r");

    if (!file)
    {
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, nullptr);

        return ESP_FAIL;
    }

    if (auto error = add_content_type(request, file_path) != ESP_OK)
        return error;

    uint8_t buffer[1024U];
    size_t read_bytes = 0;

    do
    {
        read_bytes = fread(buffer, 1, sizeof(buffer), file);

        if (read_bytes > 0)
            if (httpd_resp_send_chunk(request, reinterpret_cast<char *>(buffer), read_bytes) != ESP_OK)
            {
                fclose(file);

                httpd_resp_send_chunk(request, NULL, 0);
                httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, nullptr);

                return ESP_FAIL;
            }
    } while (read_bytes);

    fclose(file);

    httpd_resp_send_chunk(request, NULL, 0);

    return ESP_OK;
}

static esp_err_t get_index_handler(httpd_req_t *request)
{
    httpd_resp_set_status(request, "307 Temporary Redirect");
    httpd_resp_set_hdr(request, "Location", "/");
    httpd_resp_send(request, NULL, 0);

    return ESP_OK;
}

server::server(const uint16_t port, const std::string &base_path) : mp_implementation(std::make_unique<server_implementation>())
{
    mp_implementation->base_path = base_path;

    httpd_config_t httpd_config = HTTPD_DEFAULT_CONFIG();

    httpd_config.core_id = 1;
    httpd_config.uri_match_fn = httpd_uri_match_wildcard;

    ESP_ERROR_CHECK(httpd_start(&mp_implementation->httpd_handle, &httpd_config));

    const httpd_uri_t get = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = get_handler,
        .user_ctx = mp_implementation.get(),
    };

    ESP_ERROR_CHECK(httpd_register_uri_handler(mp_implementation->httpd_handle, &get));

    // const httpd_uri_t get_index = {
    //     .uri = "/index.html",
    //     .method = HTTP_GET,
    //     .handler = get_index_handler,
    //     .user_ctx = nullptr,
    // };

    // ESP_ERROR_CHECK(httpd_register_uri_handler(mp_implementation->httpd_handle, &get_index));
}

server::~server()
{
    ESP_ERROR_CHECK(httpd_stop(mp_implementation->httpd_handle));
}
