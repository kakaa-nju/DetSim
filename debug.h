#ifndef __DEBUG_H
#define __DEBUG_H

#include <assert.h>
#include <iomanip>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

#define RESET "\033[0m"

#define FG_BLACK "\033[30m"
#define FG_RED "\033[31m"
#define FG_GREEN "\033[32m"
#define FG_YELLOW "\033[33m"
#define FG_BLUE "\033[34m"
#define FG_MAGENTA "\033[35m"
#define FG_CYAN "\033[36m"
#define FG_WHITE "\033[37m"

#define Assert(cond, ...)                                                      \
  do                                                                           \
  {                                                                            \
    if (!(cond))                                                               \
    {                                                                          \
      fprintf(stderr, "\33[1;31m");                                            \
      fprintf(stderr, __VA_ARGS__);                                            \
      fprintf(stderr, "\33[0m\n");                                             \
      assert(cond);                                                            \
    }                                                                          \
  } while (0)

#define panic(...) Assert(0, __VA_ARGS__)

#define TODO() panic("please implement me")

bool should_log(int level);

static void loglineprintf(int level, const char* file, int line,
                          const char* fmt, ...) noexcept
{
  va_list v;
  va_start(v, fmt);
  if (should_log(level))
  {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm* tm_now = localtime(&tv.tv_sec);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm_now);
    std::string levelstr;
    switch (level)
    {
      case 0:
        levelstr = FG_BLACK "trace" RESET;
        break;
      case 1:
        levelstr = FG_BLUE "debug" RESET;
        break;
      case 2:
        levelstr = FG_GREEN "info" RESET;
        break;
      case 3:
        levelstr = FG_YELLOW "warn" RESET;
        break;
      case 4:
        levelstr = FG_RED "error" RESET;
        break;
      case 5:
        levelstr = FG_MAGENTA "crit" RESET;
        break;
      default:
        assert(0);
    }
    fprintf(stdout, "[%s.%03ld][%s][%s:%d] ", buf, tv.tv_usec / 1000,
            levelstr.c_str(), file, line);
    vfprintf(stdout, fmt, v);
    fputc('\n', stdout);
  }
}

#define LOGGER_PRINTF(level, ...)                                              \
  loglineprintf(level, __FILE__, __LINE__, __VA_ARGS__)

// specific log implementation macros

#define LOG_CRIT(...) LOGGER_PRINTF(5, __VA_ARGS__)
#define LOG_ERROR(...) LOGGER_PRINTF(4, __VA_ARGS__)
#define LOG_WARN(...) LOGGER_PRINTF(3, __VA_ARGS__)
#define LOG_INFO(...) LOGGER_PRINTF(2, __VA_ARGS__)
#define LOG_DEBUG(...) LOGGER_PRINTF(1, __VA_ARGS__)
#define LOG_TRACE(...) LOGGER_PRINTF(0, __VA_ARGS__)

#endif /* __DEBUG_H */
