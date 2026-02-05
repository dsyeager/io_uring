#include "commas.h"
#include "get_nanoseconds.h"
#include "hash.h"
#include "io_uring_wrapper.h"
#include "log.h"
#include "misc.h"
#include "scoped_lock.h"
#include "string_view.h"
#include "time_tracker.h"

#include <errno.h>
#include <fcntl.h>
#include <liburing.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using std::string, std::string_view, std::vector;
using namespace std::literals;

#define BUFFER_SZ 64 * 1024


time_tracker s_times(10000);

/**
 A fixed length meta data written before each file
 Between the fixed length data and the file data we write variable 
 length vals like file name and description
  */
struct file_meta_data
{
public:
    uint64_t file_size = 0;
    uint64_t file_hash = 0;
    time_t   write_time = 0;
    uint16_t file_name_len = 0;
    uint16_t file_desc_len = 0;
    // some room built into the fixed length buffer for future uses like tags
    uint16_t future_1_len = 0;
    uint16_t future_2_len = 0;
    uint16_t future_3_len = 0;
    uint16_t future_4_len = 0;
};

class client_request
{
private:
    enum STATE {READING_CLIENT_INPUT, WRITING_TO_FILE, WRITING_META, COMPLETED, FAILED};
    STATE m_state = READING_CLIENT_INPUT;
    char m_buffer[BUFFER_SZ];
    off_t m_offset = 0;
    off_t m_output_offset = 0;
    uint64_t m_bytes_written = 0;
    int  m_input_fd = -1;
    int  m_output_fd = -1;
    uint32_t m_index = 0;
    io_uring_wrapper<client_request> *m_file_uring = nullptr;
    uint64_t m_start_ns = 0;
    uint64_t m_end_ns = 0;

    file_meta_data m_meta;
    std::string m_file_name;
    std::string m_file_desc;
    uint64_t m_meta_bytes_to_write = 0;

    uint64_t meta_size() const { return sizeof(file_meta_data) + m_file_name.size() + m_file_desc.size(); } 
    uint64_t file_start() const { return m_output_offset + meta_size(); }

public:
    client_request(std::string_view file_name,
                   std::string_view file_desc,
                   int input_fd,
                   uint32_t index,
                   io_uring_wrapper<client_request> *file_uring,
                   int output_fd = -1,
                   off_t output_offset = 0)
        : m_input_fd(dup(input_fd)),
          m_index(index),
          m_file_uring(file_uring),
          m_output_fd(output_fd),
          m_output_offset(output_offset),
          m_file_name(file_name),
          m_file_desc(file_desc)
    {
        m_start_ns = get_nanoseconds();
    }

    bool start_io_uring()
    {
        m_state = READING_CLIENT_INPUT;
        return m_file_uring && m_file_uring->prep_read(m_input_fd, m_buffer, BUFFER_SZ, m_offset, this);
    }

    uint32_t process_io_uring(int res)
    {
        if (!m_file_uring)
            return 0;

        if (res > 0)
        {
            switch (m_state) {
            case READING_CLIENT_INPUT:
                // Read successful. Write to stdout.
                DEBUG(2) << "writing " << res << " bytes to m_output_fd: " << m_output_fd << ENDL;

                m_meta.file_hash = compute_hash(std::string_view(m_buffer, res), m_meta.file_hash);

                // could be writing to a spool or similar so have to pass the exact offset to write at instead of just -1 for the end
                m_file_uring->prep_write(m_output_fd,
                                         m_buffer,
                                         res,
                                         file_start() + m_offset,
                                         this);
                m_state = WRITING_TO_FILE;
                m_offset += res;
                break;
            case WRITING_TO_FILE:
                m_bytes_written += res;
                DEBUG(2) << "reading up to " << BUFFER_SZ << " bytes from m_input_fd: " << m_input_fd << ENDL;
                m_file_uring->prep_read(m_input_fd, m_buffer, BUFFER_SZ, m_offset, this);
                m_state = READING_CLIENT_INPUT;
                break;
            case WRITING_META:
                m_meta_bytes_to_write -= res;
                if (0 == m_meta_bytes_to_write)
                {
                    m_state = COMPLETED;
                    m_end_ns = get_nanoseconds();
                    s_times.add_delta(m_end_ns - m_start_ns);
                }
                return 0; // no new events so ret 0
            };
            return 1;
        }
        else if (res == 0)
        {
            switch (m_state) {
            case READING_CLIENT_INPUT:
            {
                // reached EOF
                DEBUG(2) << "EOF for m_input_fd: " << m_input_fd << ", bytes written: " << m_bytes_written << ", starting meta data, hash: " << m_meta.file_hash << ENDL;
                // no more data to read, start writing the meta data
                // should we combine meta parts into single buffer and write once or issue N prep_writes??
                m_meta.file_size = m_bytes_written;
                m_meta.file_name_len = m_file_name.size();
                m_meta.file_desc_len = m_file_desc.size();

                size_t off_set = m_output_offset;

                
                // write file meta struct
                m_file_uring->prep_write(m_output_fd, (char*)&m_meta, sizeof(m_meta), off_set, this);
                off_set += sizeof(m_meta);
                m_meta_bytes_to_write += sizeof(m_meta);

                // write file name
                m_file_uring->prep_write(m_output_fd, m_file_name.data(), m_file_name.size(), off_set, this);
                off_set += m_file_name.size();
                m_meta_bytes_to_write += m_file_name.size();

                // write file desc
                m_file_uring->prep_write(m_output_fd, m_file_desc.data(), m_file_desc.size(), off_set, this);
                m_meta_bytes_to_write += m_file_desc.size();

                m_state = WRITING_META;

                return 1;
            }
            case WRITING_TO_FILE:
                ERROR << "Failed writing to file: res == 0" << ENDL;
                m_state = FAILED;
                break;

            case WRITING_META:
                ERROR << "Failed writing meta data: res == 0" << ENDL;
                m_state = FAILED;
                break;
            };

            return 0;
        }
        else if (res < 0)
        {
            // Error reading file
            ERROR << ::strerror(abs(res)) << ENDL;
            m_state = FAILED;
            return 0;
        }
        return 0;
    }

    char* buffer() { return m_buffer; }
};

void uring_thread(uint32_t cnt,
                  const char* file_name,
                  const char* file_desc,
                  uint64_t file_size,
                  int input_fd,
                  int spool_fd,
                  uint64_t block_offset)
{
    uint64_t start = get_nanoseconds();

    io_uring_wrapper<client_request> file_uring(cnt * 10);
    if (!file_uring.is_valid())
    {
        return;
    }

    if (0 > spool_fd)
    {
        ERROR << "spool_fd is invalid: " << spool_fd << ENDL;
        return;
    }

    off_t output_offset = block_offset;

    vector<client_request*> requests;

    for (uint32_t i = 0; i < cnt; i++)
    {
        requests.push_back(new client_request(file_name, file_desc, input_fd, i, &file_uring, spool_fd, output_offset));
        requests.back()->start_io_uring();
        if (spool_fd != -1)
        {
            output_offset += file_size;
        }
    }

    file_uring.submit();

    __kernel_timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 500;   // .5 microseconds


    while (file_uring.pending())
    {
        // last two nullptr's are timespec and sigmask
        file_uring.process_events(std::max(cnt / 10, 10u), &ts);
    }
}

int32_t main (int argc, char **argv)
{
    // process cmd line args
    std::string input;
    std::string output;
    std::string file_name;
    std::string file_desc("Some file uploaded from some person. Has binary content that could be viewed on a media player and or file editor"sv);
    uint32_t cnt = 1;
    uint32_t event_cnt = 1000;
    uint32_t thread_cnt = 1;
    bool spool_it = false;
    int spool_fd = -1;
    uint64_t file_size = 0;

    for (int i = 1; i < argc; i++)
    {
        auto[key, val] = split(argv[i], '=');
        if (key == "--input"sv)
        {
            input = val;
        }
        else if (key == "--output"sv)
        {
            output = val;
        }
        else if (key == "--file-name"sv)
        {
            file_name = val;
        }
        else if (key == "--file-desc"sv)
        {
            file_desc = val;
        }
        else if (key == "--cnt"sv)
        {
            cnt = aton(val);
        } 
        else if (key == "--thread-cnt"sv)
        {
            thread_cnt = aton(val);
        } 
        else if (key == "--event-cnt"sv)
        {
            event_cnt = aton(val);
        } 
        else if (key == "--debug"sv)
        {
            s_debug_level = aton(val);
        } 
        else if (key == "--spool"sv)
        {
            spool_it = true;
        }
    }

    if (file_name.empty())
    {
        file_name = input;
    }

    int input_fd = ::open(input.data(), O_RDONLY);
    if (input_fd < 0)
    {
        ERROR << "Failed to open " << input << ", " << ::strerror(errno) << ENDL;
        return 0;
    }

    struct stat sb;
    if (::fstat(input_fd, &sb) == -1)
    {
        ERROR << "failed to stat input file: " << ::strerror(errno) << ENDL;
        return 0;
    }
    file_size = sb.st_size;

    if (spool_it)
    {
        spool_fd = ::open(output.data(), O_WRONLY | O_CREAT, 0666);
        if (-1 == spool_fd)
        {
            ERROR << "Failed to open spool file: " << output << ", " << ::strerror(errno) << ENDL;
            return 0;
        }
    }

    TRACE << "starting " << cnt << " copies of file: " << file_name << ", bytes: " << file_size << ", threads: " << thread_cnt << ENDL;

    uint64_t start = get_nanoseconds();

    off_t output_offset = 0;

    std::vector<std::thread*> threads;
    uint32_t cnt_per_thread = cnt / thread_cnt;
    uint32_t block_size = cnt_per_thread * (file_size + file_name.length() + file_desc.length() + sizeof(file_meta_data));

    for (uint32_t t = 0; t < thread_cnt; t++)
    {
        threads.push_back(new std::thread(uring_thread,
                                          cnt_per_thread,
                                          file_name.data(), 
                                          file_desc.data(),
                                          file_size, 
                                          input_fd,
                                          spool_fd,
                                          output_offset));

        output_offset += block_size;
    }

    for (auto thrd : threads)
    {
        if (thrd->joinable())
            thrd->join();
    }

    s_times.trace_total_ns(file_size, "ns"sv);
}
