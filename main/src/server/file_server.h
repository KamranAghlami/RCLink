#pragma once

#include <string>
#include <memory>

struct file_server_implementation;

class file_server
{
public:
    file_server(const uint16_t port = 80, const std::string &base_path = "");
    ~file_server();

private:
    std::unique_ptr<file_server_implementation> mp_implementation;
};