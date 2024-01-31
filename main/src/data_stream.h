#pragma once

#include <vector>
#include <cstdint>

class data_stream
{
public:
    virtual ~data_stream() {}

    virtual data_stream &operator<<(std::vector<uint8_t> &data) = 0;
    virtual data_stream &operator>>(std::vector<uint8_t> &data) = 0;
};
