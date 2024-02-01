#pragma once

#include <memory>

#include "data_stream.h"

struct websocket_server_implementation;

class websocket_server
{
public:
    websocket_server(const uint16_t port = 81);
    ~websocket_server();

    void set_data_stream(data_stream &stream);

private:
    std::unique_ptr<websocket_server_implementation> mp_implementation;
};