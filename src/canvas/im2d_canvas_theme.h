#pragma once

#include <imgui.h>

#include <array>

namespace im2d {

struct CanvasTheme {
  ImVec4 canvas_background = ImVec4(0.12f, 0.13f, 0.15f, 1.0f);
  ImVec4 ruler_background = ImVec4(0.16f, 0.17f, 0.20f, 1.0f);
  ImVec4 ruler_text = ImVec4(0.88f, 0.90f, 0.93f, 1.0f);
  ImVec4 ruler_ticks = ImVec4(0.52f, 0.56f, 0.63f, 1.0f);
  ImVec4 grid_major = ImVec4(0.30f, 0.35f, 0.41f, 0.60f);
  ImVec4 grid_minor = ImVec4(0.22f, 0.26f, 0.31f, 0.30f);
  ImVec4 guide = ImVec4(0.90f, 0.38f, 0.29f, 1.0f);
  ImVec4 guide_hovered = ImVec4(1.00f, 0.62f, 0.22f, 1.0f);
  ImVec4 guide_locked = ImVec4(0.78f, 0.30f, 0.22f, 1.0f);
  ImVec4 working_area_fill = ImVec4(0.19f, 0.22f, 0.19f, 0.92f);
  ImVec4 working_area_border = ImVec4(0.58f, 0.74f, 0.58f, 1.0f);
  ImVec4 working_area_selected = ImVec4(0.97f, 0.82f, 0.36f, 1.0f);
  ImVec4 export_area_outline = ImVec4(0.43f, 0.77f, 0.92f, 0.65f);
  ImVec4 exclusion_area_fill = ImVec4(0.88f, 0.33f, 0.24f, 0.18f);
  ImVec4 exclusion_area_outline = ImVec4(0.95f, 0.47f, 0.35f, 0.92f);
  ImVec4 exclusion_area_selected = ImVec4(1.0f, 0.62f, 0.48f, 1.0f);
  ImVec4 imported_issue_open_geometry = ImVec4(0.94f, 0.57f, 0.24f, 1.0f);
  ImVec4 imported_issue_ambiguous_cleanup = ImVec4(0.24f, 0.69f, 0.65f, 1.0f);
  ImVec4 imported_issue_orphan_hole = ImVec4(0.91f, 0.33f, 0.24f, 1.0f);
  ImVec4 imported_issue_placeholder_text = ImVec4(0.91f, 0.67f, 0.24f, 1.0f);
  ImVec4 operation_issue = ImVec4(0.35f, 0.67f, 1.0f, 1.0f);
  ImVec4 preview_assigned_stroke = ImVec4(0.35f, 0.63f, 1.0f, 0.92f);
  ImVec4 preview_assigned_fill = ImVec4(0.35f, 0.63f, 1.0f, 0.20f);
  ImVec4 preview_crossing_stroke = ImVec4(0.95f, 0.69f, 0.20f, 0.92f);
  ImVec4 preview_crossing_fill = ImVec4(0.95f, 0.69f, 0.20f, 0.20f);
  ImVec4 preview_orphan_stroke = ImVec4(0.91f, 0.33f, 0.24f, 0.92f);
  ImVec4 preview_orphan_fill = ImVec4(0.91f, 0.33f, 0.24f, 0.17f);
  ImVec4 preview_default_stroke = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
  ImVec4 preview_default_fill = ImVec4(1.0f, 1.0f, 1.0f, 0.13f);
  std::array<ImVec4, 6> preview_bucket_strokes = {
      ImVec4(0.35f, 0.63f, 1.0f, 0.92f), ImVec4(0.36f, 0.79f, 0.43f, 0.92f),
      ImVec4(0.76f, 0.47f, 1.0f, 0.92f), ImVec4(0.24f, 0.79f, 0.76f, 0.92f),
      ImVec4(1.0f, 0.53f, 0.36f, 0.92f), ImVec4(0.94f, 0.82f, 0.29f, 0.92f),
  };
  std::array<ImVec4, 6> preview_bucket_fills = {
      ImVec4(0.35f, 0.63f, 1.0f, 0.11f), ImVec4(0.36f, 0.79f, 0.43f, 0.11f),
      ImVec4(0.76f, 0.47f, 1.0f, 0.11f), ImVec4(0.24f, 0.79f, 0.76f, 0.11f),
      ImVec4(1.0f, 0.53f, 0.36f, 0.11f), ImVec4(0.94f, 0.82f, 0.29f, 0.11f),
  };
  ImVec4 preview_banner_background = ImVec4(0.08f, 0.09f, 0.13f, 0.88f);
  ImVec4 preview_banner_border = ImVec4(0.55f, 0.67f, 0.86f, 0.86f);
  ImVec4 preview_banner_title = ImVec4(0.90f, 0.93f, 0.97f, 1.0f);
  ImVec4 preview_banner_summary = ImVec4(0.71f, 0.77f, 0.89f, 1.0f);
  ImVec4 preview_label_background = ImVec4(0.06f, 0.07f, 0.10f, 0.86f);
};

} // namespace im2d
