#include "websocket_server.h"

#include <vector>

#include <esp_err.h>
#include <esp_log.h>
#include <esp_http_server.h>

#include <tlvcpp/utilities/hexdump.h>

constexpr const char *TAG = "websocket_server";
constexpr const UBaseType_t SERVER_CORE_ID = 1U;
constexpr const UBaseType_t SERVER_PRIORITY = 5U;
constexpr const size_t WS_BUFFER_SIZE = 16U * 1024U;
constexpr const size_t CLIENTS_MAX = 2;

struct websocket_server_implementation
{
    httpd_handle_t handle;
    int socket_descriptor = -1;
    std::vector<uint8_t> receive_buffer;
    std::vector<uint8_t> transmit_buffer;
    data_stream *stream;
};

static void send_async(void *arg)
{
    auto server_impl = static_cast<websocket_server_implementation *>(arg);

    httpd_ws_frame_t ws_frame = {};

    ws_frame.type = HTTPD_WS_TYPE_BINARY;
    ws_frame.payload = server_impl->transmit_buffer.data();
    ws_frame.len = server_impl->transmit_buffer.size();

    httpd_ws_send_frame_async(server_impl->handle, server_impl->socket_descriptor, &ws_frame);

    ESP_LOGI(TAG, "sent:");
    tlvcpp::hexdump(ws_frame.payload, ws_frame.len);

    server_impl->transmit_buffer.resize(0);
}

static void switch_client(websocket_server_implementation &server_impl)
{
    int client_descriptors[CLIENTS_MAX];
    size_t client_descriptors_size = CLIENTS_MAX;

    if (httpd_get_client_list(server_impl.handle, &client_descriptors_size, client_descriptors) != ESP_OK)
        return;

    server_impl.socket_descriptor = client_descriptors[client_descriptors_size - 1];
    server_impl.receive_buffer.resize(0);
    server_impl.transmit_buffer.resize(0);

    for (size_t i = 0; i < client_descriptors_size - 1; i++)
        httpd_sess_trigger_close(server_impl.handle, client_descriptors[i]);
}

static esp_err_t handler(httpd_req_t *request)
{
    auto server_impl = static_cast<websocket_server_implementation *>(request->user_ctx);

    if (request->method == HTTP_GET)
    {
        switch_client(*server_impl);

        return ESP_OK;
    }

    httpd_ws_frame_t ws_frame = {};

    if (httpd_ws_recv_frame(request, &ws_frame, 0) != ESP_OK)
        return ESP_FAIL;

    if (ws_frame.len)
    {
        auto rx_space = server_impl->receive_buffer.capacity() - server_impl->receive_buffer.size();

        if (rx_space < ws_frame.len)
            return ESP_FAIL;

        ws_frame.payload = server_impl->receive_buffer.data();
        server_impl->receive_buffer.resize(server_impl->receive_buffer.size() + ws_frame.len);

        if (httpd_ws_recv_frame(request, &ws_frame, ws_frame.len) != ESP_OK)
            return ESP_FAIL;
    }

    ESP_LOGI(TAG, "received:");
    tlvcpp::hexdump(ws_frame.payload, ws_frame.len);

    for (const auto byte : server_impl->receive_buffer)
        switch (byte)
        {
        case '\x0d':
            server_impl->transmit_buffer.push_back('\x0a');
            server_impl->transmit_buffer.push_back('\x0d');
            break;

        case '\x7f':
            server_impl->transmit_buffer.push_back('\x08');
            server_impl->transmit_buffer.push_back(' ');
            server_impl->transmit_buffer.push_back('\x08');
            break;

        default:
            server_impl->transmit_buffer.push_back(byte);
            break;
        }

    server_impl->receive_buffer.resize(0);

    return httpd_queue_work(server_impl->handle, send_async, server_impl);
}

websocket_server::websocket_server(const uint16_t port) : mp_implementation(std::make_unique<websocket_server_implementation>())
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    config.task_priority = SERVER_PRIORITY;
    config.core_id = SERVER_CORE_ID;
    config.server_port = port;
    config.ctrl_port += port;
    config.max_open_sockets = 5U;

    ESP_ERROR_CHECK(httpd_start(&mp_implementation->handle, &config));

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

    ESP_ERROR_CHECK(httpd_register_uri_handler(mp_implementation->handle, &ws_get));
}

websocket_server::~websocket_server()
{
    ESP_ERROR_CHECK(httpd_stop(mp_implementation->handle));
}

void websocket_server::set_data_stream(data_stream &stream)
{
    mp_implementation->stream = &stream;
}