#pragma once

#include <stdint.h>

#include <charconv>
#include <iostream>
#include <string_view>

#include "log.h"

inline std::string_view remove_before(std::string_view &src, std::string_view delim)
{
    size_t eq = src.find(delim);
    std::string_view left = src.substr(0, eq);
    if (eq != std::string_view::npos)
    {
        src.remove_prefix(eq + delim.size());
    }
    else
    {
        src.remove_prefix(src.size());
    }
    return left;
}

template<typename NUMBER> 
bool aton(auto str, NUMBER &number, bool short_ok = false)
{
    NUMBER value = 0;
    const char* begin = str.data();
    const char* end = begin + str.size();
    auto const out = std::from_chars(begin, end, value, 10);
    if (out.ec != std::errc{} || (!short_ok && out.ptr != end))
    {
        ERROR << str << '\n'
              << std::string(std::distance(begin, out.ptr), ' ') << "^- here" << ENDL;
        auto const ec = std::make_error_code(out.ec);
        ERROR << "err: " << ec.message() << ENDL;
        return false;
    }
    number = value;
    return true;
}

inline uint32_t aton(auto str, bool short_ok = false)
{
    uint32_t value = 0;
    const char* begin = str.data();
    const char* end = begin + str.size();
    auto const out = std::from_chars(begin, end, value, 10);
    if (out.ec != std::errc{} || (!short_ok && out.ptr != end))
    {
        ERROR << str << '\n'
              << std::string(std::distance(begin, out.ptr), ' ') << "^- here" << ENDL;
        auto const ec = std::make_error_code(out.ec);
        ERROR << "err: " << ec.message() << ENDL;
        return 0;
    }
    return value;
}

