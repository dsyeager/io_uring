#include <liburing.h>
#include <netinet/in.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>

#include <iostream>
#include <stack>
#include <string>
#include <string_view>
#include <vector>

#include "commas.h"
#include "get_nanoseconds.h"
#include "get_milliseconds.h"
#include "http_misc.h"
#include "io_uring_wrapper.h"
#include "local_buffer.h"
#include "log.h"
#include "log_file.h"
#include "misc.h"
#include "string_helpers.h"
#include "string_view.h"
#include "tc_object_pool.h"
#include "time_it.h"

using std::string, dsy::string_view, std::vector;
using namespace std::literals;

struct port_info
{
    port_info(uint16_t p, bool use_ssl, int fd)
       : port(p), is_ssl(use_ssl), listening_fd(fd)
    {} 
    uint16_t port = 8081;
    bool is_ssl = false;
    int listening_fd = -1;
};

log_file s_access_log;
thread_local std::string s_response_log_buffer("");

void log_response(uint32_t status,
                  uint64_t microseconds,
                  std::string_view url,
                  size_t request_header_size,
                  size_t request_body_size,
                  size_t response_content_length,
                  size_t response_header_bytes_sent,
                  size_t response_body_bytes_sent)
{
    s_response_log_buffer << get_milliseconds() << ' '
                          << status << ' '
                          << microseconds << ' '
                          << url << ' '
                          << request_header_size << ' '
                          << request_body_size << ' '
                          << response_content_length << ' '
                          << response_header_bytes_sent << ' '
                          << response_body_bytes_sent;

    s_response_log_buffer << '\n';

    s_access_log.log(s_response_log_buffer);

    s_response_log_buffer.clear();
}

/*
   http_11_listener is listening on N ports.
   Each connection accepted creates an http_11_connection for tracking the progress of that connection.
*/

// the listener and connection are intertwined
// or should the connection class maintain a static pool and active clt?.......
class http_11_connection;
class http_11_listener;

#define BUFF_SIZE 1024
#define BODY_BUFF_SIZE 1024 * 64 

class http_11_connection : public tc_object_pool_obj<http_11_connection>
{
public:
    http_11_connection()
        :m_buff(new char[BUFF_SIZE])
    {
    }

    void start_processing(int fd,
                          io_uring_wrapper<http_11_connection> *iouring,
                          tc_object_pool<http_11_connection> *pool)
    {
        time_it atime("start_processing");
        m_accept_ns = get_nanoseconds();
        m_client_fd = fd;
        m_io_uring = iouring;
        m_object_pool = pool;
        m_body_buff.clear();
        m_buff_len = 0;
        m_io_uring->prep_read(m_client_fd, m_buff, BUFF_SIZE, 0, this);
        m_state = http_11_state::READING_REQUEST_HEADERS;
    }

    void process_headers()
    {
        std::string_view buff(m_buff, m_buff_len);
        size_t dcrlf = buff.find("\r\n\r\n"sv);
        if (dcrlf == std::string_view::npos)
        {
DEBUG(2) << "m_buff_len: "sv << m_buff_len << ", DCRLF not found, reading again"sv << ENDL;
            // ignoring that headers may be larger than BUFF_SIZE
            m_io_uring->prep_read(m_client_fd, m_buff, BUFF_SIZE - m_buff_len, m_buff_len, this);
            m_action = http_11_action::WAIT;
            return;
        }
DEBUG(1) << "m_buff_len: " << m_buff_len << ", dcrlf: " << dcrlf << ENDL;
        std::string_view headers(m_buff, dcrlf);
        std::string_view body(headers);
        m_request_headers_size = headers.size();
        body.remove_prefix(dcrlf + 4); // remove the headers and dcrlf chars
        std::string_view line1 = remove_before(headers, "\r\n"sv);
DEBUG(2) << "m_buff_len: " << m_buff_len << ", line1: " << line1 << ENDL;
        std::string_view type = remove_before(line1, " "sv); // TODO change to remove_before_any for whitespace

        m_request_type = from_str(type);
        m_path = remove_before(line1, " "sv);
        remove_before(line1, "/"sv);
        std::string_view version = line1;

        // keep this simple, not handling extra whitespaces, lines etc

        while (!headers.empty())
        {
            std::string_view line = remove_before(headers, "\r\n"sv); // NOTE: does not handle multi line headers.
            if (line.empty())
                continue;

            auto [key, val] = split(line, ' ');

            if (key.starts_with('H'))
            {
                if (key == "Host:"sv)
                {
                    m_host = val;
                }
            }
            else if (key.starts_with('C'))
            {
                if (key == "Content-Length:"sv)
                {
                    m_content_length = aton(val);
                }
            }
            else
            {
                DEBUG(2) << "Unhandled header: " << key << ' ' << val << ENDL;
            }
        }
        m_headers_parsed = true;

        if (body.length() < m_content_length)
        {
            DEBUG(1) << "Request body already read with headers, body.length: " << body.length() << ENDL;
            m_state = http_11_state::READING_REQUEST_BODY;
            m_action = http_11_action::AGAIN;
            // TODO make sure m_read_ns is set when we get there
        }
        else
        {
            m_read_ns = get_nanoseconds();
            start_response();
        }
    }

    void start_response()
    {
        // a few common file extensions 
        if (m_path.ends_with(".html"sv) || m_path.ends_with(".jpg"sv) || m_path.ends_with("/"sv))
        {
            serve_file(0);
        }
        else if (m_path.ends_with("/hello-world"sv))
        {
DEBUG(2) << "is hello-world, responding with path: " << m_path << ENDL;
            m_response_headers.clear();
            m_response_headers << "HTTP/1.1 200 OK\r\n"sv
                               << "Server: Bob\r\n"sv
                               << "Content-Length: "sv << m_path.size() << "\r\n"sv
                               << "\r\n"sv;
            m_response_headers_size = m_response_headers.size();
            m_response_headers << m_path;
            m_io_uring->prep_write(m_client_fd, m_response_headers.data(), m_response_headers.size(), -1, this);
            m_total_bytes_to_write = m_response_headers.size();
            m_state = http_11_state::WRITING_RESPONSE_BODY; // includes body so skip the WRITING_RESPONSE_HEADERS stage
            m_action = http_11_action::WAIT;
        }
        // add any other apis that will be handled in process
        else
        {
            // default action
            serve_file(0);
        }
    }

    void start_response_body()
    {
        m_action = http_11_action::DONE;
    }

    void serve_file(int res)
    {
        // a re-entrant method
        // TODO handle ..'s in the path. Could attempt to normalize the path or just change path to an error file
        if (m_get_fd == -1)
        {
            string_view file_path(m_path);
            file_path.remove_prefix(1);
            m_file_path = file_path; // io_uring wants a null terminated file path
            if (m_path.ends_with('/'))
            {
                m_file_path += "index.html";
            }

            DEBUG(2) << "opening " << m_file_path << ENDL;
            m_io_uring->prep_open_at(m_dir_fd, m_file_path.data(), O_RDONLY, 0666, this);
            m_state = http_11_state::OPENING_GET_FILE;
            m_action = http_11_action::WAIT;
            return;
        }

        if (m_state == http_11_state::WRITING_RESPONSE_HEADERS)
        {
            m_response_headers_sent = true;
        }

        // need to send response headers
        if (!m_response_headers_sent)
        {
            m_response_headers.clear();
            m_status = 200;
            m_response_headers << "HTTP/1.1 " << m_status << " OK"sv << "\r\n"sv
                               << "Server: Bob"sv << "\r\n"sv
                               << "Content-Length: "sv << m_total_bytes_to_write << "\r\n"sv
                               << "\r\n"sv;

            m_response_headers_size = m_response_headers.size();
            m_io_uring->prep_write(m_client_fd, m_response_headers.data(), m_response_headers.size(), -1, this);
            m_state = http_11_state::WRITING_RESPONSE_HEADERS;
            m_action = http_11_action::WAIT;
            return;
        }

        // read and write the file the stupid way for now
        // 1. mainly to get it working fast
        // 2. also because TLS support is not fully baked into io_uring yet
        // there is a way using pipe2 to create 2 pipes then use io_uring to transfer data directly from file to socket with zero copies into user land.
        // but that would need kernel TLS support to handle most web server traffic

        // step 1. read from file into a local_buffer of N bytes
        // step 2. write from local_buffer to client fd
        // step 3. loop on 1 and 2 until we've sent the whole file

        if (m_state == http_11_state::READING_GET_FILE)
        {
            // we read in <res> bytes so adjust the size value of buffer
DEBUG(2) << "read " << res << " bytes from m_get_fd: " << m_get_fd << ENDL;
if (0 == res)
{
    // EOF but why are we here
    TRACE << "m_total_bytes_written: " << m_total_bytes_written << ENDL;
            m_state = http_11_state::FAILED;
    bool m_is_ssl = false;
    bool m_headers_parsed = false;
            m_action = http_11_action::FAIL;
            return;
}
            m_body_buff.set_size(res);
        }

        if (m_state == http_11_state::WRITING_RESPONSE_BODY)
        {
            if (m_body_buff.size() - m_body_offset <= res)
            {
                m_body_buff.clear();
                m_body_offset = 0;
            }
        }

        if (!m_body_buff.size())
        {
            auto [data_ptr, sz] = m_body_buff.remaining();
            m_io_uring->prep_read(m_get_fd, data_ptr, sz, -1, this);
            m_state = http_11_state::READING_GET_FILE;
            m_action = http_11_action::WAIT;
            return;
        }
        else // write to client
        {
            size_t offset = m_body_offset;
            // might have done a partial write, maybe
            // TODO: could do multiple partial writes suggesting would need to track the previous offset

            if (m_state == http_11_state::WRITING_RESPONSE_BODY)
            {
                if (res > 0)
                {
                    offset += res;
                }
            } 
            string_view data = m_body_buff.str();
            data.remove_prefix(offset);
            m_io_uring->prep_write(m_client_fd, data.data(), data.size(), -1, this);
            m_state = http_11_state::WRITING_RESPONSE_BODY;
            m_action = http_11_action::WAIT;
            m_body_offset = offset;
            return;
        }
    } 

    // event loops I've seen depend on the sub methods returning or setting an action enum like WAITING, AGAIN, DONE, etc
    // WAITING IO event has been scheduled, exit the loop returning 1
    // AGAIN loop on the event loop again, like when transitioning between states 
    // DONE exit the loop returning 0
    uint32_t process_io_uring(int res)
    {
        // TODO: maybe loop on this allowing the request/conn to flow between stages via the loop
        switch (m_state) {
        case http_11_state::READING_REQUEST_HEADERS:
            if (res < 0)
            {
                ERROR << "read of headers failed: " << ::strerror(-res) << ENDL;
                m_action = http_11_action::FAIL;
            }
            else if (res > 0)
            {
                DEBUG(2) << "read " << res << " bytes from socket: " << m_client_fd << ENDL;
                m_buff_len += res;
                process_headers();
            }
            else // EOF
            {
                m_state = http_11_state::DONE;
                m_action = http_11_action::TERM;
            }

            break;
        case http_11_state::WRITING_RESPONSE_HEADERS:
            if (res < 0)
            {
                ERROR << "write of headers failed: " << ::strerror(-res) << ENDL;
                m_action = http_11_action::FAIL;
            }
            else
            {
                // are partial writes possible? do we need to compare res with what we queued up?
                if (m_get_fd != -1)
                {
                    serve_file(res);
                }
                else
                {
                    // short circuits for now
                    start_response_body();
                }
            }
            break;
        case http_11_state::WRITING_RESPONSE_BODY:
            if (res < 0)
            {
                ERROR << "write of body failed: " << ::strerror(-res) << ENDL;
                m_action = http_11_action::FAIL;
                break;
            }

            m_total_bytes_written += res;
            if (m_total_bytes_written >= m_total_bytes_to_write)
            {
                DEBUG(3) << "response DONE, m_total_bytes_written: " << m_total_bytes_written << ", m_total_bytes_to_write: " << m_total_bytes_to_write << ENDL;
                m_action = http_11_action::DONE;
            }
            else if (m_get_fd != -1)
            {
                serve_file(res);
            }
            else if (m_total_bytes_to_write == m_response_headers.size())
            {
                m_io_uring->prep_write(m_client_fd,
                                       m_response_headers.data() + m_total_bytes_written,
                                       m_response_headers.size() - m_total_bytes_written,
                                       -1,
                                       this);
                m_action = http_11_action::WAIT;
            }
            break;
        case http_11_state::OPENING_GET_FILE:
            if (res < 0)
            {
                ERROR << "failed to open file: " << m_file_path << ": " << ::strerror(-res) << ENDL;
                // TODO wire in 404's and other failure codes
                m_action == http_11_action::FAIL;
                break;
            }
            m_get_fd = res;
            struct stat sb;
            if (::fstat(m_get_fd, &sb) == -1)
            {
                ERROR << "failed to stat GET file: " << ::strerror(errno) << ENDL;
                m_action == http_11_action::FAIL;
                break;
            }
            m_total_bytes_to_write = sb.st_size;
            m_file_size = sb.st_size;

            serve_file(res);
            break;
        case http_11_state::READING_GET_FILE:
            if (res < 0)
            {
                ERROR << "failed to open file: " << m_file_path << ": " << ::strerror(-res) << ENDL;
                // TODO wire in 404's and other failure codes
                m_action == http_11_action::FAIL;
                break;
            }
            serve_file(res);
            break;
        default:
            ERROR << "unhandled state: " << to_str(m_state) << ENDL;
        };

        if (http_11_action::FAIL == m_action)
        {
            // don't know if it was a clean or dirty fail so close the connection, client can reconnect
            cleanup(true);
            return 0;
        }

        if (http_11_action::TERM == m_action)
        {
            cleanup(true);
            return 0;
        }
 
        if (http_11_action::DONE == m_action)
        {
DEBUG(1) << "DONE: m_total_bytes_written: " << m_total_bytes_written << ", m_total_bytes_to_write: " << m_total_bytes_to_write << ENDL;
            m_write_ns = get_nanoseconds();
            uint64_t total_ns = m_write_ns - m_accept_ns;
            log_response(m_status,
                         total_ns / 1000,
                         m_file_path,
                         m_request_headers_size,
                         m_content_length,
                         m_total_bytes_to_write,
                         m_response_headers_size,
                         m_total_bytes_written);
            // TODO: look for keep alive
            // process the next request
      cleanup(true);
      return 0;

            start_processing(m_client_fd, m_io_uring, m_object_pool);
            return 1;
        }

        if (m_action == http_11_action::WAIT)
        {
            return 1;
        }

        return 0;
    }

    void cleanup(bool close_client)
    {
        if (m_get_fd)
        {
            ::close(m_get_fd);
            m_get_fd = -1;
        }
        if (m_client_fd && close_client)
        {
            ::close(m_client_fd);
            m_client_fd = -1;
        }
        m_total_bytes_to_write = 0;
        m_total_bytes_written = 0;
        m_response_headers_sent = false;

        m_object_pool->put(this);
    }
        
    inline static
    void set_dir_fd(int dir_fd)
    {
        // simple for now, might want multiple dir fds depending on disc layout
        m_dir_fd = dir_fd;
    }

private:
    void add_status_line(uint32_t status)
    {
        m_response_headers << "HTTP/1.1 "sv << status << " OK"sv << "\r\n"sv;
    }

    void add_content_length(size_t len)
    {
        m_response_headers << "Content-Length: "sv << len << "\r\n"sv;
    }

    void add_server()
    {
        m_response_headers << "Server: Bob\r\n"sv;
    }
private:

    int m_client_fd = -1;
    int m_get_fd = -1;
    uint32_t m_active_index = 0;
    io_uring_wrapper<http_11_connection> *m_io_uring = nullptr;
    tc_object_pool<http_11_connection> *m_object_pool = nullptr;
    http_11_state m_state = http_11_state::CREATED;
    http_11_action m_action = http_11_action::DONE;
    char *m_buff = nullptr;
    size_t m_buff_len = 0;
    size_t m_file_size = 0;
    size_t m_total_bytes_to_write = 0;
    size_t m_total_bytes_written = 0;

    request_type m_request_type = request_type::GET;
    size_t m_content_length = 0;
    std::string_view m_host;
    std::string_view m_path;
    std::string m_file_path;
    std::string m_response_headers;
    bool m_response_headers_sent = false;
    bool m_is_ssl = false;
    bool m_headers_parsed = false;
    local_buffer<char, BODY_BUFF_SIZE> m_body_buff;
    size_t m_body_offset = 0;
    uint32_t m_status = 0;
    uint32_t m_request_headers_size = 0;
    uint32_t m_response_headers_size = 0;

    uint64_t m_accept_ns = 0;
    uint64_t m_read_ns = 0;  // done reading request
    uint64_t m_write_ns = 0; // done writing request

private:
    inline static int m_dir_fd = -1;
};

class http_11_listener
{
    friend http_11_connection;
public:
    http_11_listener(uint32_t accept_queue_depth, uint32_t conn_queue_depth)
        :m_io_uring_accept(accept_queue_depth), m_io_uring(conn_queue_depth)
    {
        signal(SIGPIPE, SIG_IGN);

        m_conn_pool.reserve_active(10000);
        m_conn_pool.reserve_free(10000);
    }

    bool listen(int port, bool is_ssl)
    {
        for (port_info *pi : m_listening_fds)
        {
            if (pi->port == port)
            {
                ERROR << "alreading listening on port " << port << ENDL;
                return false;
            }
        }

        int fd = socket(AF_INET, SOCK_STREAM, 0);

        struct sockaddr_in server_addr;
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        server_addr.sin_port = htons(port);
        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

        // do we need to make it nonblocking
        // setting the O_NONBLOCK flag on a socket file descriptor using fcntl

        int err = ::bind(fd, (struct sockaddr *) &server_addr, sizeof(server_addr));
        if (err < 0)
        {
            ERROR << "bind error: " << ::strerror(errno) << ENDL;
            return false;
        }

        err = ::listen(fd, 1024);
        if (err < 0)
        {
            ERROR << "listen error: " << ::strerror(errno) << ENDL;
            return false;
        }

        m_listening_fds.push_back(new port_info(port, is_ssl, fd));

        accept_on_port(m_listening_fds.back());

        return true;
    }

    void accept_on_port(port_info *pi)
    {
        TRACE << "submitting accept on port " << pi->port << ", pi: " << pi << ENDL;

        m_io_uring_accept.prep_multishot_accept(pi->listening_fd, this);
        m_io_uring_accept.submit();

        TRACE << "multishot accept submitted" << ENDL;
    }

    uint32_t accept(uint32_t limit = 50)
    {
        uint32_t new_events = m_io_uring_accept.process_events(limit);

        DEBUG(3) << "accepted " << new_events << " connections" << ENDL;

        // the read events are in a diff io_uring_wrapper so have to manually submit them.
        if (new_events)
            m_io_uring.submit(); // push initial reads into kernel 

        return new_events;
    }

    uint32_t process_events()
    {
        uint32_t new_events = m_io_uring.process_events();
        if (new_events)
            m_io_uring.submit();

        return new_events;
    }

    //
    // callback used by io_uring_wrapper::process_events
    // might want the io_uring_cqe* also
    //

    uint32_t process_io_uring(int fd)
    {
        time_it atime("process_io_uring");
        DEBUG(3) << "accepted connection, fd: " << fd << ENDL;
        if (fd >= 0)
        {
            m_conn_pool.get()->start_processing(fd, &m_io_uring, &m_conn_pool);
        }
        return 0;
    }

    bool empty() const { return m_listening_fds.empty(); }

    size_t active_conns_size() const { return m_conn_pool.active_size(); }

private:
    io_uring_wrapper<http_11_listener> m_io_uring_accept;
    io_uring_wrapper<http_11_connection> m_io_uring; // for reads/writes, need to change the template param to an http_request
    std::vector<port_info*> m_listening_fds;
    tc_object_pool<http_11_connection> m_conn_pool;
};

int open_dir(const char *path)
{ 
    int dir_fd = ::open(path, O_RDONLY | O_DIRECTORY);

    if (dir_fd < 0)
    {
        ERROR << "Error opening directory " << path << ": " << ::strerror(errno) << ENDL;
    }
    else
    {
        http_11_connection::set_dir_fd(dir_fd);
    }
    return dir_fd;
}

int main (int argc, char **argv)
{
    // TODO: research timing issues around queue depths. 
    // During initial testing, with client batch sizes equal to server's queue_depth, less logging in client meant a faster client which seemed
    // to get the server queues into a stuck state
    // restarting the client did not unbreak it.

    uint32_t queue_depth = 500;
    uint32_t max_conns = 200;
    int dir_fd = -1;
    std::string error_log_dir("logs");
    std::string error_log_name("server_error.log");
    std::string access_log_dir("logs");
    std::string access_log_name("server_access.log");


    for (int i = 1; i < argc; i++)
    {
        auto [key, val] = split(argv[i], '=');
        if (key == "--queue-depth"sv)
        {
            if (!aton(val, queue_depth))
            {
                ERROR << "Failed to convert '" << val << "' to a queue depth" << ENDL;
            } 
            break;
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
        else if (key == "--max-conns"sv)
        {
            max_conns = aton(val);
        }

    }

    set_error_log_name(error_log_dir.data(), error_log_name.data());
    s_access_log.set_log_name(access_log_dir, access_log_name);

    http_11_listener listener(queue_depth * 2, queue_depth * 2);

    for (int i = 1; i < argc; i++)
    {
        auto [key, val] = split(argv[i], '=');
        if (key == "--http-11-port"sv)
        {
            uint16_t port;
            if (!aton(val, port))
            {
                ERROR << "Failed to convert '" << val << "' to a port number" << ENDL;
            } 
            else
            {
                listener.listen(port, false);
            }
        }
        else if (key == "--file-dir"sv)
        {
            dir_fd = std::max(dir_fd, open_dir(val.data()));
        }
    }

    if (-1 == dir_fd)
    {
        // default to ./www dir
        dir_fd = std::max(dir_fd, open_dir("www"));
    }

    if (listener.empty())
        listener.listen(8081, false);

    while (true)
    {
        size_t conn_size = listener.active_conns_size();
        // aggregate total conn cnt from all types of listener, only http 1 for now
        uint32_t max_accepts = max_conns - conn_size;
        DEBUG(2) << "max_accepts: " << max_accepts << ", conn_size: " << conn_size << ENDL;
        uint32_t new_events = (max_accepts ? listener.accept(max_accepts) : 0);
        new_events += listener.process_events();
        if (!new_events)
            ::usleep(1000);
        process_error_log_events();
    }
}
