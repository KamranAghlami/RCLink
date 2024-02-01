#include "websocket_server.h"

#include <vector>

#include <esp_err.h>
#include <esp_log.h>
#include <esp_http_server.h>

constexpr const char *TAG = "websocket_server";
constexpr const UBaseType_t SERVER_CORE_ID = 1U;
constexpr const UBaseType_t SERVER_PRIORITY = 5U;
constexpr const size_t WS_BUFFER_SIZE = 16U * 1024U;

struct websocket_server_implementation
{
    httpd_handle_t httpd_handle;
    std::vector<uint8_t> receive_buffer;
    std::vector<uint8_t> transmit_buffer;
    data_stream *stream;
};

static esp_err_t handler(httpd_req_t *request)
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

websocket_server::websocket_server(const uint16_t port) : mp_implementation(std::make_unique<websocket_server_implementation>())
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    config.task_priority = SERVER_PRIORITY;
    config.core_id = SERVER_CORE_ID;
    config.server_port = port;
    config.ctrl_port += port;
    config.max_open_sockets = 5U;

    ESP_ERROR_CHECK(httpd_start(&mp_implementation->httpd_handle, &config));

    mp_implementation->receive_buffer.reserve(WS_BUFFER_SIZE);
    mp_implementation->transmit_buffer.reserve(WS_BUFFER_SIZE);

    const httpd_uri_t ws_get = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = handler,
        .user_ctx = mp_implementation.get(),
        .is_websocket = true,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr,
    };

    ESP_ERROR_CHECK(httpd_register_uri_handler(mp_implementation->httpd_handle, &ws_get));
}

websocket_server::~websocket_server()
{
    ESP_ERROR_CHECK(httpd_stop(mp_implementation->httpd_handle));
}

void websocket_server::set_data_stream(data_stream &stream)
{
    mp_implementation->stream = &stream;
}