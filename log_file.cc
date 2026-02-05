
#include "log_file.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <string_view>

#include "io_uring_wrapper.h"
#include "scoped_lock.h"

log_file::log_file()
    : m_io_uring(new io_uring_wrapper<log_file>(10))
{
    //set_log_name(log_dir, file_name);
    m_input_buffer->clear();
}

log_file::~log_file()
{
    if (m_state == IDLE && m_input_buffer->size())
    {
        write_buffer(true);
    }

    while (m_state != IDLE)
        process_events();

    if (m_input_buffer->size())
    {
        write_buffer(true);
        while (m_state != IDLE)
            process_events();
    }

    delete m_io_uring;
}


void log_file::set_log_name(std::string_view log_dir, std::string_view file_name)
{
    if (file_name == "stdout")
        return;
    m_log_dir = log_dir;
    m_file_name = file_name;

    if (!m_log_dir.empty())
    {
        if (m_dir_fd != -1)
        {
            ::close(m_dir_fd);
            m_dir_fd = -1;
        }
        m_dir_fd = ::open(m_log_dir.data(), O_RDONLY | O_DIRECTORY);
        if (-1 == m_dir_fd)
        {
            std::cerr << "Failed to open log dir: " << m_log_dir << ", " << ::strerror(errno) << std::endl;
            exit(1);
        }
    }

    if (!m_file_name.empty() && m_fd == -1)
    {
        // we have not opened the log file yet, this should be at the start of the app, do a blocking open
        m_fd = ::openat(m_dir_fd, m_file_name.data(), O_WRONLY | O_CREAT | O_APPEND, 0666);
        if (-1 == m_fd)
        {
            std::cerr << "Failed to open log file: " << m_file_name << ", " << ::strerror(errno) << std::endl; 
            exit(1);
        }
    }
}


void log_file::log(std::string_view str, bool lock_it) // lock_it defaults to true for
{
    dsy::scoped_lock alock(&m_mutex, lock_it);
    m_input_buffer->append(str);
    if (m_input_buffer->back() != '\n')
        m_input_buffer->push_back('\n');

    if (m_fd > -1)
    {
        if (m_input_buffer->size() > 10 * 1024)
        {
            write_buffer(false);
        }
    }
    else
    {
        std::cerr << *m_input_buffer << std::endl;
        m_input_buffer->clear();
    }
}

void log_file::reopen()
{
    // set the flag for the write thread to take action on
    m_reopen = true;
    reopen_log();
}

uint32_t log_file::process_io_uring(int res)
{
    // do we need to lock here? hmm?

    switch (m_state) {
    case OPENING:
        if (m_new_fd == -1)
        {
            if (res < 0)
            {
                std::cerr << "Failed to open log file: " << m_file_name << ", " << ::strerror(-res) << std::endl;
                exit(1);
            }
            else
            {
                dsy::scoped_lock alock(&m_mutex, true);
                std::swap(m_new_fd, m_fd);
                m_io_uring->prep_close(m_new_fd, this); 
            }
        }
        else // m_new_fd is being closed after having been swapped with m_fd
        {
            if (res < 0)
            {
                std::cerr << "Failed to close log file: " << m_file_name << ", " << ::strerror(-res) << std::endl;
                exit(1);
            }
            m_new_fd = -1;
            m_state = IDLE;
        }
        break;
    case WRITING:
        if (res < 0)
        {
            std::cerr << "Failed to write log buffer: " << ::strerror(-res) << std::endl;
        }
        if (res < m_output_buffer->size())
        {
            std::cerr << "PARTIAL WRITE, wrote " << res << " bytes out of " << m_output_buffer->size() << std::endl;
        }
        // TODO: track bytes written so we can call write multiple times if we are getting partial writes
        m_output_buffer->clear();
        m_state = IDLE;
    default:
        break;
    }

    return m_state == IDLE ? 0 : 1;
}

void log_file::process_events()
{
    if (m_state == IDLE)
        return;
    __kernel_timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 1000;
    m_io_uring->process_events(1, &ts);
}

void log_file::write_buffer(bool lock_it)
{
    dsy::scoped_lock alock(&m_mutex, lock_it);
    if (m_state != IDLE || m_fd == -1)
        return;

    std::swap(m_input_buffer, m_output_buffer);
    m_input_buffer->clear(); // just in case

    m_io_uring->prep_write(m_fd, m_output_buffer->data(), m_output_buffer->size(), -1, this);
    m_io_uring->submit();
    m_state = WRITING;
}

void log_file::reopen_log()
{
    // 1. open new log file
    // 2. swap new and old fd
    // 3. close old fd

    if (m_state != IDLE)
        return;

    DEBUG(2) << "opening " << m_file_name << ENDL;
    m_io_uring->prep_open_at(m_dir_fd, m_file_name.data(), O_WRONLY | O_CREAT | O_APPEND, 0666, this);

    m_state = OPENING;
}
