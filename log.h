#pragma once

inline static int s_debug_level = 0;

#define ERROR std::cerr << "ERROR " << __func__ << ' ' << __FILE__ << ':' << __LINE__ << ' '
#define TRACE std::cerr << "TRACE " << __func__ << ' ' << __FILE__ << ':' << __LINE__ << ' '
#define WARN  std::cerr << "WARN " << __func__ << ' ' << __FILE__ << ':' << __LINE__ << ' '
#define ENDL std::endl; }
#define DEBUG(level) if (s_debug_level > level)  { std::cerr << "DEBUG" << level << ' ' << __func__ << ' ' << __FILE__ << ':' << __LINE__ << ' '
