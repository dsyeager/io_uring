#pragma once


#include "get_nanoseconds.h"
#include "log.h"
#include "scoped_lock.h"

#include <mutex>
#include <string_view>
#include <vector>

class time_tracker
{
public:
    time_tracker(size_t reserve_size = 10000)
    {
        m_start = get_nanoseconds();
        m_times.reserve(reserve_size);
    }

    ~time_tracker()
    {
    }

    void add_delta(uint64_t delta)
    {
        dsy::scoped_lock alock(&m_mutex, true);
        m_times.push_back(delta);
    }

    void trace_total_ns_percentile(uint32_t pct, uint64_t bytes, std::string_view unit)
    {
        size_t pN = m_times.size() * pct / 100;
        uint64_t val = m_times[pN];
        // sz      NN
        // ---  = ---
        // val    1000000000
        double secs = 1000000000 / val;
        uint64_t bytes_per_sec = bytes * 1000000000 / val;
        uint64_t mb_per_sec = bytes_per_sec / 1024 / 1024;
        TRACE << "p" << pct
              << ", " << unit << ": " << commas(val)
              << ", MB/s: " << commas(mb_per_sec)
              << ENDL;
    }

    void trace_total_ns(uint64_t bytes, std::string_view unit)
    {
        m_end = get_nanoseconds();
        uint64_t ns = m_end - m_start;
        uint64_t bytes_copied = bytes * m_times.size();
        uint64_t bytes_per_sec = bytes_copied * 1000000000 / ns;
        uint64_t mb_per_sec = bytes_per_sec / 1024 / 1024;

        std::sort(m_times.begin(), m_times.end());
        TRACE << "Total Requests: " << m_times.size()
              << ", total bytes: " << commas(bytes_copied)
              << ", bytes each: " << bytes
              << ", MB/s: " << commas(mb_per_sec) << ENDL;

        trace_total_ns_percentile(5, bytes, unit);
        trace_total_ns_percentile(50, bytes, unit);
        trace_total_ns_percentile(95, bytes, unit);
    }


private:

    std::vector<uint64_t> m_times;
    std::mutex m_mutex;
    uint64_t m_start = 0;
    uint64_t m_end = 0;
};
