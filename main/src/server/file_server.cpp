#include "file_server.h"

#include <sys/stat.h>

#include <esp_err.h>
#include <esp_log.h>
#include <esp_http_server.h>

constexpr const char *TAG = "file_server";
constexpr const UBaseType_t SERVER_CORE_ID = 1U;
constexpr const UBaseType_t SERVER_PRIORITY = 5U;
constexpr const UBaseType_t WORKER_COUNT = 4U;
constexpr const uint32_t WORKER_STACK_SIZE = 4U * 1024U;

struct file_server_implementation
{
    SemaphoreHandle_t workers_semaphore;
    QueueHandle_t requests_queue;
    TaskHandle_t workers[WORKER_COUNT];
    bool is_running;
    httpd_handle_t handle;
    std::string base_path;
};

using request_handler = esp_err_t (*)(httpd_req_t *);

struct request_context
{
    httpd_req_t *request;
    request_handler handler;
};

static void request_worker_task(void *argument)
{
    const auto server_impl = static_cast<file_server_implementation *>(argument);

    while (true)
    {
        xSemaphoreGive(server_impl->workers_semaphore);

        request_context request;

        if (xQueueReceive(server_impl->requests_queue, &request, portMAX_DELAY))
        {
            request.handler(request.request);

            httpd_req_async_handler_complete(request.request);
        }
    }
}

static void start_workers(file_server_implementation &server_impl)
{
    server_impl.is_running = true;
    server_impl.workers_semaphore = xSemaphoreCreateCounting(WORKER_COUNT, 0);
    server_impl.requests_queue = xQueueCreate(WORKER_COUNT, sizeof(request_context));

    for (size_t i = 0; i < WORKER_COUNT; i++)
        xTaskCreatePinnedToCore(request_worker_task,
                                "request_worker",
                                WORKER_STACK_SIZE,
                                &server_impl,
                                SERVER_PRIORITY,
                                &server_impl.workers[i],
                                SERVER_CORE_ID);
}

static void stop_workers(file_server_implementation &server_impl)
{
    server_impl.is_running = false;

    request_context request;

    while (xQueueReceive(server_impl.requests_queue, &request, pdMS_TO_TICKS(100)))
        httpd_req_async_handler_complete(request.request);

    while (uxSemaphoreGetCount(server_impl.workers_semaphore) != WORKER_COUNT)
        vTaskDelay(pdMS_TO_TICKS(100));

    for (size_t i = 0; i < WORKER_COUNT; i++)
        vTaskDelete(server_impl.workers[i]);

    vQueueDelete(server_impl.requests_queue);
    vSemaphoreDelete(server_impl.workers_semaphore);
}

static bool is_on_worker(const file_server_implementation &server_impl)
{
    TaskHandle_t current = xTaskGetCurrentTaskHandle();

    for (size_t i = 0; i < WORKER_COUNT; i++)
        if (current == server_impl.workers[i])
            return true;

    return false;
}

static esp_err_t submit_work(const file_server_implementation &server_impl, httpd_req_t *request, request_handler handler)
{
    xSemaphoreTake(server_impl.workers_semaphore, portMAX_DELAY);

    request_context request_ctx = {
        .request = nullptr,
        .handler = handler,
    };

    if (auto error = httpd_req_async_handler_begin(request, &request_ctx.request) != ESP_OK)
        return error;

    xQueueSend(server_impl.requests_queue, &request_ctx, portMAX_DELAY);

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

static esp_err_t handler(httpd_req_t *request)
{
    const auto server_impl = static_cast<file_server_implementation *>(request->user_ctx);

    if (!is_on_worker(*server_impl))
    {
        if (server_impl->is_running)
            return submit_work(*server_impl, request, handler);
        else
            return ESP_FAIL;
    }

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

    return ESP_OK;
}

file_server::file_server(const uint16_t port, const std::string &base_path) : mp_implementation(std::make_unique<file_server_implementation>())
{
    start_workers(*mp_implementation);

    mp_implementation->base_path = base_path;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    config.task_priority = SERVER_PRIORITY;
    config.core_id = SERVER_CORE_ID;
    config.server_port = port;
    config.ctrl_port += port;
    config.max_open_sockets = std::min((2U * WORKER_COUNT) + 3U, 11U);
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;

    ESP_ERROR_CHECK(httpd_start(&mp_implementation->handle, &config));

    const httpd_uri_t get = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = handler,
        .user_ctx = mp_implementation.get(),
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr,
    };

    ESP_ERROR_CHECK(httpd_register_uri_handler(mp_implementation->handle, &get));
}

file_server::~file_server()
{
    stop_workers(*mp_implementation);

    ESP_ERROR_CHECK(httpd_stop(mp_implementation->handle));
}