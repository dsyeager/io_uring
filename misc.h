#pragma once

#include <stdint.h>

#include <charconv>
#include <iostream>
#include <string_view>

std::pair<std::string_view, std::string_view> split(const std::string_view &src, char delim)
{
    size_t eq = src.find('=');
    std::string_view left = src.substr(0, eq);
    std::string_view right;
    if (eq != std::string_view::npos)
    {
        right = src.substr(eq + 1);
    }
    return std::pair(left, right);
}

bool aton(auto str, uint32_t &number, bool short_ok = false)
{
    uint32_t value = 0;
    const char* begin = str.data();
    const char* end = begin + str.size();
    auto const out = std::from_chars(begin, end, value, 10);
    if (out.ec != std::errc{} || (!short_ok && out.ptr != end))
    {
        std::cerr << str << '\n'
                  << std::string(std::distance(begin, out.ptr), ' ') << "^- here" << std::endl;
        auto const ec = std::make_error_code(out.ec);
        std::cerr << "err: " << ec.message() << std::endl;
        return false;
    }
    number = out;
    return true;
}

uint32_t aton(auto str, bool short_ok = false)
{
    uint32_t value = 0;
    const char* begin = str.data();
    const char* end = begin + str.size();
    auto const out = std::from_chars(begin, end, value, 10);
    if (out.ec != std::errc{} || (!short_ok && out.ptr != end))
    {
        std::cerr << str << '\n'
                  << std::string(std::distance(begin, out.ptr), ' ') << "^- here" << std::endl;
        auto const ec = std::make_error_code(out.ec);
        std::cerr << "err: " << ec.message() << std::endl;
        return 0;
    }
    return value;
}

