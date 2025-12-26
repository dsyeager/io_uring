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

#include <atomic>
#include <charconv>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

using std::cout, std::cerr, std::endl, std::string, std::string_view, std::vector;
using namespace std::literals;

#define BUFFER_SZ 1024

static uint64_t s_completed_requests = 0;

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
    io_uring *m_ring = nullptr;

public:
    client_request(int input_fd, std::string_view output_path, uint32_t index, io_uring *ring)
    : m_input_fd(dup(input_fd)), m_index(index), m_ring(ring)
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
        if (!m_ring)
        {
            ERROR << "m_ring is NULL" << std::endl;
            return false;
        }

        io_uring_sqe *sqe = io_uring_get_sqe(m_ring);
        if (!sqe)
        {
            ERROR << "io_uring_get_sqe: failed to get SQE" << std::endl;
            io_uring_queue_exit(m_ring);
            return false;
        }

        io_uring_prep_read(sqe, m_input_fd, m_buffer, BUFFER_SZ, 0);
        io_uring_sqe_set_data(sqe, (void*) this);

        return true;
    }

    uint32_t process(int res)
    {
        if (res > 0)
        {
            io_uring_sqe *sqe = io_uring_get_sqe(m_ring);
            if (!sqe)
            {
                ERROR << "io_uring_get_sqe: failed to get SQE" << std::endl;
                io_uring_queue_exit(m_ring);
                return 0;
            }
            io_uring_sqe_set_data(sqe, (void*) this);

            switch (m_state) {
            case READING_CLIENT_INPUT:
                // Read successful. Write to stdout.
                DEBUG(1) << "writing " << res << " bytes to m_output_fd: " << m_output_fd << ENDL;
                io_uring_prep_write(sqe, m_output_fd, m_buffer, res, -1);
                m_state = WRITING_TO_FILE;
                m_offset += res;
                break;
            case WRITING_TO_FILE:
                m_bytes_written += res;
                DEBUG(1) << "reading up to " << BUFFER_SZ << " bytes from m_input_fd: " << m_input_fd << ENDL;
                io_uring_prep_read(sqe, m_input_fd, m_buffer, BUFFER_SZ, m_offset);
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
            ERROR << strerror(abs(res)) << endl;
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
        else if (key == "--debug"sv)
        {
            s_debug_level = aton(val);
        } 
    }

    int input_fd = ::open(input.data(), O_RDONLY);
    if (input_fd < 0)
    {
        ERROR << "Failed to open " << input << ", " << strerror(errno) << endl;
        return 0;
    }

    struct io_uring ring;

    // 1. Initialize the io_uring instance with a queue depth of 1
    int ret = io_uring_queue_init(cnt + 10, &ring, 0);
    if (ret < 0)
    {
        ERROR << "io_uring_queue_init: " << ::strerror(-ret) << std::endl;
        return 1;
    }

    off_t offset = 0;

    vector<client_request*> requests;

    for (uint32_t i = 0; i < cnt; i++)
    {
        requests.push_back(new client_request(input_fd, output, i, &ring));
        requests.back()->start_io_uring();
    }
    io_uring_submit(&ring);


    io_uring_cqe *cqe = nullptr;

    uint32_t batch_cnt = 0;

    while (s_completed_requests < requests.size())
    {
        // last two nullptr's are timespec and sigmask
        if (0 == io_uring_wait_cqes(&ring, &cqe, std::min(1000UL, requests.size() - s_completed_requests), nullptr, nullptr))
        {
            unsigned head;
            unsigned i = 0;
            uint32_t new_events = 0;

            io_uring_for_each_cqe(&ring, head, cqe)
            {
                 client_request *req = (client_request*) io_uring_cqe_get_data(cqe);
                 new_events += req->process(cqe->res);
                 i++;
            }

            DEBUG(1) << "batch events: " << i << ENDL;
            batch_cnt++;

            if (new_events)
                io_uring_submit(&ring);

            io_uring_cq_advance(&ring, i);

        }
    }
    TRACE << "batch cnt: " << batch_cnt << std::endl;

    io_uring_queue_exit(&ring);
}
