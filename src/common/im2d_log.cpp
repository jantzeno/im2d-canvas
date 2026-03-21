#include "im2d_log.h"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace im2d::log {
namespace {

std::shared_ptr<spdlog::logger> g_logger;

} // namespace

void InitializeLogger() {
  if (g_logger != nullptr) {
    return;
  }

  g_logger = spdlog::stdout_color_mt("im2d");
  g_logger->set_pattern("[%H:%M:%S] [%^%l%$] %v");
#ifdef NDEBUG
  g_logger->set_level(spdlog::level::info);
#else
  g_logger->set_level(spdlog::level::debug);
#endif
  spdlog::set_default_logger(g_logger);
}

void ShutdownLogger() {
  g_logger.reset();
  spdlog::shutdown();
}

std::shared_ptr<spdlog::logger> GetLogger() {
  if (g_logger == nullptr) {
    InitializeLogger();
  }
  return g_logger;
}

} // namespace im2d::log