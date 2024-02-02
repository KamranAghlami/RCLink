#include "websocket_server.h"

#include <vector>
#include <cstring>

#include <esp_err.h>
#include <esp_log.h>
#include <esp_http_server.h>

#include "lock_guard.h"

using header_type = uint16_t;

constexpr const char *TAG = "websocket_server";
constexpr const UBaseType_t SERVER_CORE_ID = 1U;
constexpr const UBaseType_t SERVER_PRIORITY = 5U;
constexpr const size_t WS_BUFFER_SIZE = 16U * 1024U;
constexpr const size_t WS_SEND_CHUNK_SIZE = 1024U;
constexpr const size_t HEADER_SIZE = sizeof(header_type);

struct websocket_server_implementation
{
    httpd_handle_t handle;
    int socket_descriptor = -1;
    SemaphoreHandle_t receive_semaphore;
    SemaphoreHandle_t transmit_semaphore;
    std::vector<uint8_t> receive_buffer;
    std::vector<uint8_t> transmit_buffer;
    size_t receive_discard;
    bool transmitting;
};

static void shift_left(std::vector<uint8_t> &buffer, size_t amount)
{
    if (!amount)
        return;

    const size_t new_size = buffer.size() - amount;

    if (new_size)
    {
        std::memmove(buffer.data(), buffer.data() + amount, new_size);

        buffer.resize(new_size);
    }
    else
        buffer.resize(0);
}

static void send_async(void *arg)
{
    auto server_impl = static_cast<websocket_server_implementation *>(arg);

    lock_guard guard(server_impl->transmit_semaphore);

    if (server_impl->socket_descriptor == -1)
        return;

    bool final = server_impl->transmit_buffer.size() <= WS_SEND_CHUNK_SIZE;

    httpd_ws_frame_t ws_frame = {
        .final = final,
        .fragmented = true,
        .type = server_impl->transmitting ? HTTPD_WS_TYPE_CONTINUE : HTTPD_WS_TYPE_BINARY,
        .payload = server_impl->transmit_buffer.data(),
        .len = final ? server_impl->transmit_buffer.size() : WS_SEND_CHUNK_SIZE,
    };

    httpd_ws_send_frame_async(server_impl->handle, server_impl->socket_descriptor, &ws_frame);

    server_impl->transmitting = !final;

    if (server_impl->transmitting)
    {
        shift_left(server_impl->transmit_buffer, WS_SEND_CHUNK_SIZE);

        httpd_queue_work(server_impl->handle, send_async, server_impl);
    }
    else
        server_impl->transmit_buffer.resize(0);
}

static void switch_client(websocket_server_implementation &server_impl)
{
    int client_descriptors[5];
    size_t client_descriptors_size = 5;

    if (httpd_get_client_list(server_impl.handle, &client_descriptors_size, client_descriptors) != ESP_OK)
        return;

    {
        lock_guard rx_guard(server_impl.receive_semaphore);
        lock_guard tx_guard(server_impl.transmit_semaphore);

        server_impl.socket_descriptor = client_descriptors[client_descriptors_size - 1];
        server_impl.receive_buffer.resize(0);
        server_impl.transmit_buffer.resize(0);
        server_impl.receive_discard = 0;
        server_impl.transmitting = false;
    }

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

    {
        lock_guard guard(server_impl->receive_semaphore);

        if (server_impl->socket_descriptor == -1)
            return ESP_FAIL;

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
    }

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

    ESP_ERROR_CHECK(httpd_start(&mp_implementation->handle, &config));

    mp_implementation->receive_semaphore = xSemaphoreCreateMutex();
    mp_implementation->transmit_semaphore = xSemaphoreCreateMutex();
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

    vSemaphoreDelete(mp_implementation->transmit_semaphore);
    vSemaphoreDelete(mp_implementation->receive_semaphore);
}

websocket_server &websocket_server::operator>>(tlvcpp::tlv_tree_node &node)
{
    lock_guard guard(mp_implementation->receive_semaphore);

    if (mp_implementation->socket_descriptor == -1)
        return *this;

    auto &buffer = mp_implementation->receive_buffer;

    size_t dealt_with = 0;

    while (true)
    {
        const auto data = buffer.data() + dealt_with;
        const auto size = buffer.size() - dealt_with;

        if (size < HEADER_SIZE)
            break;

        const auto message_size = *reinterpret_cast<const header_type *>(data);

        if (message_size > WS_BUFFER_SIZE - HEADER_SIZE)
        {
            mp_implementation->receive_discard += (HEADER_SIZE + message_size - size);

            dealt_with = buffer.size();

            break;
        }

        if (!node.deserialize(data + HEADER_SIZE, message_size))
            ESP_LOGW(TAG, "deserialization error!");

        dealt_with += (HEADER_SIZE + message_size);
    }

    shift_left(buffer, dealt_with);

    return *this;
}

websocket_server &websocket_server::operator<<(const tlvcpp::tlv_tree_node &node)
{
    lock_guard guard(mp_implementation->transmit_semaphore);

    if (mp_implementation->socket_descriptor == -1)
        return *this;

    if (!node.serialize(mp_implementation->transmit_buffer))
    {
        ESP_LOGW(TAG, "serialization error!");

        return *this;
    }

    if (!mp_implementation->transmitting)
        httpd_queue_work(mp_implementation->handle, send_async, mp_implementation.get());

    return *this;
}