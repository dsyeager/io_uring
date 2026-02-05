#include "log.h"
#include "log_file.h"

log_file s_error_log; // used by log.h

void submit_log_entry(std::string &buff)
{
    s_error_log.log(buff);
}

void set_error_log_name(const char *dir_name, const char *file_name)
{
    s_error_log.set_log_name(dir_name, file_name);
}

void process_error_log_events()
{
    s_error_log.process_events();
}
