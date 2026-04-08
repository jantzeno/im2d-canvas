#pragma once

#include "im2d_canvas_settings.h"

#include <imgui.h>

#include <cstdint>
#include <string>

namespace im2d {

enum class GuideOrientation {
  Vertical,
  Horizontal,
};

enum WorkingAreaFlags : uint32_t {
  WorkingAreaFlagNone = 0,
  WorkingAreaFlagMovable = 1u << 0,
  WorkingAreaFlagResizable = 1u << 1,
};

constexpr uint32_t kDefaultWorkingAreaFlags =
    static_cast<uint32_t>(WorkingAreaFlagMovable) |
    static_cast<uint32_t>(WorkingAreaFlagResizable);

constexpr bool HasWorkingAreaFlag(uint32_t flags, WorkingAreaFlags flag) {
  return (flags & static_cast<uint32_t>(flag)) != 0;
}

struct Guide {
  int id = 0;
  GuideOrientation orientation = GuideOrientation::Vertical;
  float position = 0.0f;
  bool locked = false;
};

struct WorkingArea {
  int id = 0;
  std::string name;
  ImVec2 origin = ImVec2(0.0f, 0.0f);
  ImVec2 size = ImVec2(0.0f, 0.0f);
  bool visible = true;
  ImVec4 border_color = ImVec4(0.58f, 0.74f, 0.58f, 1.0f);
  ImVec4 selected_border_color = ImVec4(0.97f, 0.82f, 0.36f, 1.0f);
  float outline_thickness = Outlines().outline_thickness;
  float selected_outline_thickness = Outlines().selected_outline_thickness;
  uint32_t flags = kDefaultWorkingAreaFlags;
};

struct ExportArea {
  int id = 0;
  int source_working_area_id = 0;
  ImVec2 origin = ImVec2(0.0f, 0.0f);
  ImVec2 size = ImVec2(0.0f, 0.0f);
  bool visible = true;
  bool hide_fill = false;
  ImVec4 outline_color = ImVec4(0.43f, 0.77f, 0.92f, 0.65f);
  ImVec4 fill_color = ImVec4(0.43f, 0.77f, 0.92f, 0.18f);
};

struct ExclusionArea {
  int id = 0;
  int source_working_area_id = 0;
  ImVec2 origin = ImVec2(0.0f, 0.0f);
  ImVec2 size = ImVec2(0.0f, 0.0f);
  bool visible = true;
  bool selected = false;
  bool hide_fill = false;
};

struct Layer {
  int id = 0;
  std::string name;
  bool visible = true;
  bool locked = false;
};

struct WorkingAreaCreateInfo {
  std::string name;
  ImVec2 size_pixels = ImVec2(0.0f, 0.0f);
  float outline_thickness = Outlines().outline_thickness;
  float selected_outline_thickness = Outlines().selected_outline_thickness;
  uint32_t flags = kDefaultWorkingAreaFlags;
};

} // namespace im2d
