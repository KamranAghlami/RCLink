#pragma once

#include <memory>

#include "data_stream.h"

struct websocket_server_implementation;

class websocket_server : public data_stream
{
public:
    websocket_server(const uint16_t port = 81);
    ~websocket_server();

    websocket_server &operator>>(tlvcpp::tlv_tree_node &node) override;
    websocket_server &operator<<(const tlvcpp::tlv_tree_node &node) override;

private:
    std::unique_ptr<websocket_server_implementation> mp_implementation;
};