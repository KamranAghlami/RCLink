#pragma once

#include <string>
#include <memory>

#include "data_stream.h"

struct server_implementation;

class server
{
public:
    server(const uint16_t port = 80, const std::string &base_path = "");
    ~server();

    void set_data_stream(data_stream &stream);

private:
    std::unique_ptr<server_implementation> mp_implementation;
};
