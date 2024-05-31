#pragma once

#include <string>
#include <memory>

struct http_server_implementation;

class http_server
{
public:
    http_server(const uint16_t port = 80, const std::string &base_path = "");
    ~http_server();

private:
    std::unique_ptr<http_server_implementation> mp_implementation;
};