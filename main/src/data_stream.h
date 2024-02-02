#pragma once

#include <vector>
#include <cstdint>

#include <tlvcpp/tlv_tree.h>

class data_stream
{
public:
    virtual ~data_stream() {}

    virtual data_stream &operator>>(tlvcpp::tlv_tree_node &node) = 0;
    virtual data_stream &operator<<(const tlvcpp::tlv_tree_node &node) = 0;
};
