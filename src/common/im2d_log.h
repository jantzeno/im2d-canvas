#pragma once

#include <memory>

#include <spdlog/spdlog.h>

namespace im2d::log {

void InitializeLogger();
void ShutdownLogger();
std::shared_ptr<spdlog::logger> GetLogger();

} // namespace im2d::log