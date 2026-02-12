#include <liburing.h>
#include <netdb.h>
#include <netinet/in.h>
#include <openssl/md5.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <algorithm>
#include <iostream>
#include <stack>
#include <string>
#include <string_view>
#include <vector>

#include "commas.h"
#include "http_misc.h"
#include "get_nanoseconds.h"
#include "io_uring_wrapper.h"
#include "local_buffer.h"
#include "log.h"
#include "log_file.h"
#include "misc.h"
#include "string_helpers.h"
#include "string_view.h"

using std::string, dsy::string_view, std::vector;
using namespace std::literals;



struct port_info
{
    port_info(uint16_t p, bool use_ssl)
       : port(p), is_ssl(use_ssl)
    {} 
    uint16_t port = 8081;
    bool is_ssl = false;
};

std::string s_response_log("client.log");
log_file s_access_log;
thread_local std::string s_response_log_buffer("");

// some patterns for logging
//   - client threads lock, add to buffer, unlock, pretty fast
//   - writer thread peridically swaps the buffer and writes it out to disk
//   - handles the synchronization problem between threads writing to same file
//   - defaults to maintaining the order of writes
// keep writes (logs) under page size (4k) and write using io_uring after transaction is done
//   - each request object is responsible for building the log entry and writing it to disk
//   - seems simpler than having a dedicated writer thread, less locking going on between threads
//   - does result in smaller writes to the disc, which is less optimised on the write
//   - needs extra configs with io_uring to ensure order of writes
//      - wouldn't want access/error logs showing in the file out of chronological order

void log_response(uint32_t status, uint64_t microseconds, std::string_view url, size_t content_length, size_t bytes_received, MD5_CTX *md5_ctx)
{
    unsigned char digest[MD5_DIGEST_LENGTH];
    MD5_Final(digest, md5_ctx);

    s_response_log_buffer << get_milliseconds() << ' '
                          << status << ' '
                          << microseconds << ' '
                          << url << ' '
                          << content_length << ' '
                          << bytes_received << ' ';
    char buff[10];
    for (int i = 0; i < MD5_DIGEST_LENGTH; i++)
    {
        sprintf(buff, "%02x", digest[i]);
        s_response_log_buffer << buff;
    }

    s_response_log_buffer << '\n';

    s_access_log.log(s_response_log_buffer);

    s_response_log_buffer.clear();
}

#define BUFF_SIZE 1024 * 64

class http_11_client
{
public:
    http_11_client(const char *host, const char *port, std::string_view path, io_uring_wrapper<http_11_client> *iouring)
        :m_io_uring(iouring), m_host(host), m_port(port), m_path(path)
    {
    }

    bool get_addr_info()
    {
        addrinfo hints;
        addrinfo *rp = nullptr;

        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;    // Allow IPv4 or IPv6
        hints.ai_socktype = SOCK_STREAM; // STREAM socket
        hints.ai_flags = 0;
        hints.ai_protocol = 0;          // Any protocol

        int s = getaddrinfo(m_host, m_port, &hints, &m_result);
        if (s != 0)
        {
            ERROR << "getaddrinfo: " << gai_strerror(s) << ENDL;
            return false;
        }

        return true;
    }

    //
    // this is a re-entrant method
    //

    bool connect()
    {
        DEBUG(3) << "m_result: " << uint64_t(m_result) << ", m_fd: " << m_fd << THIS << ENDL;

        if (!m_result)
        {
            if (!this->get_addr_info())
            {
                return false;
            }
        }

        if (!m_start_connecting_ns)
            m_start_connecting_ns = get_nanoseconds();


        if (!m_rp)
        {
            m_rp = m_result;
        }
        else
        {
            m_rp = m_rp->ai_next;
        }

        if (!m_rp)
        {
            return false;
        }

        m_fd = socket(m_rp->ai_family, m_rp->ai_socktype, m_rp->ai_protocol);
        if (m_fd == -1)
        {
            ERROR << "socket() failed: " << ::strerror(errno) << ENDL;
            return false;
        }

        m_io_uring->prep_connect(m_fd, m_rp->ai_addr, m_rp->ai_addrlen, this); 
        m_state = http_11_state::CONNECTING;
        m_action = http_11_action::WAIT;
        DEBUG(3) << "prepped connect" << ENDL;
        return true;
    }

    uint32_t write_request()
    {
        m_start_sending_ns = get_nanoseconds();

        m_request.clear();
        m_request << "GET "sv << m_path << " HTTP/1.1\r\n"sv
                  << "Host: Bob\r\n"sv
                  << "\r\n"sv;
        m_io_uring->prep_write(m_fd, m_request.data(), m_request.size(), -1, this);
        DEBUG(2) << "prepped http request for writing, bytes: " << m_request.size() << ENDL;
        m_state = http_11_state::WRITING_REQUEST;
        m_action = http_11_action::WAIT;
        return 1;
    }
    
    uint32_t read_response_headers()
    {
        m_start_reading_ns = get_nanoseconds();
        DEBUG(3) << "starting read of response headers"sv << ENDL;
        auto [data_ptr, sz] = m_header_buff.remaining();
        m_io_uring->prep_read(m_fd, data_ptr, sz, 0, this);
        m_state = http_11_state::READING_RESPONSE_HEADERS;
        m_action = http_11_action::WAIT;
        return 1;
    }

    uint32_t process_response_headers(int res)
    {
        std::string_view buff(m_header_buff);
        size_t pos = buff.find("\r\n\r\n"sv);
        if (std::string_view::npos == pos)
        {
            DEBUG(3) << "DBCRLF not found, calling prep read, current bytes: " << buff.size()
                     << ", m_fd: " << m_fd << ENDL;
            auto [data_ptr, sz] = m_header_buff.remaining();
            if (sz == 0)
            {
                ERROR << "ran out of buffer room for headers, m_header_buff chars: "sv << m_header_buff.size() << ", m_fd: " << m_fd << THIS << ENDL;
                ERROR << buff.substr(0, 50) << ENDL;
                m_action = http_11_action::FAIL;
                return 0;
            }
            m_io_uring->prep_read(m_fd, data_ptr, sz, 0, this);
            DEBUG(3) << "prep'd read for more header data" << ENDL;
            m_state = http_11_state::READING_RESPONSE_HEADERS;
            m_action = http_11_action::WAIT;
            return 1;
        }

        header_bytes_received += pos;

        std::string_view hdrs = buff.substr(0, pos);
        std::string_view line1 = remove_before(hdrs, "\r\n"sv);
        std::string_view http_type = remove_before(line1, " "sv);
        uint32_t status = aton(remove_before(line1, " "sv));
        std::string_view desc = line1;

        DEBUG(3) << "found DBCFLR in headers, m_fd: " << m_fd << ENDL;
        DEBUG(3) << hdrs << ENDL;

        while (!hdrs.empty())
        {
            std::string_view line = remove_before(hdrs, "\r\n"sv);
            auto [key, val] = split(line, ':'); 
            key.rtrim(dsy::whitespace);
            val.ltrim(dsy::whitespace).rtrim(dsy::whitespace);

            DEBUG(2) << "key: " << key << ", val: " << val << ENDL;
            switch (toupper(key.at(0))) {
            case 'C':
                if (key.equals_ci("Content-Length"sv))
                {
                    if (!val.aton(m_content_length))
                    {
                        ERROR << "Failed to parse Content-Length: " << val << ENDL;
                    }
                    else
                    {
                        DEBUG(3) << "Response Content-Length: " << m_content_length
                                 << ", m_fd: " << m_fd << ENDL;
                    }
                }
                else if (key.equals_ci("Connection"sv))
                {
                    m_connection = val;
                }
                else
                {
                    WARN << "unhandled header: '" << key << "'" << ENDL; 
                }
                break;
            default:
                break;
            }
        }

        string_view remainder = buff.substr(pos + 4);

        DEBUG(2) << "content size included with header data: " << remainder.size()
                 << ", m_fd: " << m_fd << ENDL;

        if (remainder.size()) // read some of the body
        {
            return process_response_body(remainder);
        }

        DEBUG(2) << "Content-Length: " << m_content_length
                 << ", m_content_received: " << m_content_received
                 << ", m_fd: " << m_fd << ENDL;

        if (remainder.empty() && m_content_length > m_content_received)
        {
            DEBUG(2) << "Content-Length: " << m_content_length << ", calling read_response_body"
                     << ", m_fd: " << m_fd << ENDL;
            return read_response_body();
        }

        DEBUG(2) << "Request processing done, m_content_length: " << m_content_length
                 << ", m_content_received: " << m_content_received
                 << ", m_fd: " << m_fd
                 << ENDL;
        m_state = http_11_state::DONE;
        m_action = http_11_action::DONE;
        return 0;
    }

    uint32_t read_response_body()
    {
        DEBUG(3) << "starting read of response body, m_fd: "sv << m_fd << ENDL;
        auto [data_ptr, sz] = m_body_buff.remaining();
        if (sz == 0)
        {
            ERROR << "m_body_buff is full, m_fd: " << m_fd << THIS << ENDL;
            m_action = http_11_action::FAIL;
            return 0;
        }
        m_io_uring->prep_read(m_fd, data_ptr, sz, 0, this);
        m_state = http_11_state::READING_RESPONSE_BODY;
        m_action = http_11_action::WAIT;
        return 1;
    }

    // the body could be at end of headers or in m_body_buff so pass it in
    uint32_t process_response_body(std::string_view body)
    {
        m_content_received += body.size();
        body_bytes_received += body.size();
        MD5_Update(&m_md5_ctx, body.data(), body.size());
        //DEBUG(3) << body << ENDL;
        DEBUG(3) << "m_content_received >= m_content_length?, " << m_content_received << " >= " << m_content_length << ", m_fd: " << m_fd << THIS << ENDL;
        m_body_buff.clear();
        if (m_content_received >= m_content_length)
        {
            DEBUG(3) << "DONE, m_fd: " << m_fd << THIS << ENDL;
            m_state = http_11_state::DONE;
            m_action = http_11_action::DONE;
            return 0;
        }
        return read_response_body();
    }

    uint32_t process_io_uring(int res)
    {
        if (!m_io_uring)
        {
            // was reset?? how to get here?
            ERROR << "m_io_uring is NULL, why/what/how? m_fd: " << m_fd << THIS << ENDL;
            return 0;
        }

        using enum http_11_state;

        DEBUG(3) << "state: " << to_str(m_state) << ", res: " << res << ", m_fd: " << m_fd << THIS << ENDL;
        switch (m_state) {
        case CONNECTING:
            if (res < 0)
            {
                return this->connect();
            }
            m_state = http_11_state::CONNECTED;
            DEBUG(3) << "got connection: " << m_fd << ", ns: " << commas(get_nanoseconds() - m_start_connecting_ns) << ENDL;
            // fall through to CONNECTED
        case CONNECTED:
            write_request();
            break;
        case WRITING_REQUEST:
            DEBUG(3) << "Request was sent, res: " << res << ", m_fd: " << m_fd << THIS << ENDL;
            read_response_headers();
            break;
        case READING_RESPONSE_HEADERS:
            if (res < 0)
            {
                ERROR << "Failed to read response headers: " << ::strerror(-res) << ENDL;
                m_action = http_11_action::FAIL;
            }
            else
            {
                m_header_buff.add_size(res);
                process_response_headers(res);
            }
            break;
        case READING_RESPONSE_BODY:
            if (res < 0)
            {
                ERROR << "Failed to read body: " << ::strerror(-res) << ENDL;
                m_action = http_11_action::FAIL;
            }
            else if (res > 0)
            {
                m_body_buff.add_size(res);
                process_response_body(m_body_buff.str());
            }
            else // EOF
            {
                DEBUG(3) << "res: " << res << ", EOF, m_fd: " << m_fd << THIS << ENDL;
                this->close();
            }
            break;
        };

        if (m_action == http_11_action::WAIT)
        {
            return 1;
        }

        if (m_action == http_11_action::FAIL)
        {
            requests_failed++;
            this->close();
        }

        if (m_action == http_11_action::DONE)
        {
            requests_succeeded++;
            m_done_ns = get_nanoseconds();
            uint64_t total_ns = m_done_ns - m_start_sending_ns;

            add_times(m_start_sending_ns - m_start_connecting_ns,
                      m_start_reading_ns - m_start_sending_ns,
                      m_done_ns - m_start_reading_ns);

            log_response(200, total_ns / 1000, m_path, m_content_length, m_content_received, &m_md5_ctx);
            this->close();
        }
            
        return 0;
    }

    http_11_state state() const { return m_state; }
    std::string_view state_str() const { return to_str(m_state); }

    void close()
    {
        if (m_fd >= 0)
        {
            DEBUG(3) << "m_fd: " << m_fd << THIS << ENDL;
            ::close(m_fd);
            m_fd = -1;
            if (active_clients)
                active_clients--;
        }
        this->reset();
    }

    void reset()
    {
        m_header_buff.clear();
        m_body_buff.clear();
        m_request.clear();
        m_path = ""sv;
        //m_fd = -1;  // need fd for keep alive requests
        m_io_uring = nullptr;
        m_result = nullptr;
        m_rp = nullptr;
        m_host = nullptr;
        m_port = nullptr;
        m_content_length = 0;
        m_content_received = 0;

        MD5_Init(&m_md5_ctx);
    }

    void reset(const char *host, const char *port, std::string_view path, io_uring_wrapper<http_11_client> *iouring)
    {
        DEBUG(2) << "resetting host/port/path/etc" << ", m_fd: " << m_fd << THIS << ENDL;
        if (-1 != m_fd)
        {
            ::close(m_fd);
            m_fd = -1;
        }
        this->reset();
        m_host = host;
        m_port = port;
        m_path = path;
        m_io_uring = iouring;

        m_state = http_11_state::CREATED;
        m_action = http_11_action::DONE;
    }

private:
    int m_fd = -1;
    io_uring_wrapper<http_11_client> *m_io_uring;
    addrinfo *m_result = nullptr;
    addrinfo *m_rp = nullptr;
    std::string m_request;
    const char *m_host;
    const char *m_port;
    std::string_view m_path;
    http_11_state m_state = http_11_state::CREATED;
    http_11_action m_action = http_11_action::DONE;
    // TODO reconsider use of local_buffer here, might have to go with a std::string to allow for larger headers
    // thinking: want to be able to accept large headers but not drag around a large buffer on each client object
    //           could we have a vector of fixed sized buffers that we pull from a free list/vector,
    //           then stick back in the free list when done parsing headers?
    //           or if we are not proxying the headers upstream we could parse as we receive them?? will we proxy?
    local_buffer<char, BUFF_SIZE> m_header_buff;
    local_buffer<char, BUFF_SIZE> m_body_buff;

    size_t m_content_length = 0;
    size_t m_content_received = 0;
    std::string_view m_connection;

    uint64_t m_start_connecting_ns = 0;
    uint64_t m_start_sending_ns = 0;
    uint64_t m_start_reading_ns = 0;
    uint64_t m_done_ns = 0;

    MD5_CTX m_md5_ctx;
public:
    inline static uint32_t active_clients = 0;
    inline static uint64_t requests_succeeded = 0;
    inline static uint64_t requests_failed = 0;
    inline static uint64_t header_bytes_received = 0;
    inline static uint64_t body_bytes_received = 0;
    inline static uint64_t connecting_ns = 0;
    inline static uint64_t sending_ns = 0;
    inline static uint64_t reading_ns = 0;

    inline static std::vector<uint64_t> connecting_ns_vector;
    inline static std::vector<uint64_t> sending_ns_vector;
    inline static std::vector<uint64_t> reading_ns_vector;

    static void init_storage(uint64_t cnt)
    {
        connecting_ns_vector.reserve(cnt);
        sending_ns_vector.reserve(cnt);
        reading_ns_vector.reserve(cnt);
    }

    static void add_times(uint64_t conn_ns, uint64_t send_ns, uint64_t read_ns)
    {
        connecting_ns += conn_ns;
        connecting_ns_vector.push_back(conn_ns);

        sending_ns += send_ns;
        sending_ns_vector.push_back(send_ns);

        reading_ns += read_ns;
        reading_ns_vector.push_back(read_ns);
    }

    static void sort_vects()
    {
        std::sort(connecting_ns_vector.begin(), connecting_ns_vector.end());
        std::sort(sending_ns_vector.begin(), sending_ns_vector.end());
        std::sort(reading_ns_vector.begin(), reading_ns_vector.end());
    }
    
    static auto median_val(auto &vec)
    {
        size_t sz = vec.size();
        size_t mid = sz / 2;
        return vec[mid];
    }

    static void trace()
    {
        uint64_t reqs = requests_succeeded + requests_failed;
        if (reqs == 0)
        {
            ERROR << "reqs: " << reqs << ", requests_succeeded: " << requests_succeeded << ", requests_failed: " << requests_failed << ENDL;
            reqs = 1;
        }

        TRACE << "requests_succeeded: " << add_commas(requests_succeeded)
              << ", requests_failed: " << add_commas(requests_failed)
              << ", header_bytes_received: " << add_commas(header_bytes_received)
              << ", body_bytes_received: " << add_commas(body_bytes_received)
              << ENDL;
        TRACE << "Avg: conn ns: " << add_commas(connecting_ns/reqs)
              << ", send ns: " << add_commas(sending_ns/reqs)
              << ", read ns: " << add_commas(reading_ns/reqs)
              << ENDL;
        sort_vects();
        TRACE << "Median: conn ns: " << add_commas(median_val(connecting_ns_vector))
              << ", send ns: " << add_commas(median_val(sending_ns_vector))
              << ", read ns: " << add_commas(median_val(reading_ns_vector))
              << ENDL;
    }
};

int main (int argc, char **argv)
{
    uint32_t queue_depth = 500; // use queue_depth as a batch size to complete <cnt> requests
    uint32_t cnt = 1;
    std::string error_log_dir("logs");
    std::string error_log_name("client_error.log");
    std::string access_log_dir("logs");
    std::string access_log_name("client_access.log");

    for (int i = 1; i < argc; i++)
    {
        auto [key, val] = split(string_view(argv[i]), '=');
        if (key == "--queue-depth"sv)
        {
            if (!aton(val, queue_depth))
            {
                ERROR << "Failed to convert '" << val << "' to a queue depth" << ENDL;
            } 
        }
        else if (key == "--cnt"sv)
        {
            cnt = aton(val);
            http_11_client::init_storage(cnt);
        }
        else if (key == "--debug"sv)
        {
            s_debug_level = aton(val);
        }
        else if (key == "--error-log-dir"sv)
        {
            error_log_dir = val;
        }
        else if (key == "--error-log-name"sv)
        {
            error_log_name = val;
        }
        else if (key == "--access-log-dir"sv)
        {
            access_log_dir = val;
        }
        else if (key == "--access-log-name"sv)
        {
            access_log_name = val;
        }
    }

    set_error_log_name(error_log_dir.data(), error_log_name.data());
    s_access_log.set_log_name(access_log_dir, access_log_name);

    io_uring_wrapper<http_11_client> iouring(queue_depth * 2);

    std::string host("127.0.0.1");
    std::string port("9091");
    std::string path("/");

    for (int i = 1; i < argc; i++)
    {
        auto [key, val] = split(string_view(argv[i]), '=');
        if (key == "--host"sv)
        {
            host = val;
        }
        else if (key == "--port"sv)
        {
            port = val;
        }
        else if (key == "--path"sv)
        {
            path = val;
            DEBUG(2) << "path: " << path << ENDL;
        }
    }

    uint64_t start = get_nanoseconds();
    uint32_t req_cnt = 0;
    vector<http_11_client*> clients;
    for (uint32_t i = 0; i < queue_depth; i++)
    {
        clients.push_back(new http_11_client(nullptr, nullptr, ""sv, nullptr));
    }

    TRACE << "Running " << cnt << " requests in batches of " << clients.size() << ENDL;

    uint64_t prev_admin_ms = get_milliseconds();

    // add the client objects, prepare the connects
    uint32_t loop = 0;

    while (cnt > req_cnt)
    {
        DEBUG(2) << "request loop " << loop++ << ", cnt: " << cnt << ", req_cnt: " << req_cnt << ENDL;
        uint64_t cur_admin_ms = get_milliseconds();
        if (cur_admin_ms - prev_admin_ms > 100)
        {
            process_error_log_events();
            s_access_log.process_events();
            prev_admin_ms = cur_admin_ms;
        }

        for (auto client : clients)
        {
            client->reset(host.data(), port.data(), path, &iouring);
            if (!client->connect())
            {
                client->reset();
            }
            else
            {
                req_cnt++;
                http_11_client::active_clients++;
            }

            if (req_cnt >= cnt)
            {
                break;
            }
        }

        iouring.submit(); // submit the connects to the kernel 

        while (http_11_client::active_clients)
        {
            if (0 == iouring.process_events(http_11_client::active_clients))
                ::usleep(100000);
            DEBUG(5) << "active clients: " << http_11_client::active_clients << ENDL;
        }
    }

    process_error_log_events();
    s_access_log.process_events();

    uint64_t end = get_nanoseconds();
    uint64_t total = end - start;
    uint64_t each = total / req_cnt; 
    TRACE << "Req cnt: " << req_cnt << ", total ns: " << add_commas(total) << ", ns each: " << add_commas(each) << ENDL;
    http_11_client::trace(); 

}
