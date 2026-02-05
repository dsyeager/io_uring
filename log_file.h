#pragma once

#include <algorithm>
#include <mutex>
#include <string>
#include <string_view>

#include "io_uring_wrapper.h"

//class log_file;
//template<typename T> class io_uring_wrapper;
//template<> class io_uring_wrapper<log_file>; // try forward declaration

/**
  General purpose logger
  client threads submit log entries
  writer thread writes the log entries in batches
     - probably should allow the destination to be a POST url but will start with just a file name


  some thoughts floating up about why the opens/writes/closes in this class can't also use io_uring
     - mixing it with the transaction handling would muck that up more than I like but you could if the event loop was brought out into the app code instead of inside the io_uring_wrapper, then the events could have an enum to say what type of logic are they for (TXN, LOG, ETC) so the data* could be interpretted correctly. Hmm.
     - could give this class it's own io_uring wrapper then the owner of the log call process periodically.
  */
class log_file
{
public:
    enum state { OPENING, IDLE, WRITING, CLOSING };
    const char* to_str(state val)
    {
        switch (val) {
        case OPENING: return "OPENING";
        case IDLE: return "IDLE";
        case WRITING: return "WRITING";
        case CLOSING: return "CLOSING";
        };
    } 
public:
    log_file();

    ~log_file();

    void set_log_name(std::string_view log_dir, std::string_view file_name);

    void log(std::string_view str, bool lock_it = true);

    void reopen();

    uint32_t process_io_uring(int res);

    void process_events();

private:

    void write_buffer(bool lock_it);
    void reopen_log();

private:
    state m_state = IDLE;
    std::string m_log_dir = ".";
    std::string m_file_name = "out.log";
    int m_dir_fd = -1;
    int m_fd = -1;
    int m_new_fd = -1;
    bool m_reopen = true;
   
    io_uring_wrapper<log_file> *m_io_uring = nullptr;;

    std::string m_buffers[2];
    std::string *m_input_buffer = &m_buffers[0];
    std::string *m_output_buffer = &m_buffers[1];
    std::mutex m_mutex;
};
