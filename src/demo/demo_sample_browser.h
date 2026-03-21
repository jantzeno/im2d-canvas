#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace demo {

struct SampleBrowserState {
  int selected_index = -1;
  std::string error_message;
};

bool DrawSampleBrowserWindow(const char *window_title,
                             const std::filesystem::path &directory,
                             const std::vector<std::string> &extensions,
                             SampleBrowserState &state,
                             std::filesystem::path *clicked_path);

} // namespace demo