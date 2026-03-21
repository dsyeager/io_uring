#pragma once

#include "commas.h"
#include "log.h"
#include "from_file.h"
#include "get_nanoseconds.h"
#include "string_view.h"

#include <string>

class proc_pid_stat
{
public:
    proc_pid_stat()
    {
        read_stats("start");
    }

    ~proc_pid_stat()
    {
        for (auto sp : m_stats)
        {
            delete sp;
        }
        m_stats.clear();
    }

    void read_stats(const char *desc)
    {
        pid_stats *stats = new pid_stats;
        stats->desc = desc;

        std::string buff;
        from_file(buff, "/proc/self/stat");

        dsy::string_view str(buff);
        
        for (uint32_t i = 1; i < 41; i++)
        {
            dsy::string_view fld = str.split(' ');

            switch (i) {
            case 1: fld.aton(stats->pid); break;
            case 14: fld.aton(stats->utime); break;
            case 15: fld.aton(stats->stime); break;
            case 20: fld.aton(stats->threads); break;
            case 23: fld.aton(stats->vsize); break;
            case 39: fld.aton(stats->processor); break;
            };
        }

        stats->now_ns = get_nanoseconds();

        m_stats.push_back(stats);
    }

    void trace(size_t first = 0, size_t last = -1)
    {
        pid_stats *psf = m_stats[first];
        pid_stats *psl = (last == -1 ? m_stats.back() : m_stats[last]);

        TRACE << "pid: " << psf->pid
              << ", clock ns: " << commas(psl->now_ns - psf->now_ns)
              << ", utime ns: " << commas((psl->utime - psf->utime)*1000000000/sysconf(_SC_CLK_TCK))
              << ", stime ns: " << commas((psl->stime - psf->stime)*1000000000/sysconf(_SC_CLK_TCK))
              //<< ", threads: " << psl->threads
              //<< ", vsize: " << commas(psl->vsize)
              << ", desc: " << psl->desc
              << ENDL;
    }

private:
    struct pid_stats
    {
                      // 1 based index
        uint64_t now_ns = 0;
        uint64_t pid = 0;       // #1 
        uint64_t utime = 0;     // #14     divide by sysconf(_SC_CLK_TCK)
        uint64_t stime = 0;     // #15     divide by sysconf(_SC_CLK_TCK)
        uint64_t threads = 0;   // #20
        uint64_t vsize = 0;     // #23
        uint64_t processor = 0; // #39
        const char *desc = nullptr;
    };

    std::vector<pid_stats*> m_stats;
};
