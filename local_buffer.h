#pragma once

#include <array>
#include <string_view>

template<typename TYPE, size_t MAX_SIZE>
class local_buffer : public std::array<TYPE, MAX_SIZE>
{
public:
    local_buffer()
        :std::array<TYPE, MAX_SIZE>()
    {

    }

    size_t size() const { return m_size; }

    void set_size(size_t sz)
    {
        if (sz <= MAX_SIZE)
        {
            m_size = sz;
        }
    }

    void add_size(size_t sz)
    {
        if (sz + m_size <= MAX_SIZE)
        {
            m_size += sz;
        }
    }

    std::pair<char*, size_t> remaining()
    {
        return std::make_pair(this->data() + m_size, this->max_size() - m_size);
    }

    void clear()
    {
        // It is non-null terminated data, can rely on just setting m_size to 0
        m_size = 0;
    }

    // Overloaded conversion operator to std::string_view
    operator std::string_view() const
    {
        return std::string_view(this->data(), m_size);
    }

    std::string_view str() const
    {
        return std::string_view(this->data(), m_size);
    }
private:
    size_t m_size = 0;
};
