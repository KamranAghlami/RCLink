#include "server.h"

#include <sys/stat.h>

#include <esp_err.h>
#include <esp_log.h>
#include <esp_http_server.h>

constexpr const char *TAG = "server";
constexpr const UBaseType_t SERVER_CORE_ID = 1U;
constexpr const UBaseType_t SERVER_PRIORITY = 5U;
constexpr const UBaseType_t WORKER_COUNT = 4U;
constexpr const uint32_t WORKER_STACK_SIZE = 4U * 1024U;

typedef esp_err_t (*request_handler)(httpd_req_t *);

struct request_context
{
    httpd_req_t *request;
    request_handler handler;
};

struct file_server_context
{
    httpd_handle_t httpd_handle;
    SemaphoreHandle_t workers_semaphore;
    QueueHandle_t requests_queue;
    TaskHandle_t workers[WORKER_COUNT];
    std::string base_path;
    bool is_running;
};

struct websocket_server_context
{
    httpd_handle_t httpd_handle;
};

struct server_implementation
{
    file_server_context file_server;
    websocket_server_context websocket_server;
};

static void request_worker_task(void *argument)
{
    const auto file_server = static_cast<file_server_context *>(argument);

    while (true)
    {
        xSemaphoreGive(file_server->workers_semaphore);

        if (!file_server->is_running)
            break;

        request_context request;

        if (xQueueReceive(file_server->requests_queue, &request, portMAX_DELAY))
        {
            request.handler(request.request);

            httpd_req_async_handler_complete(request.request);
        }
    }

    vTaskDelete(nullptr);
}

static void start_workers(file_server_context &file_server)
{
    file_server.is_running = true;
    file_server.workers_semaphore = xSemaphoreCreateCounting(WORKER_COUNT, 0);
    file_server.requests_queue = xQueueCreate(WORKER_COUNT, sizeof(request_context));

    for (size_t i = 0; i < WORKER_COUNT; i++)
        xTaskCreatePinnedToCore(request_worker_task, "request_worker",
                                WORKER_STACK_SIZE,
                                &file_server,
                                SERVER_PRIORITY,
                                &file_server.workers[i],
                                SERVER_CORE_ID);
}

static void stop_workers(file_server_context &file_server)
{
    file_server.is_running = false;

    request_context request;

    while (xQueueReceive(file_server.requests_queue, &request, pdMS_TO_TICKS(100)))
        httpd_req_async_handler_complete(request.request);

    for (size_t i = 0; i < WORKER_COUNT; i++)
    {
        request = {
            .request = nullptr,
            .handler = [](httpd_req_t *) -> esp_err_t
            { return ESP_OK; },
        };

        xQueueSend(file_server.requests_queue, &request, portMAX_DELAY);
    }

    while (uxSemaphoreGetCount(file_server.workers_semaphore) != WORKER_COUNT)
        vTaskDelay(pdMS_TO_TICKS(100));

    vQueueDelete(file_server.requests_queue);
    vSemaphoreDelete(file_server.workers_semaphore);
}

static bool is_on_worker(const file_server_context &file_server)
{
    TaskHandle_t current = xTaskGetCurrentTaskHandle();

    for (size_t i = 0; i < WORKER_COUNT; i++)
        if (current == file_server.workers[i])
            return true;

    return false;
}

static esp_err_t submit_work(const file_server_context &file_server, httpd_req_t *request, request_handler handler)
{
    xSemaphoreTake(file_server.workers_semaphore, portMAX_DELAY);

    request_context request_ctx = {
        .request = nullptr,
        .handler = handler,
    };

    if (auto error = httpd_req_async_handler_begin(request, &request_ctx.request) != ESP_OK)
        return error;

    xQueueSend(file_server.requests_queue, &request_ctx, portMAX_DELAY);

    return ESP_OK;
}

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

static esp_err_t get_index_handler(httpd_req_t *request)
{
    httpd_resp_set_status(request, "307 Temporary Redirect");
    httpd_resp_set_hdr(request, "Location", "/");
    httpd_resp_send(request, nullptr, 0);

    return ESP_OK;
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

static esp_err_t get_handler(httpd_req_t *request)
{
    const auto file_server = static_cast<file_server_context *>(request->user_ctx);

    if (!is_on_worker(*file_server))
    {
        if (file_server->is_running)
            return submit_work(*file_server, request, get_handler);
        else
            return ESP_FAIL;
    }

    const auto base_path_length = file_server->base_path.size();
    char file_path[CONFIG_LITTLEFS_OBJ_NAME_LEN] = {0};

    file_path_from_uri(request->uri, file_server->base_path.c_str(), file_path, sizeof(file_path));

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

    ESP_LOGI(TAG, "sending: %s", file_path);

    FILE *file = fopen(file_path, "r");

    if (!file)
    {
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, nullptr);

        return ESP_FAIL;
    }

    if (add_content_type(request, file_path) != ESP_OK)
        return ESP_FAIL;

    {
        uint8_t buffer[1024U];
        size_t read_bytes = 0;

        do
        {
            read_bytes = fread(buffer, 1, sizeof(buffer), file);

            if (read_bytes > 0)
                if (httpd_resp_send_chunk(request, reinterpret_cast<char *>(buffer), read_bytes) != ESP_OK)
                {
                    fclose(file);

                    httpd_resp_send_chunk(request, nullptr, 0);
                    httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, nullptr);

                    return ESP_FAIL;
                }
        } while (read_bytes);
    }

    fclose(file);

    httpd_resp_send_chunk(request, nullptr, 0);

    ESP_LOGI(TAG, "sent: %s", file_path);

    return ESP_OK;
}

static esp_err_t ws_handler(httpd_req_t *request)
{
    if (request->method == HTTP_GET)
        return ESP_OK;

    httpd_ws_frame_t ws_frame = {};

    if (httpd_ws_recv_frame(request, &ws_frame, 0) != ESP_OK)
        return ESP_FAIL;

    uint8_t buffer[8] = {0};

    if (ws_frame.len)
    {
        if (ws_frame.len + 1 > sizeof(buffer))
            return ESP_FAIL;

        ws_frame.payload = buffer;

        if (httpd_ws_recv_frame(request, &ws_frame, ws_frame.len) != ESP_OK)
            return ESP_FAIL;

        ESP_LOGI(TAG, "new frame! type: %d, size: %zu, value: %02x %02x %02x %02x",
                 ws_frame.type, ws_frame.len, ws_frame.payload[0], ws_frame.payload[1], ws_frame.payload[2], ws_frame.payload[3]);
    }

    if (buffer[0] == '\x0d')
    {
        buffer[0] = '\x0a';
        buffer[1] = '\x0d';
        ws_frame.len = 2;
    }

    if (buffer[0] == '\x7f')
    {
        buffer[0] = '\x08';
        buffer[1] = ' ';
        buffer[2] = '\x08';
        ws_frame.len = 3;
    }

    if (httpd_ws_send_frame(request, &ws_frame) != ESP_OK)
        return ESP_FAIL;

    return ESP_OK;
}

server::server(const uint16_t port, const std::string &base_path) : mp_implementation(std::make_unique<server_implementation>())
{
    mp_implementation->file_server.base_path = base_path;

    httpd_config_t httpd_config = HTTPD_DEFAULT_CONFIG();

    httpd_config.task_priority = SERVER_PRIORITY;
    httpd_config.core_id = SERVER_CORE_ID;
    httpd_config.server_port = port;
    httpd_config.max_open_sockets = std::min((2U * WORKER_COUNT) + 3U, 11U);
    httpd_config.uri_match_fn = httpd_uri_match_wildcard;

    ESP_ERROR_CHECK(httpd_start(&mp_implementation->file_server.httpd_handle, &httpd_config));

    start_workers(mp_implementation->file_server);

    const httpd_uri_t get = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = get_handler,
        .user_ctx = &mp_implementation.get()->file_server,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr,
    };

    ESP_ERROR_CHECK(httpd_register_uri_handler(mp_implementation->file_server.httpd_handle, &get));

    httpd_config_t httpd_ws_config = HTTPD_DEFAULT_CONFIG();

    httpd_ws_config.task_priority = SERVER_PRIORITY;
    httpd_ws_config.core_id = SERVER_CORE_ID;
    httpd_ws_config.server_port = httpd_config.server_port + 1;
    httpd_ws_config.ctrl_port = httpd_config.ctrl_port + 1;
    httpd_ws_config.max_open_sockets = 5;

    ESP_ERROR_CHECK(httpd_start(&mp_implementation->websocket_server.httpd_handle, &httpd_ws_config));

    const httpd_uri_t ws_get = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = ws_handler,
        .user_ctx = &mp_implementation.get()->websocket_server,
        .is_websocket = true,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr,
    };

    ESP_ERROR_CHECK(httpd_register_uri_handler(mp_implementation->websocket_server.httpd_handle, &ws_get));
}

server::~server()
{
    ESP_ERROR_CHECK(httpd_stop(mp_implementation->websocket_server.httpd_handle));

    stop_workers(mp_implementation->file_server);

    ESP_ERROR_CHECK(httpd_stop(mp_implementation->file_server.httpd_handle));
}
