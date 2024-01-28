#pragma once

#include "string"
#include "memory"

struct server_implementation;

class server
{
public:
    server(const uint16_t port = 80, const std::string &base_path = "");
    ~server();

private:
    std::unique_ptr<server_implementation> mp_implementation;
};
