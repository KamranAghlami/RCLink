#include "server.h"

#include <sys/stat.h>

#include <esp_err.h>
#include <esp_log.h>
#include <esp_http_server.h>

constexpr const char *TAG = "server";

struct file_server
{
    httpd_handle_t httpd_handle;
    std::string base_path;
};

struct websocket_server
{
    httpd_handle_t httpd_handle;
};

struct server_implementation
{
    file_server file_server_ctx;
    websocket_server websocket_server_ctx;
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

    if (!strcmp(file_extension, "html"))
        content_type = "text/html";
    else if (!strcmp(file_extension, "css"))
        content_type = "text/css";
    else if (!strcmp(file_extension, "js"))
        content_type = "application/javascript";
    else if (!strcmp(file_extension, "wasm"))
        content_type = "application/wasm";
    else if (!strcmp(file_extension, "png"))
        content_type = "image/png";
    else if (!strcmp(file_extension, "svg"))
        content_type = "image/svg+xml";
    else if (!strcmp(file_extension, "ico"))
        content_type = "image/x-icon";
    else if (!strcmp(file_extension, "bin"))
        content_type = "application/octet-stream";

    return httpd_resp_set_type(request, content_type);
}

static esp_err_t get_index_handler(httpd_req_t *request)
{
    httpd_resp_set_status(request, "307 Temporary Redirect");
    httpd_resp_set_hdr(request, "Location", "/");
    httpd_resp_send(request, NULL, 0);

    return ESP_OK;
}

static esp_err_t get_handler(httpd_req_t *request)
{
    const auto server_impl = static_cast<file_server *>(request->user_ctx);
    const auto base_path_length = server_impl->base_path.size();
    char file_path[CONFIG_LITTLEFS_OBJ_NAME_LEN] = {0};

    file_path_from_uri(request->uri, server_impl->base_path.c_str(), file_path, sizeof(file_path));

    if (!*file_path)
    {
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, nullptr);

        return ESP_FAIL;
    }

    const char *file_name = file_path + base_path_length;

    if (!strcmp(file_name, "/"))
        strcat(file_path, "index.html");
    else if (!strcmp(file_name, "/index.html"))
        return get_index_handler(request);

    {
        struct stat file_stat;

        if (file_path[strlen(file_path) - 1] == '/' || stat(file_path, &file_stat))
        {
            httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, nullptr);

            ESP_LOGW(TAG, "not found! file_path: %s", file_path);

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

static esp_err_t ws_handler(httpd_req_t *request)
{
    return ESP_FAIL;
}

server::server(const uint16_t port, const std::string &base_path) : mp_implementation(std::make_unique<server_implementation>())
{
    mp_implementation->file_server_ctx.base_path = base_path;

    httpd_config_t httpd_config = HTTPD_DEFAULT_CONFIG();

    httpd_config.core_id = 1;
    httpd_config.server_port = port;
    httpd_config.uri_match_fn = httpd_uri_match_wildcard;

    ESP_ERROR_CHECK(httpd_start(&mp_implementation->file_server_ctx.httpd_handle, &httpd_config));

    const httpd_uri_t get = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = get_handler,
        .user_ctx = &mp_implementation.get()->file_server_ctx,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr,
    };

    ESP_ERROR_CHECK(httpd_register_uri_handler(mp_implementation->file_server_ctx.httpd_handle, &get));

    httpd_config_t httpd_ws_config = HTTPD_DEFAULT_CONFIG();

    httpd_ws_config.core_id = 1;
    httpd_ws_config.server_port = httpd_config.server_port + 1;
    httpd_ws_config.ctrl_port = httpd_config.ctrl_port + 1;

    ESP_ERROR_CHECK(httpd_start(&mp_implementation->websocket_server_ctx.httpd_handle, &httpd_ws_config));

    const httpd_uri_t ws_get = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = ws_handler,
        .user_ctx = &mp_implementation.get()->websocket_server_ctx,
        .is_websocket = true,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr,
    };

    ESP_ERROR_CHECK(httpd_register_uri_handler(mp_implementation->websocket_server_ctx.httpd_handle, &ws_get));
}

server::~server()
{
    ESP_ERROR_CHECK(httpd_stop(mp_implementation->websocket_server_ctx.httpd_handle));
    ESP_ERROR_CHECK(httpd_stop(mp_implementation->websocket_server_ctx.httpd_handle));
}
