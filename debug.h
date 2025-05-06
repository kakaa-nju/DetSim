#ifndef __DEBUG_H
#define __DEBUG_H

#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <spdlog/spdlog.h>
#include <spdlog/fmt/bundled/printf.h>

#ifdef DEBUG
extern FILE *LOG;
# define _Log(...) \
  do { \
    fprintf(LOG, __VA_ARGS__); \
  } while (0)
#else
# define LOG stdin
# define _Log(...) 
#endif /* DEBUG */

#define Log(format, ...) \
    _Log("\33[1;34m[%s,%d,%s] " format "\33[0m\n", \
        __FILE__, __LINE__, __func__, ## __VA_ARGS__)

#define Assert(cond, ...) \
  do { \
    if (!(cond)) { \
      fflush(LOG); \
      fprintf(stderr, "\33[1;31m"); \
      fprintf(stderr, __VA_ARGS__); \
      fprintf(stderr, "\33[0m\n"); \
      assert(cond); \
    } \
  } while (0)

#define panic(...) Assert(0, __VA_ARGS__)

#define TODO() panic("please implement me")

static inline void *Lmalloc(size_t size) {
#ifdef DEBUG
  Log("malloc: %d bytes", size);
#endif /* DEBUG */
  return malloc(size);
}

static inline void *Lrealloc(void *raw, size_t size) {
#ifdef DEBUG
  Log("realloc: %d bytes", size);
#endif /* DEBUG */
  return realloc(raw, size);
}
template <class loggerPtr, class... Args>
void loglineprintf(loggerPtr                 logger,
    spdlog::level::level_enum level,
    spdlog::source_loc        loc,
    const char* fmt,
    const Args&... args) noexcept
{
    if (logger && logger->should_log(level))
    {
        logger->log(loc, level, "{}", fmt::sprintf(fmt, args...));
    }
}

#define SPDLOG_LOGGER_PRINTF(logger, level, ...) \
    loglineprintf(logger, level, spdlog::source_loc{__FILE__, __LINE__, SPDLOG_FUNCTION}, __VA_ARGS__)

//specific log implementation macros

#define LOG_CRIT(...) SPDLOG_LOGGER_PRINTF(spdlog::default_logger(),spdlog::level::critical,__VA_ARGS__)
#define LOG_ERROR(...) SPDLOG_LOGGER_PRINTF(spdlog::default_logger(),spdlog::level::err,__VA_ARGS__)
#define LOG_WARN(...) SPDLOG_LOGGER_PRINTF(spdlog::default_logger(),spdlog::level::warn,__VA_ARGS__)
#define LOG_INFO(...) SPDLOG_LOGGER_PRINTF(spdlog::default_logger(),spdlog::level::info,__VA_ARGS__)
#define LOG_DEBUG(...) SPDLOG_LOGGER_PRINTF(spdlog::default_logger(),spdlog::level::debug,__VA_ARGS__)
#define LOG_TRACE(...) SPDLOG_LOGGER_PRINTF(spdlog::default_logger(),spdlog::level::trace,__VA_ARGS__)

#endif /* __DEBUG_H */
