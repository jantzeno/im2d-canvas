#include "demo_sample_browser.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <optional>

namespace demo {
namespace {

struct SampleNode {
  std::filesystem::path path;
  std::string label;
  bool is_directory = false;
  std::vector<SampleNode> children;
};

std::string ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return value;
}

bool HasMatchingExtension(const std::filesystem::path &path,
                          const std::vector<std::string> &extensions) {
  const std::string extension = ToLower(path.extension().string());
  return std::find(extensions.begin(), extensions.end(), extension) !=
         extensions.end();
}

std::optional<SampleNode>
BuildSampleNode(const std::filesystem::path &path,
                const std::vector<std::string> &extensions,
                std::string *error_message) {
  std::error_code error;
  if (std::filesystem::is_directory(path, error)) {
    if (error) {
      *error_message = "Failed to inspect sample folder: " + path.string();
      return std::nullopt;
    }

    SampleNode directory_node;
    directory_node.path = path;
    directory_node.label = path.filename().string();
    directory_node.is_directory = true;

    for (const std::filesystem::directory_entry &entry :
         std::filesystem::directory_iterator(path, error)) {
      if (error) {
        *error_message = "Failed to enumerate sample folder: " + path.string();
        return std::nullopt;
      }

      std::optional<SampleNode> child =
          BuildSampleNode(entry.path(), extensions, error_message);
      if (!error_message->empty()) {
        return std::nullopt;
      }
      if (child.has_value()) {
        directory_node.children.push_back(std::move(*child));
      }
    }

    std::sort(directory_node.children.begin(), directory_node.children.end(),
              [](const SampleNode &left, const SampleNode &right) {
                if (left.is_directory != right.is_directory) {
                  return left.is_directory > right.is_directory;
                }
                return left.label < right.label;
              });

    if (directory_node.children.empty()) {
      return std::nullopt;
    }
    return directory_node;
  }

  if (std::filesystem::is_regular_file(path, error) &&
      HasMatchingExtension(path, extensions)) {
    SampleNode file_node;
    file_node.path = path;
    file_node.label = path.filename().string();
    return file_node;
  }

  if (error) {
    *error_message = "Failed to inspect sample path: " + path.string();
  }
  return std::nullopt;
}

SampleNode BuildSampleTree(const std::filesystem::path &directory,
                           const std::vector<std::string> &extensions,
                           std::string *error_message) {
  error_message->clear();
  SampleNode root;
  root.path = directory;
  root.label = directory.filename().string();
  root.is_directory = true;

  std::error_code error;
  if (!std::filesystem::exists(directory, error)) {
    *error_message = "Sample folder not found: " + directory.string();
    return root;
  }

  for (const std::filesystem::directory_entry &entry :
       std::filesystem::directory_iterator(directory, error)) {
    if (error) {
      *error_message =
          "Failed to enumerate sample folder: " + directory.string();
      return root;
    }

    std::optional<SampleNode> child =
        BuildSampleNode(entry.path(), extensions, error_message);
    if (!error_message->empty()) {
      root.children.clear();
      return root;
    }
    if (child.has_value()) {
      root.children.push_back(std::move(*child));
    }
  }

  std::sort(root.children.begin(), root.children.end(),
            [](const SampleNode &left, const SampleNode &right) {
              if (left.is_directory != right.is_directory) {
                return left.is_directory > right.is_directory;
              }
              return left.label < right.label;
            });
  return root;
}

bool DrawSampleNode(const SampleNode &node, SampleBrowserState &state,
                    std::filesystem::path *clicked_path) {
  bool clicked = false;
  if (node.is_directory) {
    const ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth;
    const bool open = ImGui::TreeNodeEx(node.path.string().c_str(), flags, "%s",
                                        node.label.c_str());
    if (open) {
      for (const SampleNode &child : node.children) {
        clicked = DrawSampleNode(child, state, clicked_path) || clicked;
      }
      ImGui::TreePop();
    }
    return clicked;
  }

  const std::string normalized_path = node.path.lexically_normal().string();
  const bool selected = state.selected_path == normalized_path;
  if (ImGui::Selectable(node.label.c_str(), selected)) {
    state.selected_path = normalized_path;
    *clicked_path = node.path;
    return true;
  }
  return false;
}

} // namespace

bool DrawSampleBrowserWindow(const char *window_title,
                             const std::filesystem::path &directory,
                             const std::vector<std::string> &extensions,
                             SampleBrowserState &state,
                             std::filesystem::path *clicked_path) {
  SampleNode root =
      BuildSampleTree(directory, extensions, &state.error_message);

  bool clicked = false;
  ImGui::Begin(window_title);
  ImGui::TextUnformatted(directory.string().c_str());
  ImGui::Separator();

  if (!state.error_message.empty()) {
    ImGui::TextWrapped("%s", state.error_message.c_str());
  } else if (root.children.empty()) {
    ImGui::TextUnformatted("No matching sample files found.");
  } else {
    for (const SampleNode &child : root.children) {
      clicked = DrawSampleNode(child, state, clicked_path) || clicked;
    }
  }

  ImGui::End();
  return clicked;
}

} // namespace demo