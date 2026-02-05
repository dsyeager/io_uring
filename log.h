#pragma once

#include <string>

#include "get_milliseconds.h"
#include "string_helpers.h"

inline static int s_debug_level = 0;
inline thread_local std::string s_log_buffer;

void submit_log_entry(std::string &buff);
void set_error_log_name(const char *dir_name, const char *file_name);
void process_error_log_events();

#define BEGL s_log_buffer.clear(); s_log_buffer << get_milliseconds()
#define ERROR { BEGL << " ERROR " << __func__ << ' ' << __FILE__ << ':' << __LINE__ << ' '
#define TRACE { BEGL << " TRACE " << __func__ << ' ' << __FILE__ << ':' << __LINE__ << ' '
#define WARN  { BEGL << " WARN " << __func__ << ' ' << __FILE__ << ':' << __LINE__ << ' '
#define DEBUG(level) if (s_debug_level >= level)  { BEGL << " DEBUG" << level << ' ' << __func__ << ' ' << __FILE__ << ':' << __LINE__ << ' '
#define ENDL '\n'; submit_log_entry(s_log_buffer); }
