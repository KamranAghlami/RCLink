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
constexpr const size_t WS_RX_BUFFER_SIZE = 4U * 1024U;
constexpr const size_t WS_TX_BUFFER_SIZE = 16U * 1024U;
constexpr const size_t WS_TX_CHUNK_SIZE = 1024U;
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

static void shift_left(uint8_t *buffer, size_t size, size_t amount)
{
    if (!amount)
        return;

    const size_t new_size = size - amount;

    if (new_size)
        std::memmove(buffer, buffer + amount, new_size);
}

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

    auto &buffer = server_impl->transmit_buffer;
    bool final = buffer.size() <= WS_TX_CHUNK_SIZE;

    httpd_ws_frame_t ws_frame = {
        .final = final,
        .fragmented = true,
        .type = server_impl->transmitting ? HTTPD_WS_TYPE_CONTINUE : HTTPD_WS_TYPE_BINARY,
        .payload = buffer.data(),
        .len = final ? buffer.size() : WS_TX_CHUNK_SIZE,
    };

    if (httpd_ws_send_frame_async(server_impl->handle, server_impl->socket_descriptor, &ws_frame) != ESP_OK)
    {
        ESP_LOGW(TAG, "couldn't send frame! retrying...");

        httpd_queue_work(server_impl->handle, send_async, server_impl);

        return;
    }

    server_impl->transmitting = !final;

    if (server_impl->transmitting)
    {
        shift_left(buffer, WS_TX_CHUNK_SIZE);

        httpd_queue_work(server_impl->handle, send_async, server_impl);

        return;
    }

    if (buffer.capacity() > WS_TX_BUFFER_SIZE)
    {
        {
            std::remove_reference<decltype(buffer)>::type new_buffer;

            buffer.swap(new_buffer);
        }

        buffer.reserve(WS_TX_BUFFER_SIZE);

        return;
    }

    buffer.resize(0);
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
        {
            ESP_LOGW(TAG, "couldn't receive frame size!");

            return ESP_FAIL;
        }

        if (ws_frame.len)
        {
            auto &buffer = server_impl->receive_buffer;
            const auto size = buffer.size();

            if ((buffer.capacity() - size) < ws_frame.len)
            {
                ESP_LOGW(TAG, "receive buffer full!");

                return ESP_FAIL;
            }

            buffer.resize(size + ws_frame.len);
            ws_frame.payload = buffer.data() + size;

            if (httpd_ws_recv_frame(request, &ws_frame, ws_frame.len) != ESP_OK)
            {
                ESP_LOGW(TAG, "couldn't receive frame!");

                return ESP_FAIL;
            }

            if (server_impl->receive_discard)
            {
                const auto discardable = std::min(server_impl->receive_discard, ws_frame.len);

                if (discardable < ws_frame.len)
                    shift_left(ws_frame.payload, ws_frame.len, discardable);

                buffer.resize(buffer.size() - discardable);
                server_impl->receive_discard -= discardable;
            }
        }
    }

    return ESP_OK;
}

void on_connected(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    ESP_LOGI(TAG, "connected.");
}

void on_disconnected(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    ESP_LOGI(TAG, "disconnected.");
}

websocket_server::websocket_server(const uint16_t port) : mp_implementation(std::make_unique<websocket_server_implementation>())
{
    ESP_ERROR_CHECK(esp_event_handler_register(ESP_HTTP_SERVER_EVENT, HTTP_SERVER_EVENT_ON_CONNECTED, on_connected, mp_implementation.get()));
    ESP_ERROR_CHECK(esp_event_handler_register(ESP_HTTP_SERVER_EVENT, HTTP_SERVER_EVENT_DISCONNECTED, on_disconnected, mp_implementation.get()));

    mp_implementation->receive_semaphore = xSemaphoreCreateMutex();
    mp_implementation->transmit_semaphore = xSemaphoreCreateMutex();
    mp_implementation->receive_buffer.reserve(WS_RX_BUFFER_SIZE);
    mp_implementation->transmit_buffer.reserve(WS_TX_BUFFER_SIZE);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    config.task_priority = SERVER_PRIORITY;
    config.core_id = SERVER_CORE_ID;
    config.server_port = port;
    config.ctrl_port += port;
    config.max_open_sockets = 5U;
    config.lru_purge_enable = true;

    ESP_ERROR_CHECK(httpd_start(&mp_implementation->handle, &config));

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

    ESP_ERROR_CHECK(esp_event_handler_unregister(ESP_HTTP_SERVER_EVENT, HTTP_SERVER_EVENT_DISCONNECTED, on_disconnected));
    ESP_ERROR_CHECK(esp_event_handler_unregister(ESP_HTTP_SERVER_EVENT, HTTP_SERVER_EVENT_ON_CONNECTED, on_connected));
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
        const auto total_size = HEADER_SIZE + message_size;

        if (message_size > WS_RX_BUFFER_SIZE - HEADER_SIZE)
        {
            mp_implementation->receive_discard += (total_size - size);

            dealt_with = buffer.size();

            break;
        }

        if (size < total_size)
            break;

        tlvcpp::tlv_tree_node received_node;

        if (received_node.deserialize(data + HEADER_SIZE, message_size))
        {
            if (received_node.data().tag())
                node.add_child() = std::move(received_node);
            else
                for (const auto &child : received_node.children())
                    node.add_child() = std::move(child);
        }
        else
            ESP_LOGW(TAG, "deserialization error!");

        dealt_with += total_size;
    }

    shift_left(buffer, dealt_with);

    return *this;
}

websocket_server &websocket_server::operator<<(const tlvcpp::tlv_tree_node &node)
{
    lock_guard guard(mp_implementation->transmit_semaphore);

    if (mp_implementation->socket_descriptor == -1)
        return *this;

    auto &buffer = mp_implementation->transmit_buffer;
    const auto size = buffer.size();

    for (size_t i = 0; i < sizeof(header_type); i++)
        buffer.push_back(0);

    size_t bytes_written = 0;

    if (!node.serialize(buffer, &bytes_written))
    {
        ESP_LOGW(TAG, "serialization error!");

        buffer.resize(size);

        return *this;
    }

    if (!bytes_written || bytes_written > std::numeric_limits<header_type>::max())
    {
        buffer.resize(size);

        return *this;
    }

    auto message_size = reinterpret_cast<header_type *>(&*buffer.end() - (HEADER_SIZE + bytes_written));

    *message_size = bytes_written;

    if (!mp_implementation->transmitting)
        httpd_queue_work(mp_implementation->handle, send_async, mp_implementation.get());

    return *this;
}