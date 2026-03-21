#include "demo_sample_browser.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>

namespace demo {
namespace {

std::string ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return value;
}

std::vector<std::filesystem::path>
ListSampleFiles(const std::filesystem::path &directory,
                const std::vector<std::string> &extensions,
                std::string *error_message) {
  std::vector<std::filesystem::path> files;
  error_message->clear();

  std::error_code error;
  if (!std::filesystem::exists(directory, error)) {
    *error_message = "Sample folder not found: " + directory.string();
    return files;
  }

  for (const std::filesystem::directory_entry &entry :
       std::filesystem::directory_iterator(directory, error)) {
    if (error) {
      *error_message =
          "Failed to enumerate sample folder: " + directory.string();
      files.clear();
      return files;
    }

    if (!entry.is_regular_file()) {
      continue;
    }

    const std::string extension = ToLower(entry.path().extension().string());
    if (std::find(extensions.begin(), extensions.end(), extension) !=
        extensions.end()) {
      files.push_back(entry.path());
    }
  }

  std::sort(files.begin(), files.end(),
            [](const std::filesystem::path &left,
               const std::filesystem::path &right) {
              return left.filename().string() < right.filename().string();
            });
  return files;
}

} // namespace

bool DrawSampleBrowserWindow(const char *window_title,
                             const std::filesystem::path &directory,
                             const std::vector<std::string> &extensions,
                             SampleBrowserState &state,
                             std::filesystem::path *clicked_path) {
  std::vector<std::filesystem::path> files =
      ListSampleFiles(directory, extensions, &state.error_message);
  if (state.selected_index >= static_cast<int>(files.size())) {
    state.selected_index = files.empty() ? -1 : 0;
  }

  bool clicked = false;
  ImGui::Begin(window_title);
  ImGui::TextUnformatted(directory.string().c_str());
  ImGui::Separator();

  if (!state.error_message.empty()) {
    ImGui::TextWrapped("%s", state.error_message.c_str());
  } else if (files.empty()) {
    ImGui::TextUnformatted("No matching sample files found.");
  } else {
    for (int index = 0; index < static_cast<int>(files.size()); ++index) {
      const bool selected = state.selected_index == index;
      if (ImGui::Selectable(files[index].filename().string().c_str(),
                            selected)) {
        state.selected_index = index;
        *clicked_path = files[index];
        clicked = true;
      }
    }
  }

  ImGui::End();
  return clicked;
}

} // namespace demo