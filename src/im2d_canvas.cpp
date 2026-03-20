#include "im2d_canvas.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

#include <imgui_internal.h>

namespace im2d {

namespace {

struct TransientCanvasState {
  bool creating_guide = false;
  GuideOrientation pending_orientation = GuideOrientation::Vertical;
  float pending_position = 0.0f;
  int dragging_guide_id = 0;
  int context_guide_id = 0;
};

TransientCanvasState &GetTransientCanvasState() {
  static TransientCanvasState state;
  return state;
}

float ClampZoom(float zoom) { return std::clamp(zoom, 0.1f, 8.0f); }

ImVec2 WorldToScreen(const CanvasState &state, const ImVec2 &canvas_min,
                     const ImVec2 &point) {
  return ImVec2(canvas_min.x + state.view.pan.x + point.x * state.view.zoom,
                canvas_min.y + state.view.pan.y + point.y * state.view.zoom);
}

ImVec2 ScreenToWorld(const CanvasState &state, const ImVec2 &canvas_min,
                     const ImVec2 &point) {
  return ImVec2((point.x - canvas_min.x - state.view.pan.x) / state.view.zoom,
                (point.y - canvas_min.y - state.view.pan.y) / state.view.zoom);
}

float NiceStep(float raw_value) {
  if (raw_value <= 0.0f) {
    return 1.0f;
  }
  const float exponent = std::floor(std::log10(raw_value));
  const float base = std::pow(10.0f, exponent);
  const float fraction = raw_value / base;

  float nice_fraction = 1.0f;
  if (fraction <= 1.0f) {
    nice_fraction = 1.0f;
  } else if (fraction <= 2.0f) {
    nice_fraction = 2.0f;
  } else if (fraction <= 5.0f) {
    nice_fraction = 5.0f;
  } else {
    nice_fraction = 10.0f;
  }

  return nice_fraction * base;
}

Guide *FindGuide(CanvasState &state, int guide_id) {
  auto it = std::find_if(
      state.guides.begin(), state.guides.end(),
      [guide_id](const Guide &guide) { return guide.id == guide_id; });
  return it == state.guides.end() ? nullptr : &(*it);
}

int FindHoveredGuide(const CanvasState &state, const ImVec2 &canvas_min,
                     const ImRect &canvas_rect, const ImVec2 &mouse_pos) {
  if (!canvas_rect.Contains(mouse_pos)) {
    return 0;
  }

  constexpr float kGuideHitRadius = 5.0f;
  int hovered_id = 0;
  float closest_distance = kGuideHitRadius;

  for (const Guide &guide : state.guides) {
    float distance = 0.0f;
    if (guide.orientation == GuideOrientation::Vertical) {
      const float guide_screen_x =
          WorldToScreen(state, canvas_min, ImVec2(guide.position, 0.0f)).x;
      distance = std::fabs(mouse_pos.x - guide_screen_x);
    } else {
      const float guide_screen_y =
          WorldToScreen(state, canvas_min, ImVec2(0.0f, guide.position)).y;
      distance = std::fabs(mouse_pos.y - guide_screen_y);
    }

    if (distance <= closest_distance) {
      closest_distance = distance;
      hovered_id = guide.id;
    }
  }

  return hovered_id;
}

void RemoveGuide(CanvasState &state, int guide_id) {
  std::erase_if(state.guides, [guide_id](const Guide &guide) {
    return guide.id == guide_id;
  });
}

void DrawGrid(ImDrawList *draw_list, const CanvasState &state,
              const ImRect &canvas_rect) {
  if (!state.grid.visible || state.grid.spacing <= 0.0f ||
      state.grid.subdivisions <= 0) {
    return;
  }

  const float major_spacing_world =
      UnitsToPixels(state.grid.spacing, state.grid.unit, state.pixels_per_mm);
  if (major_spacing_world <= 0.0f) {
    return;
  }

  const float major_spacing_screen = major_spacing_world * state.view.zoom;
  const float minor_spacing_screen =
      major_spacing_screen / static_cast<float>(state.grid.subdivisions);

  draw_list->PushClipRect(canvas_rect.Min, canvas_rect.Max, true);

  if (minor_spacing_screen >= 8.0f) {
    const float minor_spacing_world =
        major_spacing_world / static_cast<float>(state.grid.subdivisions);
    const ImU32 minor_color =
        ImGui::ColorConvertFloat4ToU32(state.theme.grid_minor);
    const ImVec2 world_min =
        ScreenToWorld(state, canvas_rect.Min, canvas_rect.Min);
    const ImVec2 world_max =
        ScreenToWorld(state, canvas_rect.Min, canvas_rect.Max);
    const float start_x =
        std::floor(world_min.x / minor_spacing_world) * minor_spacing_world;
    const float end_x =
        std::ceil(world_max.x / minor_spacing_world) * minor_spacing_world;
    const float start_y =
        std::floor(world_min.y / minor_spacing_world) * minor_spacing_world;
    const float end_y =
        std::ceil(world_max.y / minor_spacing_world) * minor_spacing_world;

    for (float x = start_x; x <= end_x; x += minor_spacing_world) {
      const float screen_x =
          WorldToScreen(state, canvas_rect.Min, ImVec2(x, 0.0f)).x;
      draw_list->AddLine(ImVec2(screen_x, canvas_rect.Min.y),
                         ImVec2(screen_x, canvas_rect.Max.y), minor_color,
                         1.0f);
    }
    for (float y = start_y; y <= end_y; y += minor_spacing_world) {
      const float screen_y =
          WorldToScreen(state, canvas_rect.Min, ImVec2(0.0f, y)).y;
      draw_list->AddLine(ImVec2(canvas_rect.Min.x, screen_y),
                         ImVec2(canvas_rect.Max.x, screen_y), minor_color,
                         1.0f);
    }
  }

  const ImU32 major_color =
      ImGui::ColorConvertFloat4ToU32(state.theme.grid_major);
  const ImVec2 world_min =
      ScreenToWorld(state, canvas_rect.Min, canvas_rect.Min);
  const ImVec2 world_max =
      ScreenToWorld(state, canvas_rect.Min, canvas_rect.Max);
  const float start_x =
      std::floor(world_min.x / major_spacing_world) * major_spacing_world;
  const float end_x =
      std::ceil(world_max.x / major_spacing_world) * major_spacing_world;
  const float start_y =
      std::floor(world_min.y / major_spacing_world) * major_spacing_world;
  const float end_y =
      std::ceil(world_max.y / major_spacing_world) * major_spacing_world;

  for (float x = start_x; x <= end_x; x += major_spacing_world) {
    const float screen_x =
        WorldToScreen(state, canvas_rect.Min, ImVec2(x, 0.0f)).x;
    draw_list->AddLine(ImVec2(screen_x, canvas_rect.Min.y),
                       ImVec2(screen_x, canvas_rect.Max.y), major_color, 1.0f);
  }
  for (float y = start_y; y <= end_y; y += major_spacing_world) {
    const float screen_y =
        WorldToScreen(state, canvas_rect.Min, ImVec2(0.0f, y)).y;
    draw_list->AddLine(ImVec2(canvas_rect.Min.x, screen_y),
                       ImVec2(canvas_rect.Max.x, screen_y), major_color, 1.0f);
  }

  draw_list->PopClipRect();
}

void DrawWorkingAreas(ImDrawList *draw_list, const CanvasState &state,
                      const ImRect &canvas_rect) {
  draw_list->PushClipRect(canvas_rect.Min, canvas_rect.Max, true);

  const ImU32 fill_color =
      ImGui::ColorConvertFloat4ToU32(state.theme.working_area_fill);
  const ImU32 border_color =
      ImGui::ColorConvertFloat4ToU32(state.theme.working_area_border);
  const ImU32 export_outline =
      ImGui::ColorConvertFloat4ToU32(state.theme.export_area_outline);

  for (const WorkingArea &area : state.working_areas) {
    if (!area.visible) {
      continue;
    }

    const ImVec2 min = WorldToScreen(state, canvas_rect.Min, area.origin);
    const ImVec2 max = WorldToScreen(
        state, canvas_rect.Min,
        ImVec2(area.origin.x + area.size.x, area.origin.y + area.size.y));
    draw_list->AddRectFilled(min, max, fill_color, 4.0f);
    draw_list->AddRect(min, max, border_color, 4.0f, 0, 2.0f);
    draw_list->AddText(ImVec2(min.x + 8.0f, min.y + 8.0f), border_color,
                       area.name.c_str());
  }

  for (const ExportArea &area : state.export_areas) {
    if (!area.visible) {
      continue;
    }
    const ImVec2 min = WorldToScreen(state, canvas_rect.Min, area.origin);
    const ImVec2 max = WorldToScreen(
        state, canvas_rect.Min,
        ImVec2(area.origin.x + area.size.x, area.origin.y + area.size.y));
    draw_list->AddRect(min, max, export_outline, 0.0f, 0, 1.0f);
  }

  draw_list->PopClipRect();
}

void DrawGuides(ImDrawList *draw_list, const CanvasState &state,
                const ImRect &canvas_rect, int hovered_guide_id,
                const TransientCanvasState &transient_state) {
  draw_list->PushClipRect(canvas_rect.Min, canvas_rect.Max, true);

  for (const Guide &guide : state.guides) {
    ImVec4 color = guide.locked ? state.theme.guide_locked : state.theme.guide;
    if (guide.id == hovered_guide_id ||
        guide.id == transient_state.dragging_guide_id) {
      color = state.theme.guide_hovered;
    }

    const ImU32 packed = ImGui::ColorConvertFloat4ToU32(color);
    if (guide.orientation == GuideOrientation::Vertical) {
      const float x =
          WorldToScreen(state, canvas_rect.Min, ImVec2(guide.position, 0.0f)).x;
      draw_list->AddLine(ImVec2(x, canvas_rect.Min.y),
                         ImVec2(x, canvas_rect.Max.y), packed,
                         guide.locked ? 1.0f : 2.0f);
    } else {
      const float y =
          WorldToScreen(state, canvas_rect.Min, ImVec2(0.0f, guide.position)).y;
      draw_list->AddLine(ImVec2(canvas_rect.Min.x, y),
                         ImVec2(canvas_rect.Max.x, y), packed,
                         guide.locked ? 1.0f : 2.0f);
    }
  }

  if (transient_state.creating_guide) {
    const ImU32 preview_color =
        ImGui::ColorConvertFloat4ToU32(state.theme.guide_hovered);
    if (transient_state.pending_orientation == GuideOrientation::Vertical) {
      const float x =
          WorldToScreen(state, canvas_rect.Min,
                        ImVec2(transient_state.pending_position, 0.0f))
              .x;
      draw_list->AddLine(ImVec2(x, canvas_rect.Min.y),
                         ImVec2(x, canvas_rect.Max.y), preview_color, 2.0f);
    } else {
      const float y =
          WorldToScreen(state, canvas_rect.Min,
                        ImVec2(0.0f, transient_state.pending_position))
              .y;
      draw_list->AddLine(ImVec2(canvas_rect.Min.x, y),
                         ImVec2(canvas_rect.Max.x, y), preview_color, 2.0f);
    }
  }

  draw_list->PopClipRect();
}

void DrawRulerAxis(ImDrawList *draw_list, const CanvasState &state,
                   const ImRect &rect, bool horizontal) {
  const ImU32 background =
      ImGui::ColorConvertFloat4ToU32(state.theme.ruler_background);
  const ImU32 tick_color =
      ImGui::ColorConvertFloat4ToU32(state.theme.ruler_ticks);
  const ImU32 text_color =
      ImGui::ColorConvertFloat4ToU32(state.theme.ruler_text);
  draw_list->AddRectFilled(rect.Min, rect.Max, background);

  const float pixels_per_unit =
      UnitsToPixels(1.0f, state.ruler_unit, state.pixels_per_mm) *
      state.view.zoom;
  if (pixels_per_unit <= 0.0f) {
    return;
  }

  const float major_step_units = NiceStep(80.0f / pixels_per_unit);
  const float minor_step_units = major_step_units / 5.0f;
  const float major_step_world =
      UnitsToPixels(major_step_units, state.ruler_unit, state.pixels_per_mm);
  const float minor_step_world =
      UnitsToPixels(minor_step_units, state.ruler_unit, state.pixels_per_mm);

  if (minor_step_world <= 0.0f || major_step_world <= 0.0f) {
    return;
  }

  const float visible_min_world =
      horizontal ? ScreenToWorld(state, ImVec2(rect.Min.x, rect.Max.y),
                                 ImVec2(rect.Min.x, rect.Max.y))
                       .x
                 : ScreenToWorld(state, ImVec2(rect.Max.x, rect.Min.y),
                                 ImVec2(rect.Max.x, rect.Min.y))
                       .y;
  const float visible_max_world =
      horizontal ? ScreenToWorld(state, ImVec2(rect.Min.x, rect.Max.y),
                                 ImVec2(rect.Max.x, rect.Max.y))
                       .x
                 : ScreenToWorld(state, ImVec2(rect.Max.x, rect.Min.y),
                                 ImVec2(rect.Max.x, rect.Max.y))
                       .y;

  const float min_units =
      PixelsToUnits(visible_min_world, state.ruler_unit, state.pixels_per_mm);
  const float max_units =
      PixelsToUnits(visible_max_world, state.ruler_unit, state.pixels_per_mm);
  const float start_units =
      std::floor(std::min(min_units, max_units) / minor_step_units) *
      minor_step_units;
  const float end_units =
      std::ceil(std::max(min_units, max_units) / minor_step_units) *
      minor_step_units;

  for (float tick_units = start_units; tick_units <= end_units;
       tick_units += minor_step_units) {
    const float tick_world =
        UnitsToPixels(tick_units, state.ruler_unit, state.pixels_per_mm);
    const bool major =
        std::fabs(std::fmod(std::fabs(tick_units), major_step_units)) <
            0.0001f ||
        std::fabs(std::fmod(std::fabs(tick_units), major_step_units) -
                  major_step_units) < 0.0001f;
    const float tick_length =
        major
            ? (horizontal ? rect.GetHeight() - 4.0f : rect.GetWidth() - 4.0f)
            : (horizontal ? rect.GetHeight() * 0.45f : rect.GetWidth() * 0.45f);

    if (horizontal) {
      const float x = WorldToScreen(state, ImVec2(rect.Min.x, rect.Max.y),
                                    ImVec2(tick_world, 0.0f))
                          .x;
      draw_list->AddLine(ImVec2(x, rect.Max.y),
                         ImVec2(x, rect.Max.y - tick_length), tick_color, 1.0f);
      if (major) {
        char label[32];
        std::snprintf(label, sizeof(label), "%.3g", tick_units);
        draw_list->AddText(ImVec2(x + 4.0f, rect.Min.y + 4.0f), text_color,
                           label);
      }
    } else {
      const float y = WorldToScreen(state, ImVec2(rect.Max.x, rect.Min.y),
                                    ImVec2(0.0f, tick_world))
                          .y;
      draw_list->AddLine(ImVec2(rect.Max.x, y),
                         ImVec2(rect.Max.x - tick_length, y), tick_color, 1.0f);
      if (major) {
        char label[32];
        std::snprintf(label, sizeof(label), "%.3g", tick_units);
        draw_list->AddText(ImVec2(rect.Min.x + 4.0f, y + 2.0f), text_color,
                           label);
      }
    }
  }
}

} // namespace

const char *MeasurementUnitLabel(MeasurementUnit unit) {
  switch (unit) {
  case MeasurementUnit::Pixels:
    return "px";
  case MeasurementUnit::Millimeters:
    return "mm";
  case MeasurementUnit::Inches:
    return "in";
  }
  return "?";
}

float UnitsToPixels(float value, MeasurementUnit unit, float pixels_per_mm) {
  switch (unit) {
  case MeasurementUnit::Pixels:
    return value;
  case MeasurementUnit::Millimeters:
    return value * pixels_per_mm;
  case MeasurementUnit::Inches:
    return value * pixels_per_mm * 25.4f;
  }
  return value;
}

float PixelsToUnits(float value, MeasurementUnit unit, float pixels_per_mm) {
  switch (unit) {
  case MeasurementUnit::Pixels:
    return value;
  case MeasurementUnit::Millimeters:
    return value / pixels_per_mm;
  case MeasurementUnit::Inches:
    return value / (pixels_per_mm * 25.4f);
  }
  return value;
}

int AddWorkingArea(CanvasState &state, const std::string &name,
                   ImVec2 size_pixels) {
  WorkingArea area;
  area.id = state.next_working_area_id++;
  area.name = name;
  area.size =
      ImVec2(std::max(size_pixels.x, 1.0f), std::max(size_pixels.y, 1.0f));
  const float stagger = 32.0f * static_cast<float>(state.working_areas.size());
  area.origin = ImVec2(stagger, stagger);
  state.working_areas.push_back(area);

  ExportArea export_area;
  export_area.id = state.next_export_area_id++;
  export_area.source_working_area_id = area.id;
  export_area.origin = area.origin;
  export_area.size = area.size;
  state.export_areas.push_back(export_area);
  return area.id;
}

void InitializeDefaultDocument(CanvasState &state) {
  if (state.layers.empty()) {
    state.layers.push_back(Layer{state.next_layer_id++, "Root", true, false});
  }

  if (state.working_areas.empty()) {
    AddWorkingArea(state, "Working Area 1",
                   ImVec2(UnitsToPixels(210.0f, MeasurementUnit::Millimeters,
                                        state.pixels_per_mm),
                          UnitsToPixels(297.0f, MeasurementUnit::Millimeters,
                                        state.pixels_per_mm)));
  }
}

bool DrawCanvas(CanvasState &state, const CanvasWidgetOptions &options) {
  InitializeDefaultDocument(state);
  TransientCanvasState &transient_state = GetTransientCanvasState();
  ImGuiIO &io = ImGui::GetIO();

  const ImVec2 canvas_size = ImGui::GetContentRegionAvail();
  if (canvas_size.x <= 2.0f || canvas_size.y <= 2.0f) {
    return false;
  }

  ImGui::PushID("im2d_canvas");
  ImGui::InvisibleButton("##surface", canvas_size,
                         ImGuiButtonFlags_MouseButtonLeft |
                             ImGuiButtonFlags_MouseButtonRight |
                             ImGuiButtonFlags_MouseButtonMiddle);

  const ImRect total_rect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
  const ImRect top_ruler_rect(
      ImVec2(total_rect.Min.x + options.ruler_thickness, total_rect.Min.y),
      ImVec2(total_rect.Max.x, total_rect.Min.y + options.ruler_thickness));
  const ImRect left_ruler_rect(
      ImVec2(total_rect.Min.x, total_rect.Min.y + options.ruler_thickness),
      ImVec2(total_rect.Min.x + options.ruler_thickness, total_rect.Max.y));
  const ImRect corner_rect(total_rect.Min,
                           ImVec2(total_rect.Min.x + options.ruler_thickness,
                                  total_rect.Min.y + options.ruler_thickness));
  const ImRect canvas_rect(ImVec2(total_rect.Min.x + options.ruler_thickness,
                                  total_rect.Min.y + options.ruler_thickness),
                           total_rect.Max);

  ImDrawList *draw_list = ImGui::GetWindowDrawList();
  draw_list->AddRectFilled(
      total_rect.Min, total_rect.Max,
      ImGui::ColorConvertFloat4ToU32(state.theme.ruler_background));
  draw_list->AddRectFilled(
      canvas_rect.Min, canvas_rect.Max,
      ImGui::ColorConvertFloat4ToU32(state.theme.canvas_background));
  draw_list->AddRectFilled(
      corner_rect.Min, corner_rect.Max,
      ImGui::ColorConvertFloat4ToU32(state.theme.ruler_background));

  DrawWorkingAreas(draw_list, state, canvas_rect);
  DrawGrid(draw_list, state, canvas_rect);

  const bool canvas_hovered = canvas_rect.Contains(io.MousePos);
  const bool top_ruler_hovered = top_ruler_rect.Contains(io.MousePos);
  const bool left_ruler_hovered = left_ruler_rect.Contains(io.MousePos);

  int hovered_guide_id = 0;
  if (canvas_hovered && !transient_state.creating_guide) {
    hovered_guide_id =
        FindHoveredGuide(state, canvas_rect.Min, canvas_rect, io.MousePos);
  }

  if (canvas_hovered && io.MouseWheel != 0.0f) {
    const ImVec2 focus_world =
        ScreenToWorld(state, canvas_rect.Min, io.MousePos);
    state.view.zoom =
        ClampZoom(state.view.zoom * std::pow(1.15f, io.MouseWheel));
    state.view.pan = ImVec2(
        io.MousePos.x - canvas_rect.Min.x - focus_world.x * state.view.zoom,
        io.MousePos.y - canvas_rect.Min.y - focus_world.y * state.view.zoom);
  }

  if (canvas_hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f)) {
    state.view.pan.x += io.MouseDelta.x;
    state.view.pan.y += io.MouseDelta.y;
  }

  if (!transient_state.creating_guide &&
      (top_ruler_hovered || left_ruler_hovered) &&
      ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    transient_state.creating_guide = true;
    transient_state.pending_orientation = left_ruler_hovered
                                              ? GuideOrientation::Vertical
                                              : GuideOrientation::Horizontal;
  }

  if (transient_state.creating_guide) {
    const ImVec2 world = ScreenToWorld(state, canvas_rect.Min, io.MousePos);
    transient_state.pending_position =
        transient_state.pending_orientation == GuideOrientation::Vertical
            ? world.x
            : world.y;

    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      state.guides.push_back(Guide{
          state.next_guide_id++,
          transient_state.pending_orientation,
          transient_state.pending_position,
          false,
      });
      transient_state.creating_guide = false;
    }
  }

  if (!transient_state.creating_guide && canvas_hovered &&
      hovered_guide_id != 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    if (Guide *guide = FindGuide(state, hovered_guide_id);
        guide != nullptr && !guide->locked) {
      transient_state.dragging_guide_id = hovered_guide_id;
    }
  }

  if (transient_state.dragging_guide_id != 0) {
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      if (Guide *guide = FindGuide(state, transient_state.dragging_guide_id);
          guide != nullptr) {
        const ImVec2 world = ScreenToWorld(state, canvas_rect.Min, io.MousePos);
        guide->position = guide->orientation == GuideOrientation::Vertical
                              ? world.x
                              : world.y;
      }
    } else {
      transient_state.dragging_guide_id = 0;
    }
  }

  if (hovered_guide_id != 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    transient_state.context_guide_id = hovered_guide_id;
    ImGui::OpenPopup("guide_context_menu");
  } else if ((top_ruler_hovered || left_ruler_hovered) &&
             ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    ImGui::OpenPopup("ruler_context_menu");
  } else if (canvas_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    ImGui::OpenPopup("canvas_context_menu");
  }

  DrawGuides(draw_list, state, canvas_rect, hovered_guide_id, transient_state);
  DrawRulerAxis(draw_list, state, top_ruler_rect, true);
  DrawRulerAxis(draw_list, state, left_ruler_rect, false);
  draw_list->AddLine(ImVec2(canvas_rect.Min.x, total_rect.Min.y),
                     ImVec2(canvas_rect.Min.x, total_rect.Max.y),
                     ImGui::ColorConvertFloat4ToU32(state.theme.ruler_ticks));
  draw_list->AddLine(ImVec2(total_rect.Min.x, canvas_rect.Min.y),
                     ImVec2(total_rect.Max.x, canvas_rect.Min.y),
                     ImGui::ColorConvertFloat4ToU32(state.theme.ruler_ticks));

  draw_list->AddText(ImVec2(corner_rect.Min.x + 6.0f, corner_rect.Min.y + 7.0f),
                     ImGui::ColorConvertFloat4ToU32(state.theme.ruler_text),
                     MeasurementUnitLabel(state.ruler_unit));

  if (ImGui::BeginPopup("ruler_context_menu")) {
    ImGui::TextUnformatted("Ruler Units");
    ImGui::Separator();
    for (MeasurementUnit unit :
         {MeasurementUnit::Millimeters, MeasurementUnit::Inches,
          MeasurementUnit::Pixels}) {
      const bool selected = state.ruler_unit == unit;
      if (ImGui::MenuItem(MeasurementUnitLabel(unit), nullptr, selected)) {
        state.ruler_unit = unit;
      }
    }
    ImGui::EndPopup();
  }

  if (ImGui::BeginPopup("guide_context_menu")) {
    if (Guide *guide = FindGuide(state, transient_state.context_guide_id);
        guide != nullptr) {
      if (ImGui::MenuItem(guide->locked ? "Unlock guide" : "Lock guide")) {
        guide->locked = !guide->locked;
      }
      if (ImGui::MenuItem("Delete guide")) {
        RemoveGuide(state, guide->id);
        transient_state.context_guide_id = 0;
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::EndPopup();
  }

  if (ImGui::BeginPopup("canvas_context_menu")) {
    ImGui::TextUnformatted("Canvas actions TBD");
    ImGui::Separator();
    ImGui::MenuItem("Placeholder", nullptr, false, false);
    ImGui::EndPopup();
  }

  ImGui::PopID();
  return canvas_hovered;
}

} // namespace im2d