#include "im2d_log.h"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace im2d::log {
namespace {

constexpr const char *kLoggerName = "canvas";

std::shared_ptr<spdlog::logger> g_logger;

} // namespace

void InitializeLogger() {
  if (g_logger != nullptr) {
    return;
  }

  g_logger = spdlog::get(kLoggerName);
  if (g_logger == nullptr) {
    g_logger = spdlog::stdout_color_mt(kLoggerName);
  }

  g_logger->set_pattern("[%H:%M:%S] [%^%l%$] [" + std::string(kLoggerName) +
                        "] %v");
#ifdef NDEBUG
  g_logger->set_level(spdlog::level::info);
#else
  g_logger->set_level(spdlog::level::debug);
#endif
  spdlog::set_default_logger(g_logger);
}

void ShutdownLogger() {
  if (g_logger == nullptr) {
    spdlog::drop(kLoggerName);
    return;
  }

  g_logger.reset();
  spdlog::drop(kLoggerName);
}

std::shared_ptr<spdlog::logger> GetLogger() {
  if (g_logger == nullptr) {
    InitializeLogger();
  }
  return g_logger;
}

} // namespace im2d::log