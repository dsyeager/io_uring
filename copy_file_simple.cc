#include "log.h"
#include "misc.h"

#include <errno.h>
#include <fcntl.h>
#include <liburing.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

using std::cout, std::cerr, std::endl, std::string, std::string_view, std::vector;
using namespace std::literals;

#define BUFFER_SZ 1024

static uint64_t s_completed_requests = 0;

template <class EVENT_CLASS>
class io_uring_wrapper
{
private:
    io_uring m_ring;
    uint32_t m_queue_depth;
    uint32_t m_pending = 0;
    bool m_valid = true;
public:
    io_uring_wrapper(uint32_t queue_depth)
        : m_queue_depth(queue_depth)
    {
        // 1. Initialize the io_uring instance with a queue depth of 1
        int ret = io_uring_queue_init(queue_depth, &m_ring, 0);
        if (ret < 0)
        {
            ERROR << "io_uring_queue_init: " << ::strerror(-ret) << std::endl;
            m_valid = false;
            return;
        }
    }

    ~io_uring_wrapper()
    {
        if (m_valid)
        {
            io_uring_queue_exit(&m_ring);
        }
    }

    void submit()
    {
        if (!m_valid)
            return;
        io_uring_submit(&m_ring);
    }

    bool prep_write(int fd, char *buffer, size_t len, off_t offset, void *data)
    {
        if (!m_valid)
            return false;

        io_uring_sqe *sqe = io_uring_get_sqe(&m_ring);
        if (!sqe)
        {
            ERROR << "io_uring_get_sqe: failed to get SQE" << std::endl;
            io_uring_queue_exit(&m_ring);
            m_valid = false;
            return false;
        }

        io_uring_sqe_set_data(sqe, data);

        io_uring_prep_write(sqe, fd, buffer, len, offset);

        m_pending++;

        return true;
    }

    bool prep_read(int fd, char *buffer, size_t sz, off_t offset, void *data)
    {
        if (!m_valid)
            return false;
        io_uring_sqe *sqe = io_uring_get_sqe(&m_ring);
        if (!sqe)
        {
            ERROR << "io_uring_get_sqe: failed to get SQE" << std::endl;
            io_uring_queue_exit(&m_ring);
            m_valid = false;
            return false;
        }

        io_uring_sqe_set_data(sqe, data);

        io_uring_prep_read(sqe, fd, buffer, sz, offset);

        m_pending++;

        return true;
    }

    uint32_t process_events(uint32_t max_events = UINT32_MAX, __kernel_timespec *ts = nullptr)
    {
        if (!m_valid)
            return 0;

        io_uring_cqe *cqe = nullptr;
        unsigned i = 0;

        // last two nullptr's are timespec and sigmask
        if (0 == io_uring_wait_cqes(&m_ring, &cqe, std::min(max_events, m_pending), ts, nullptr))
        {
            unsigned head;
            uint32_t new_events = 0;

            io_uring_for_each_cqe(&m_ring, head, cqe)
            {
                 m_pending--; // decrement prior to ::process potentially incrementing
                 EVENT_CLASS *req = reinterpret_cast<EVENT_CLASS*>(io_uring_cqe_get_data(cqe));
                 new_events += req->process(cqe->res);
                 i++;
            }

            DEBUG(1) << "batch events: " << i << ENDL;

            if (new_events)
                io_uring_submit(&m_ring);

            io_uring_cq_advance(&m_ring, i);
        }
        return i;
    }

    bool is_valid() const { return m_valid; }

    uint32_t pending() const { return m_pending; }

};

class client_request
{
private:
    enum STATE {READING_CLIENT_INPUT, WRITING_TO_FILE, COMPLETED, FAILED};
    STATE m_state = READING_CLIENT_INPUT;
    char m_buffer[BUFFER_SZ];
    off_t m_offset = 0;
    uint64_t m_bytes_written = 0;
    int  m_input_fd = -1;
    int  m_output_fd = -1;
    uint32_t m_index = 0;
    io_uring_wrapper<client_request> *m_file_uring = nullptr;

public:
    client_request(int input_fd, std::string_view output_path, uint32_t index, io_uring_wrapper<client_request> *file_uring)
        : m_input_fd(dup(input_fd)), m_index(index), m_file_uring(file_uring)
    {
        // build the full output filepath with index on the end
        // open the output fd
        std::string opath(output_path);
        opath += ".";
        opath += std::to_string(m_index);
        m_output_fd = ::open(opath.data(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (m_output_fd < 0)
        {
            ERROR << "Failed to open output file, " << opath << ", " << ::strerror(errno) << endl;
            s_completed_requests++;
        }
    }

    bool start_io_uring()
    {
        m_state = READING_CLIENT_INPUT;
        return m_file_uring && m_file_uring->prep_read(m_input_fd, m_buffer, BUFFER_SZ, m_offset, this);
    }

    uint32_t process(int res)
    {
        if (!m_file_uring)
            return 0;

        if (res > 0)
        {
            switch (m_state) {
            case READING_CLIENT_INPUT:
                // Read successful. Write to stdout.
                DEBUG(1) << "writing " << res << " bytes to m_output_fd: " << m_output_fd << ENDL;
                m_file_uring->prep_write(m_output_fd, m_buffer, res, -1, this);
                m_state = WRITING_TO_FILE;
                m_offset += res;
                break;
            case WRITING_TO_FILE:
                m_bytes_written += res;
                DEBUG(1) << "reading up to " << BUFFER_SZ << " bytes from m_input_fd: " << m_input_fd << ENDL;
                m_file_uring->prep_read(m_input_fd, m_buffer, BUFFER_SZ, m_offset, this);
                m_state = READING_CLIENT_INPUT;
                break;
            };
            return 1;
        }
        else if (res == 0)
        {
            // reached EOF
            DEBUG(1) << "EOF for m_input_fd: " << m_input_fd << ", bytes written: " << m_bytes_written << ENDL;
            m_state = COMPLETED;
            s_completed_requests++;
            return 0;
        }
        else if (res < 0)
        {
            // Error reading file
            ERROR << ::strerror(abs(res)) << endl;
            m_state = FAILED;
            s_completed_requests++;
            return 0;
        }
        return 0;
    }

    char* buffer() { return m_buffer; }
};

int32_t main (int argc, char **argv)
{
    // process cmd line args
    std::string input;
    std::string output;
    uint32_t cnt = 1;
    uint32_t event_cnt = 1000;

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
        else if (key == "--cnt"sv)
        {
            cnt = aton(val);
        } 
        else if (key == "--event-cnt"sv)
        {
            event_cnt = aton(val);
        } 
        else if (key == "--debug"sv)
        {
            s_debug_level = aton(val);
        } 
    }

    int input_fd = ::open(input.data(), O_RDONLY);
    if (input_fd < 0)
    {
        ERROR << "Failed to open " << input << ", " << ::strerror(errno) << endl;
        return 0;
    }

    io_uring_wrapper<client_request> file_uring(cnt);
    if (!file_uring.is_valid())
    {
        return 1;
    }

    off_t offset = 0;

    vector<client_request*> requests;

    for (uint32_t i = 0; i < cnt; i++)
    {
        requests.push_back(new client_request(input_fd, output, i, &file_uring));
        requests.back()->start_io_uring();
    }
    file_uring.submit();

    uint32_t batch_cnt = 0;

    while (file_uring.pending())
    {
        // last two nullptr's are timespec and sigmask
        file_uring.process_events(event_cnt);
        batch_cnt++;
    }
    TRACE << "batch cnt: " << batch_cnt << std::endl;
}
