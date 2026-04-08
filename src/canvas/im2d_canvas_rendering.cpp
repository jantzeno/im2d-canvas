#include "im2d_canvas_widget_internal.h"

#include "im2d_canvas_document.h"
#include "im2d_canvas_internal.h"
#include "im2d_canvas_notification.h"
#include "im2d_canvas_snap.h"
#include "im2d_canvas_units.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include <imgui_internal.h>

namespace im2d::detail {

namespace {

constexpr float kImportedPreviewStrokeWidth = 1.0f;
constexpr float kImportedPreviewCurveFlatnessPixels = 0.35f;
constexpr int kImportedPreviewCurveMaxSubdivisionDepth = 10;
constexpr float kMarqueeOutlineThickness = 1.5f;
constexpr float kMarqueeFillAlpha = 0.18f;

} // namespace

ImU32 ThemeColorToU32(const ImVec4 &color) {
  return ImGui::ColorConvertFloat4ToU32(color);
}

PreviewOverlayColors MakePreviewOverlayColors(const ImVec4 &stroke,
                                              const ImVec4 &fill) {
  return {ThemeColorToU32(stroke), ThemeColorToU32(fill)};
}

bool ImportedIssueOverlayVisible(const ImportedIssueOverlaySettings &settings,
                                 const uint32_t issue_flags) {
  return (settings.show_ambiguous_cleanup &&
          HasImportedElementIssueFlag(
              issue_flags, ImportedElementIssueFlagAmbiguousCleanup)) ||
         (settings.show_orphan_hole &&
          HasImportedElementIssueFlag(issue_flags,
                                      ImportedElementIssueFlagOrphanHole)) ||
         (settings.show_placeholder_text &&
          HasImportedElementIssueFlag(
              issue_flags, ImportedElementIssueFlagPlaceholderText)) ||
         (settings.show_open_geometry &&
          HasImportedElementIssueFlag(issue_flags,
                                      ImportedElementIssueFlagOpenGeometry));
}

ImU32 ImportedIssueOverlayColor(const CanvasTheme &theme,
                                const ImportedIssueOverlaySettings &settings,
                                const uint32_t issue_flags) {
  if (settings.show_ambiguous_cleanup &&
      HasImportedElementIssueFlag(issue_flags,
                                  ImportedElementIssueFlagAmbiguousCleanup)) {
    return ThemeColorToU32(theme.imported_issue_ambiguous_cleanup);
  }
  if (settings.show_orphan_hole &&
      HasImportedElementIssueFlag(issue_flags,
                                  ImportedElementIssueFlagOrphanHole)) {
    return ThemeColorToU32(theme.imported_issue_orphan_hole);
  }
  if (settings.show_placeholder_text &&
      HasImportedElementIssueFlag(issue_flags,
                                  ImportedElementIssueFlagPlaceholderText)) {
    return ThemeColorToU32(theme.imported_issue_placeholder_text);
  }
  return ThemeColorToU32(theme.imported_issue_open_geometry);
}

PreviewOverlayColors GetSeparationPreviewColors(
    const CanvasTheme &theme,
    const ImportedSeparationPreviewClassification classification) {
  switch (classification) {
  case ImportedSeparationPreviewClassification::Assigned:
    return MakePreviewOverlayColors(theme.preview_assigned_stroke,
                                    theme.preview_assigned_fill);
  case ImportedSeparationPreviewClassification::Crossing:
    return MakePreviewOverlayColors(theme.preview_crossing_stroke,
                                    theme.preview_crossing_fill);
  case ImportedSeparationPreviewClassification::Orphan:
    return MakePreviewOverlayColors(theme.preview_orphan_stroke,
                                    theme.preview_orphan_fill);
  }
  return MakePreviewOverlayColors(theme.preview_default_stroke,
                                  theme.preview_default_fill);
}

std::vector<PreviewBucketRegion> BuildPreviewBucketRegions(
    const CanvasTheme &theme,
    const std::vector<ImportedSeparationPreviewPart> &parts) {
  std::vector<PreviewBucketRegion> bucket_regions;
  for (const ImportedSeparationPreviewPart &part : parts) {
    if (part.classification !=
            ImportedSeparationPreviewClassification::Assigned ||
        part.bucket_index < 0) {
      continue;
    }

    const bool already_added =
        std::any_of(bucket_regions.begin(), bucket_regions.end(),
                    [&part](const PreviewBucketRegion &region) {
                      return region.bucket_index == part.bucket_index;
                    });
    if (already_added) {
      continue;
    }

    PreviewOverlayColors colors =
        GetSeparationPreviewColors(theme, part.classification);
    const size_t palette_index = static_cast<size_t>(part.bucket_index) %
                                 theme.preview_bucket_strokes.size();
    colors.stroke =
        ThemeColorToU32(theme.preview_bucket_strokes[palette_index]);
    colors.fill = ThemeColorToU32(theme.preview_bucket_fills[palette_index]);
    bucket_regions.push_back(
        {part.bucket_index, part.bucket_column, part.bucket_row, colors});
  }
  return bucket_regions;
}

const char *
PreviewLabelForPart(ImportedSeparationPreviewClassification classification,
                    int bucket_index, std::string *scratch_label) {
  if (classification == ImportedSeparationPreviewClassification::Assigned) {
    *scratch_label = "B" + std::to_string(bucket_index + 1);
    return scratch_label->c_str();
  }
  if (classification == ImportedSeparationPreviewClassification::Crossing) {
    return "Crossing";
  }
  return "Orphan";
}

ActiveCanvasNotification
ResolveActiveCanvasNotification(const CanvasState &state) {
  if (state.imported_artwork_auto_cut_preview.active) {
    return ActiveCanvasNotification::AutoCutPreview;
  }
  if (state.imported_artwork_separation_preview.active) {
    return ActiveCanvasNotification::GuideSplitPreview;
  }
  if (state.canvas_notification.active) {
    return ActiveCanvasNotification::CallerNotification;
  }
  return ActiveCanvasNotification::None;
}

ActiveCanvasNotificationContent
BuildActiveCanvasNotificationContent(const CanvasState &state) {
  ActiveCanvasNotificationContent content;
  content.kind = ResolveActiveCanvasNotification(state);
  switch (content.kind) {
  case ActiveCanvasNotification::GuideSplitPreview:
    content.title = "Guide Split Preview Active";
    content.summary =
        std::to_string(
            state.imported_artwork_separation_preview.future_object_count) +
        " objects, " +
        std::to_string(
            state.imported_artwork_separation_preview.skipped_count) +
        " skipped";
    break;
  case ActiveCanvasNotification::AutoCutPreview:
    content.title = "Auto Cut Preview Active";
    content.summary =
        std::to_string(
            state.imported_artwork_auto_cut_preview.future_band_count) +
        " bands, " +
        std::to_string(state.imported_artwork_auto_cut_preview.skipped_count) +
        " skipped";
    break;
  case ActiveCanvasNotification::CallerNotification:
    content.title = state.canvas_notification.title.c_str();
    content.summary = state.canvas_notification.summary;
    content.dismiss_mode = state.canvas_notification.dismiss_mode;
    content.notification_id = state.canvas_notification.id;
    break;
  case ActiveCanvasNotification::None:
    break;
  }
  return content;
}

void DrawCallerNotification(ImDrawList *draw_list, const CanvasState &state,
                            const ImRect &canvas_rect) {
  const CanvasNotificationState &notification = state.canvas_notification;
  if (!notification.active) {
    return;
  }

  DrawCanvasNotificationBanner(
      draw_list,
      {.min = ImVec2(canvas_rect.Min.x + 12.0f, canvas_rect.Min.y + 12.0f)},
      CanvasNotificationBannerStyleFromTheme(state.theme),
      notification.title.c_str(), notification.summary,
      notification.dismiss_mode);
}

void DrawBandPreviewOverlay(
    ImDrawList *draw_list, const CanvasState &state, const ImRect &canvas_rect,
    int artwork_id, const std::vector<float> &vertical_positions,
    const std::vector<float> &horizontal_positions,
    const std::vector<ImportedSeparationPreviewPart> &parts, const char *title,
    const std::string &preview_summary, const bool show_notification,
    const CanvasNotificationDismissMode dismiss_mode) {
  draw_list->PushClipRect(canvas_rect.Min, canvas_rect.Max, true);
  if (show_notification) {
    DrawCanvasNotificationBanner(
        draw_list,
        {.min = ImVec2(canvas_rect.Min.x + 12.0f, canvas_rect.Min.y + 12.0f)},
        CanvasNotificationBannerStyleFromTheme(state.theme), title,
        preview_summary, dismiss_mode);
  }

  ImRect preview_artwork_rect = canvas_rect;
  if (const ImportedArtwork *artwork = FindImportedArtwork(state, artwork_id)) {
    preview_artwork_rect =
        ImportedArtworkScreenRect(state, canvas_rect.Min, *artwork);
    preview_artwork_rect.ClipWith(canvas_rect);
  }

  const std::vector<PreviewBucketRegion> bucket_regions =
      BuildPreviewBucketRegions(state.theme, parts);
  const ImVec2 visible_world_min =
      ScreenToWorld(state, canvas_rect.Min, canvas_rect.Min);
  const ImVec2 visible_world_max =
      ScreenToWorld(state, canvas_rect.Min, canvas_rect.Max);

  auto bucket_min_for = [](const std::vector<float> &positions, int index,
                           float fallback_min) {
    if (index <= 0 || positions.empty()) {
      return fallback_min;
    }
    return positions[static_cast<size_t>(index - 1)];
  };
  auto bucket_max_for = [](const std::vector<float> &positions, int index,
                           float fallback_max) {
    if (positions.empty() || index >= static_cast<int>(positions.size())) {
      return fallback_max;
    }
    return positions[static_cast<size_t>(index)];
  };

  for (const PreviewBucketRegion &region : bucket_regions) {
    const float world_left = bucket_min_for(
        vertical_positions, region.bucket_column, visible_world_min.x);
    const float world_right = bucket_max_for(
        vertical_positions, region.bucket_column, visible_world_max.x);
    const float world_top = bucket_min_for(
        horizontal_positions, region.bucket_row, visible_world_min.y);
    const float world_bottom = bucket_max_for(
        horizontal_positions, region.bucket_row, visible_world_max.y);
    const ImRect band_screen_rect =
        WorldRectToScreenRect(state, canvas_rect.Min,
                              ImRect(ImVec2(world_left, world_top),
                                     ImVec2(world_right, world_bottom)));
    ImRect clipped_band_rect = band_screen_rect;
    clipped_band_rect.ClipWith(preview_artwork_rect);
    if (!clipped_band_rect.IsInverted()) {
      draw_list->AddRectFilled(clipped_band_rect.Min, clipped_band_rect.Max,
                               region.colors.fill);
      draw_list->AddRect(clipped_band_rect.Min, clipped_band_rect.Max,
                         region.colors.stroke, 0.0f, 0, 2.0f);

      const std::string bucket_label =
          "Split " + std::to_string(region.bucket_index + 1);
      const ImVec2 label_pos(clipped_band_rect.Min.x + 10.0f,
                             clipped_band_rect.Min.y + 10.0f);
      draw_list->AddText(label_pos, region.colors.stroke, bucket_label.c_str());
    }
  }

  std::string label_scratch;
  for (const ImportedSeparationPreviewPart &part : parts) {
    PreviewOverlayColors colors =
        GetSeparationPreviewColors(state.theme, part.classification);
    if (part.classification ==
        ImportedSeparationPreviewClassification::Assigned) {
      continue;
    }

    const ImRect screen_rect = WorldRectToScreenRect(
        state, canvas_rect.Min,
        ImRect(part.world_bounds_min, part.world_bounds_max));
    draw_list->AddRect(screen_rect.Min, screen_rect.Max, colors.stroke, 4.0f, 0,
                       3.0f);

    const char *label = PreviewLabelForPart(part.classification,
                                            part.bucket_index, &label_scratch);
    const ImVec2 label_min(screen_rect.Min.x + 4.0f, screen_rect.Min.y + 4.0f);
    const ImVec2 label_max(label_min.x + 72.0f, label_min.y + 20.0f);
    draw_list->AddRectFilled(
        label_min, label_max,
        ThemeColorToU32(state.theme.preview_label_background), 4.0f);
    draw_list->AddRect(label_min, label_max, colors.stroke, 4.0f, 0, 1.5f);
    draw_list->AddText(ImVec2(label_min.x + 6.0f, label_min.y + 3.0f),
                       colors.stroke, label);
  }
  draw_list->PopClipRect();
}

bool TryGetImportedDebugScreenRect(const CanvasState &state,
                                   const ImVec2 &canvas_min,
                                   const ImportedArtwork &artwork,
                                   ImRect *screen_rect) {
  if (state.selected_imported_debug.artwork_id != artwork.id) {
    return false;
  }

  switch (state.selected_imported_debug.kind) {
  case ImportedDebugSelectionKind::Artwork:
    *screen_rect = ImportedArtworkScreenRect(state, canvas_min, artwork);
    return true;
  case ImportedDebugSelectionKind::Group: {
    const ImportedGroup *group =
        FindImportedGroup(artwork, state.selected_imported_debug.item_id);
    if (group == nullptr) {
      return false;
    }
    ImVec2 world_min;
    ImVec2 world_max;
    ImportedLocalBoundsToWorldBounds(artwork, group->bounds_min,
                                     group->bounds_max, &world_min, &world_max);
    *screen_rect =
        WorldRectToScreenRect(state, canvas_min, ImRect(world_min, world_max));
    return true;
  }
  case ImportedDebugSelectionKind::Path: {
    const ImportedPath *path =
        FindImportedPath(artwork, state.selected_imported_debug.item_id);
    if (path == nullptr) {
      return false;
    }
    ImVec2 world_min;
    ImVec2 world_max;
    ImportedLocalBoundsToWorldBounds(artwork, path->bounds_min,
                                     path->bounds_max, &world_min, &world_max);
    *screen_rect =
        WorldRectToScreenRect(state, canvas_min, ImRect(world_min, world_max));
    return true;
  }
  case ImportedDebugSelectionKind::DxfText: {
    const ImportedDxfText *text =
        FindImportedDxfText(artwork, state.selected_imported_debug.item_id);
    if (text == nullptr) {
      return false;
    }
    ImVec2 world_min;
    ImVec2 world_max;
    ImportedLocalBoundsToWorldBounds(artwork, text->bounds_min,
                                     text->bounds_max, &world_min, &world_max);
    *screen_rect =
        WorldRectToScreenRect(state, canvas_min, ImRect(world_min, world_max));
    return true;
  }
  case ImportedDebugSelectionKind::None:
    break;
  }

  return false;
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

int DecimalPlacesForStep(float step_units) {
  if (step_units <= 0.0f) {
    return 0;
  }

  float normalized = std::fabs(step_units);
  int decimal_places = 0;
  while (decimal_places < 6 &&
         std::fabs(normalized - std::round(normalized)) > 0.0001f) {
    normalized *= 10.0f;
    ++decimal_places;
  }

  return decimal_places;
}

std::string FormatRulerTickLabel(float tick_units, float step_units) {
  const int decimal_places = DecimalPlacesForStep(step_units);
  char label[32];
  std::snprintf(label, sizeof(label), "%.*f", decimal_places, tick_units);

  std::string formatted(label);
  if (formatted.find('.') != std::string::npos) {
    while (!formatted.empty() && formatted.back() == '0') {
      formatted.pop_back();
    }
    if (!formatted.empty() && formatted.back() == '.') {
      formatted.pop_back();
    }
  }

  if (formatted == "-0") {
    return "0";
  }

  return formatted;
}

float RulerDirection(const CanvasState &state, bool horizontal) {
  const float direction = horizontal
                              ? state.ruler_reference.horizontal_direction
                              : state.ruler_reference.vertical_direction;
  return std::abs(direction) < 1e-6f ? 1.0f : direction;
}

float RulerOriginWorld(const CanvasState &state, bool horizontal) {
  return horizontal ? state.ruler_reference.origin_world.x
                    : state.ruler_reference.origin_world.y;
}

float TransformRulerWorld(const CanvasState &state, float world,
                          bool horizontal) {
  if (!state.ruler_reference.enabled) {
    return world;
  }
  return (world - RulerOriginWorld(state, horizontal)) *
         RulerDirection(state, horizontal);
}

float InverseTransformRulerWorld(const CanvasState &state, float world,
                                 bool horizontal) {
  if (!state.ruler_reference.enabled) {
    return world;
  }
  return RulerOriginWorld(state, horizontal) +
         (world / RulerDirection(state, horizontal));
}

float ComputeLeftRulerThickness(const CanvasState &state,
                                const ImRect &total_rect,
                                float top_ruler_thickness,
                                float minimum_thickness) {
  const float pixels_per_unit =
      UnitsToPixels(1.0f, state.ruler_unit, state.calibration) *
      state.view.zoom;
  if (pixels_per_unit <= 0.0f) {
    return minimum_thickness;
  }

  const float major_step_units = NiceStep(80.0f / pixels_per_unit);
  const float minor_step_units = major_step_units / 5.0f;
  if (major_step_units <= 0.0f || minor_step_units <= 0.0f) {
    return minimum_thickness;
  }

  const ImVec2 canvas_min(total_rect.Min.x + minimum_thickness,
                          total_rect.Min.y + top_ruler_thickness);
  const float visible_min_world =
      ScreenToWorld(state, canvas_min, ImVec2(canvas_min.x, canvas_min.y)).y;
  const float visible_max_world =
      ScreenToWorld(state, canvas_min, ImVec2(canvas_min.x, total_rect.Max.y))
          .y;

  const float min_units =
      PixelsToUnits(TransformRulerWorld(state, visible_min_world, false),
                    state.ruler_unit, state.calibration);
  const float max_units =
      PixelsToUnits(TransformRulerWorld(state, visible_max_world, false),
                    state.ruler_unit, state.calibration);
  const float start_units =
      std::floor(std::min(min_units, max_units) / minor_step_units) *
      minor_step_units;
  const float end_units =
      std::ceil(std::max(min_units, max_units) / minor_step_units) *
      minor_step_units;

  float max_label_width = 0.0f;
  for (float tick_units = start_units; tick_units <= end_units;
       tick_units += minor_step_units) {
    const float remainder = std::fmod(std::fabs(tick_units), major_step_units);
    const bool major = remainder < 0.0001f ||
                       std::fabs(remainder - major_step_units) < 0.0001f;
    if (!major) {
      continue;
    }

    const std::string label =
        FormatRulerTickLabel(tick_units, major_step_units);
    max_label_width =
        std::max(max_label_width, ImGui::CalcTextSize(label.c_str()).x);
  }

  constexpr float kRulerLabelPadding = 4.0f;
  return std::max(minimum_thickness,
                  max_label_width + kRulerLabelPadding * 2.0f);
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

WorkingAreaHit FindHoveredWorkingArea(const CanvasState &state,
                                      const ImVec2 &canvas_min,
                                      const ImRect &canvas_rect,
                                      const ImVec2 &mouse_pos,
                                      float resize_handle_size) {
  if (!canvas_rect.Contains(mouse_pos)) {
    return {};
  }

  for (auto it = state.working_areas.rbegin(); it != state.working_areas.rend();
       ++it) {
    const WorkingArea &area = *it;
    if (!area.visible) {
      continue;
    }

    const ImRect screen_rect = WorkingAreaScreenRect(state, canvas_min, area);
    if (!screen_rect.Contains(mouse_pos)) {
      continue;
    }

    if (HasWorkingAreaFlag(area.flags, WorkingAreaFlagResizable)) {
      const ImRect handle_rect(ImVec2(screen_rect.Max.x - resize_handle_size,
                                      screen_rect.Max.y - resize_handle_size),
                               screen_rect.Max);
      if (handle_rect.Contains(mouse_pos)) {
        return {area.id, WorkingAreaHitZone::ResizeHandle};
      }
    }

    return {area.id, WorkingAreaHitZone::Body};
  }

  return {};
}

ImportedArtworkHit FindHoveredImportedArtwork(const CanvasState &state,
                                              const ImVec2 &canvas_min,
                                              const ImRect &canvas_rect,
                                              const ImVec2 &mouse_pos,
                                              float resize_handle_size) {
  if (!canvas_rect.Contains(mouse_pos)) {
    return {};
  }

  for (auto it = state.imported_artwork.rbegin();
       it != state.imported_artwork.rend(); ++it) {
    const ImportedArtwork &artwork = *it;
    if (!artwork.visible) {
      continue;
    }

    const ImRect screen_rect =
        ImportedArtworkScreenRect(state, canvas_min, artwork);
    if (!screen_rect.Contains(mouse_pos)) {
      continue;
    }

    if (HasImportedArtworkFlag(artwork.flags, ImportedArtworkFlagResizable)) {
      const ImRect handle_rect(ImVec2(screen_rect.Max.x - resize_handle_size,
                                      screen_rect.Max.y - resize_handle_size),
                               screen_rect.Max);
      if (handle_rect.Contains(mouse_pos)) {
        return {artwork.id, ImportedArtworkHitZone::ResizeHandle};
      }
    }

    return {artwork.id, ImportedArtworkHitZone::Body};
  }

  return {};
}

bool IsLastOperationIssueElement(const CanvasState &state, int artwork_id,
                                 ImportedElementKind kind, int item_id) {
  if (state.last_imported_operation_issue_artwork_id != artwork_id) {
    return false;
  }

  return std::any_of(
      state.last_imported_operation_issue_elements.begin(),
      state.last_imported_operation_issue_elements.end(),
      [kind, item_id](const ImportedElementSelection &selection) {
        return selection.kind == kind && selection.item_id == item_id;
      });
}

void DrawSeparationPreviewOverlay(
    ImDrawList *draw_list, const CanvasState &state, const ImRect &canvas_rect,
    const bool show_notification,
    const CanvasNotificationDismissMode dismiss_mode) {
  const ImportedArtworkSeparationPreview &preview =
      state.imported_artwork_separation_preview;
  if (!preview.active) {
    return;
  }

  const std::string preview_summary =
      std::to_string(preview.future_object_count) + " objects, " +
      std::to_string(preview.skipped_count) + " skipped";

  std::vector<float> vertical_positions;
  std::vector<float> horizontal_positions;
  for (const int guide_id : preview.guide_ids) {
    const Guide *guide = FindGuide(state, guide_id);
    if (guide == nullptr) {
      continue;
    }
    if (guide->orientation == GuideOrientation::Vertical) {
      vertical_positions.push_back(guide->position);
    } else {
      horizontal_positions.push_back(guide->position);
    }
  }
  std::sort(vertical_positions.begin(), vertical_positions.end());
  std::sort(horizontal_positions.begin(), horizontal_positions.end());

  DrawBandPreviewOverlay(draw_list, state, canvas_rect, preview.artwork_id,
                         vertical_positions, horizontal_positions,
                         preview.parts, "Guide Split Preview Active",
                         preview_summary, show_notification, dismiss_mode);
}

void DrawAutoCutPreviewOverlay(
    ImDrawList *draw_list, const CanvasState &state, const ImRect &canvas_rect,
    const bool show_notification,
    const CanvasNotificationDismissMode dismiss_mode) {
  const ImportedArtworkAutoCutPreview &preview =
      state.imported_artwork_auto_cut_preview;
  if (!preview.active) {
    return;
  }

  const std::string preview_summary =
      std::to_string(preview.future_band_count) + " bands, " +
      std::to_string(preview.skipped_count) + " skipped";
  DrawBandPreviewOverlay(draw_list, state, canvas_rect, preview.artwork_id,
                         preview.vertical_positions,
                         preview.horizontal_positions, preview.parts,
                         "Auto Cut Preview Active", preview_summary,
                         show_notification, dismiss_mode);
}

void RemoveGuide(CanvasState &state, int guide_id) {
  std::erase_if(state.guides, [guide_id](const Guide &guide) {
    return guide.id == guide_id;
  });
}

bool PreviewPointsNear(const ImVec2 &a, const ImVec2 &b) {
  const float dx = a.x - b.x;
  const float dy = a.y - b.y;
  return dx * dx + dy * dy <= 1.0f;
}

void AppendPreviewPathPoint(std::vector<ImVec2> *points, const ImVec2 &point) {
  if (!points->empty() && PreviewPointsNear(points->back(), point)) {
    return;
  }
  points->push_back(point);
}

float DistancePointToLineSquared(const ImVec2 &point, const ImVec2 &line_start,
                                 const ImVec2 &line_end) {
  const float dx = line_end.x - line_start.x;
  const float dy = line_end.y - line_start.y;
  const float line_length_squared = dx * dx + dy * dy;
  if (line_length_squared <= 0.000001f) {
    const float px = point.x - line_start.x;
    const float py = point.y - line_start.y;
    return px * px + py * py;
  }

  const float t = std::clamp(
      ((point.x - line_start.x) * dx + (point.y - line_start.y) * dy) /
          line_length_squared,
      0.0f, 1.0f);
  const float projection_x = line_start.x + t * dx;
  const float projection_y = line_start.y + t * dy;
  const float px = point.x - projection_x;
  const float py = point.y - projection_y;
  return px * px + py * py;
}

bool ImportedPreviewCurveFlatEnough(const ImVec2 &start, const ImVec2 &control1,
                                    const ImVec2 &control2, const ImVec2 &end) {
  const float tolerance_squared =
      kImportedPreviewCurveFlatnessPixels * kImportedPreviewCurveFlatnessPixels;
  return DistancePointToLineSquared(control1, start, end) <=
             tolerance_squared &&
         DistancePointToLineSquared(control2, start, end) <= tolerance_squared;
}

void AppendAdaptiveImportedPreviewCurvePoints(std::vector<ImVec2> *points,
                                              const ImVec2 &start,
                                              const ImVec2 &control1,
                                              const ImVec2 &control2,
                                              const ImVec2 &end, int depth) {
  if (depth >= kImportedPreviewCurveMaxSubdivisionDepth ||
      ImportedPreviewCurveFlatEnough(start, control1, control2, end)) {
    AppendPreviewPathPoint(points, end);
    return;
  }

  const ImVec2 start_control1_mid((start.x + control1.x) * 0.5f,
                                  (start.y + control1.y) * 0.5f);
  const ImVec2 control1_control2_mid((control1.x + control2.x) * 0.5f,
                                     (control1.y + control2.y) * 0.5f);
  const ImVec2 control2_end_mid((control2.x + end.x) * 0.5f,
                                (control2.y + end.y) * 0.5f);
  const ImVec2 left_control2(
      (start_control1_mid.x + control1_control2_mid.x) * 0.5f,
      (start_control1_mid.y + control1_control2_mid.y) * 0.5f);
  const ImVec2 right_control1(
      (control1_control2_mid.x + control2_end_mid.x) * 0.5f,
      (control1_control2_mid.y + control2_end_mid.y) * 0.5f);
  const ImVec2 split_point((left_control2.x + right_control1.x) * 0.5f,
                           (left_control2.y + right_control1.y) * 0.5f);

  AppendAdaptiveImportedPreviewCurvePoints(
      points, start, start_control1_mid, left_control2, split_point, depth + 1);
  AppendAdaptiveImportedPreviewCurvePoints(points, split_point, right_control1,
                                           control2_end_mid, end, depth + 1);
}

void AppendImportedSegmentPath(ImDrawList *draw_list, const CanvasState &state,
                               const ImRect &canvas_rect,
                               const ImportedArtwork &artwork,
                               const std::vector<ImportedPathSegment> &segments,
                               bool closed) {
  if (segments.empty()) {
    return;
  }

  std::vector<ImVec2> screen_points;
  screen_points.reserve(segments.size() * 4);
  const ImVec2 first_world =
      ImportedArtworkPointToWorld(artwork, segments.front().start);
  AppendPreviewPathPoint(&screen_points,
                         WorldToScreen(state, canvas_rect.Min, first_world));

  for (const ImportedPathSegment &segment : segments) {
    if (segment.kind == ImportedPathSegmentKind::Line) {
      const ImVec2 end_world =
          ImportedArtworkPointToWorld(artwork, segment.end);
      AppendPreviewPathPoint(&screen_points,
                             WorldToScreen(state, canvas_rect.Min, end_world));
      continue;
    }

    const ImVec2 start_screen =
        WorldToScreen(state, canvas_rect.Min,
                      ImportedArtworkPointToWorld(artwork, segment.start));
    const ImVec2 control1_screen =
        WorldToScreen(state, canvas_rect.Min,
                      ImportedArtworkPointToWorld(artwork, segment.control1));
    const ImVec2 control2_screen =
        WorldToScreen(state, canvas_rect.Min,
                      ImportedArtworkPointToWorld(artwork, segment.control2));
    const ImVec2 end_screen =
        WorldToScreen(state, canvas_rect.Min,
                      ImportedArtworkPointToWorld(artwork, segment.end));
    AppendAdaptiveImportedPreviewCurvePoints(&screen_points, start_screen,
                                             control1_screen, control2_screen,
                                             end_screen, 0);
  }

  if (closed && screen_points.size() > 1 &&
      PreviewPointsNear(screen_points.front(), screen_points.back())) {
    screen_points.pop_back();
  }

  draw_list->PathClear();
  for (const ImVec2 &point : screen_points) {
    draw_list->PathLineTo(point);
  }
}

void FlattenImportedTextContour(const ImportedTextContour &contour,
                                std::vector<ImVec2> *points) {
  points->clear();
  if (contour.segments.empty()) {
    return;
  }

  points->push_back(contour.segments.front().start);
  for (const ImportedPathSegment &segment : contour.segments) {
    if (segment.kind == ImportedPathSegmentKind::Line) {
      points->push_back(segment.end);
      continue;
    }

    constexpr int kCurveSamples = 12;
    for (int sample_index = 1; sample_index <= kCurveSamples; ++sample_index) {
      const float t =
          static_cast<float>(sample_index) / static_cast<float>(kCurveSamples);
      points->push_back(CubicBezierPoint(segment.start, segment.control1,
                                         segment.control2, segment.end, t));
    }
  }

  if (points->size() > 1) {
    const ImVec2 &first = points->front();
    const ImVec2 &last = points->back();
    if (std::abs(first.x - last.x) < 0.001f &&
        std::abs(first.y - last.y) < 0.001f) {
      points->pop_back();
    }
  }
}

float PolygonSignedArea(const std::vector<ImVec2> &polygon) {
  if (polygon.size() < 3) {
    return 0.0f;
  }

  float twice_area = 0.0f;
  for (size_t index = 0; index < polygon.size(); ++index) {
    const ImVec2 &current = polygon[index];
    const ImVec2 &next = polygon[(index + 1) % polygon.size()];
    twice_area += current.x * next.y - next.x * current.y;
  }
  return twice_area * 0.5f;
}

void EnsureCounterClockwise(std::vector<ImVec2> *polygon) {
  if (PolygonSignedArea(*polygon) < 0.0f) {
    std::reverse(polygon->begin(), polygon->end());
  }
}

void EnsureClockwise(std::vector<ImVec2> *polygon) {
  if (PolygonSignedArea(*polygon) > 0.0f) {
    std::reverse(polygon->begin(), polygon->end());
  }
}

bool PointInPolygon(const std::vector<ImVec2> &polygon, const ImVec2 &point) {
  bool inside = false;
  for (size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
    const ImVec2 &a = polygon[i];
    const ImVec2 &b = polygon[j];
    const bool intersects =
        ((a.y > point.y) != (b.y > point.y)) &&
        (point.x < (b.x - a.x) * (point.y - a.y) / (b.y - a.y + 1e-6f) + a.x);
    if (intersects) {
      inside = !inside;
    }
  }
  return inside;
}

size_t FindNearestPolygonVertex(const std::vector<ImVec2> &polygon,
                                const ImVec2 &point) {
  size_t best_index = 0;
  float best_distance_sq = std::numeric_limits<float>::max();
  for (size_t index = 0; index < polygon.size(); ++index) {
    const float dx = polygon[index].x - point.x;
    const float dy = polygon[index].y - point.y;
    const float distance_sq = dx * dx + dy * dy;
    if (distance_sq < best_distance_sq) {
      best_distance_sq = distance_sq;
      best_index = index;
    }
  }
  return best_index;
}

size_t FindRightmostVertex(const std::vector<ImVec2> &polygon) {
  size_t best_index = 0;
  for (size_t index = 1; index < polygon.size(); ++index) {
    if (polygon[index].x > polygon[best_index].x ||
        (polygon[index].x == polygon[best_index].x &&
         polygon[index].y < polygon[best_index].y)) {
      best_index = index;
    }
  }
  return best_index;
}

void BridgeHoleIntoPolygon(std::vector<ImVec2> *polygon,
                           const std::vector<ImVec2> &hole) {
  if (polygon->empty() || hole.empty()) {
    return;
  }

  const size_t hole_index = FindRightmostVertex(hole);
  const size_t polygon_index =
      FindNearestPolygonVertex(*polygon, hole[hole_index]);

  std::vector<ImVec2> bridged;
  bridged.reserve(polygon->size() + hole.size() + 2);
  bridged.insert(bridged.end(), polygon->begin(),
                 polygon->begin() +
                     static_cast<std::ptrdiff_t>(polygon_index + 1));
  for (size_t offset = 0; offset < hole.size(); ++offset) {
    const size_t index = (hole_index + offset) % hole.size();
    bridged.push_back(hole[index]);
  }
  bridged.push_back(hole[hole_index]);
  bridged.push_back((*polygon)[polygon_index]);
  bridged.insert(bridged.end(),
                 polygon->begin() +
                     static_cast<std::ptrdiff_t>(polygon_index + 1),
                 polygon->end());
  *polygon = std::move(bridged);
}

std::vector<std::vector<ImVec2>>
BuildGlyphFillPolygons(const ImportedTextGlyph &glyph) {
  struct OuterPolygon {
    std::vector<ImVec2> outline;
    std::vector<std::vector<ImVec2>> holes;
  };

  std::vector<OuterPolygon> outers;
  std::vector<std::vector<ImVec2>> holes;
  std::vector<ImVec2> flattened;

  for (const ImportedTextContour &contour : glyph.contours) {
    if (!contour.closed) {
      continue;
    }
    FlattenImportedTextContour(contour, &flattened);
    if (flattened.size() < 3) {
      continue;
    }

    if (contour.role == ImportedTextContourRole::Hole) {
      EnsureClockwise(&flattened);
      holes.push_back(flattened);
      continue;
    }

    EnsureCounterClockwise(&flattened);
    outers.push_back(OuterPolygon{flattened, {}});
  }

  for (const std::vector<ImVec2> &hole : holes) {
    bool assigned = false;
    for (OuterPolygon &outer : outers) {
      if (!outer.outline.empty() &&
          PointInPolygon(outer.outline, hole.front())) {
        outer.holes.push_back(hole);
        assigned = true;
        break;
      }
    }
    if (!assigned && !outers.empty()) {
      outers.front().holes.push_back(hole);
    }
  }

  std::vector<std::vector<ImVec2>> polygons;
  polygons.reserve(outers.size());
  for (OuterPolygon &outer : outers) {
    std::vector<ImVec2> polygon = outer.outline;
    for (const std::vector<ImVec2> &hole : outer.holes) {
      BridgeHoleIntoPolygon(&polygon, hole);
    }
    if (polygon.size() >= 3) {
      polygons.push_back(std::move(polygon));
    }
  }
  return polygons;
}

void AppendImportedTextContourPath(ImDrawList *draw_list,
                                   const CanvasState &state,
                                   const ImRect &canvas_rect,
                                   const ImportedArtwork &artwork,
                                   const ImportedTextContour &contour) {
  AppendImportedSegmentPath(draw_list, state, canvas_rect, artwork,
                            contour.segments, contour.closed);
}

void DrawImportedDxfText(ImDrawList *draw_list, const CanvasState &state,
                         const ImRect &canvas_rect,
                         const ImportedArtwork &artwork,
                         const ImportedDxfText &text) {
  const ImU32 packed_color = ImGui::ColorConvertFloat4ToU32(text.stroke_color);
  const float outline_thickness = kImportedPreviewStrokeWidth;

  if (!text.placeholder_only) {
    for (const ImportedTextGlyph &glyph : text.glyphs) {
      const std::vector<std::vector<ImVec2>> polygons =
          BuildGlyphFillPolygons(glyph);
      for (const std::vector<ImVec2> &local_polygon : polygons) {
        if (local_polygon.size() < 3) {
          continue;
        }

        std::vector<ImVec2> screen_polygon;
        screen_polygon.reserve(local_polygon.size());
        for (const ImVec2 &point : local_polygon) {
          const ImVec2 world = ImportedArtworkPointToWorld(artwork, point);
          screen_polygon.push_back(
              WorldToScreen(state, canvas_rect.Min, world));
        }
        draw_list->AddConcavePolyFilled(screen_polygon.data(),
                                        static_cast<int>(screen_polygon.size()),
                                        packed_color);
      }
    }
  }

  for (const ImportedTextGlyph &glyph : text.glyphs) {
    for (const ImportedTextContour &contour : glyph.contours) {
      AppendImportedTextContourPath(draw_list, state, canvas_rect, artwork,
                                    contour);
      draw_list->PathStroke(packed_color, contour.closed, outline_thickness);
    }
  }

  for (const ImportedTextContour &contour : text.placeholder_contours) {
    AppendImportedTextContourPath(draw_list, state, canvas_rect, artwork,
                                  contour);
    draw_list->PathStroke(packed_color, contour.closed,
                          std::max(1.0f, text.stroke_width));
  }
}

void DrawGrid(ImDrawList *draw_list, const CanvasState &state,
              const ImRect &canvas_rect) {
  if (!state.grid.visible || state.grid.spacing <= 0.0f ||
      state.grid.subdivisions <= 0) {
    return;
  }

  const float major_spacing_world =
      UnitsToPixels(state.grid.spacing, state.grid.unit, state.calibration);
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

void DrawImportedArtwork(ImDrawList *draw_list, const CanvasState &state,
                         const ImRect &canvas_rect,
                         int selected_imported_artwork_id,
                         const CanvasWidgetOptions &options) {
  draw_list->PushClipRect(canvas_rect.Min, canvas_rect.Max, true);

  const ImU32 selected_color =
      ImGui::ColorConvertFloat4ToU32(state.theme.working_area_selected);
  const ImU32 element_selection_color =
      ImGui::ColorConvertFloat4ToU32(state.theme.guide_hovered);
  const ImU32 operation_issue_color =
      ThemeColorToU32(state.theme.operation_issue);
  const bool show_group_selection_bounds =
      IsCanvasArtworkScope(state) &&
      CountSelectedImportedArtworkObjects(state) > 1;

  for (const ImportedArtwork &artwork : state.imported_artwork) {
    if (!artwork.visible) {
      continue;
    }

    const bool artwork_selected =
        IsImportedArtworkObjectSelected(state, artwork.id);
    const bool show_issue_overlays =
        artwork_selected || artwork.id == selected_imported_artwork_id ||
        state.selected_imported_debug.artwork_id == artwork.id;

    for (const ImportedDxfText &text : artwork.dxf_text) {
      if (text.placeholder_only && artwork.source_format == "DXF" &&
          !state.show_imported_dxf_text) {
        continue;
      }
      DrawImportedDxfText(draw_list, state, canvas_rect, artwork, text);
      const bool has_visible_issue_overlay =
          show_issue_overlays &&
          ImportedIssueOverlayVisible(state.imported_issue_overlays,
                                      text.issue_flags);
      if (has_visible_issue_overlay) {
        const ImRect text_rect = ImportedElementScreenRect(
            state, canvas_rect.Min, artwork, text.bounds_min, text.bounds_max);
        draw_list->AddRect(
            text_rect.Min, text_rect.Max,
            ImportedIssueOverlayColor(
                state.theme, state.imported_issue_overlays, text.issue_flags),
            3.0f, 0, 2.0f);
      }
      if (state.highlight_last_imported_operation_issue_elements &&
          show_issue_overlays && !has_visible_issue_overlay &&
          IsLastOperationIssueElement(state, artwork.id,
                                      ImportedElementKind::DxfText, text.id)) {
        const ImRect text_rect = ImportedElementScreenRect(
            state, canvas_rect.Min, artwork, text.bounds_min, text.bounds_max);
        draw_list->AddRect(text_rect.Min, text_rect.Max, operation_issue_color,
                           5.0f, 0, 2.0f);
      }
      if (IsImportedElementSelected(state, artwork.id,
                                    ImportedElementKind::DxfText, text.id)) {
        const ImRect text_rect = ImportedElementScreenRect(
            state, canvas_rect.Min, artwork, text.bounds_min, text.bounds_max);
        draw_list->AddRect(text_rect.Min, text_rect.Max,
                           element_selection_color, 3.0f, 0, 2.0f);
      }
    }

    for (const ImportedPath &path : artwork.paths) {
      if (path.segments.empty()) {
        continue;
      }

      const auto append_path = [&]() {
        AppendImportedSegmentPath(draw_list, state, canvas_rect, artwork,
                                  path.segments, path.closed);
      };

      const bool is_filled_text =
          HasImportedPathFlag(path.flags, ImportedPathFlagFilledText);
      const bool is_hole_contour =
          HasImportedPathFlag(path.flags, ImportedPathFlagHoleContour);
      const float thickness = kImportedPreviewStrokeWidth;
      const ImU32 packed_color =
          ImGui::ColorConvertFloat4ToU32(path.stroke_color);
      const ImU32 background_color =
          ImGui::ColorConvertFloat4ToU32(state.theme.canvas_background);

      append_path();
      if (is_filled_text && path.closed && !is_hole_contour) {
        draw_list->PathFillConcave(packed_color);
        append_path();
      }
      if (is_hole_contour && path.closed) {
        draw_list->PathFillConcave(background_color);
      } else {
        draw_list->PathStroke(packed_color, path.closed,
                              is_filled_text ? std::max(1.0f, thickness * 0.35f)
                                             : thickness);
      }

      const bool has_visible_issue_overlay =
          show_issue_overlays &&
          ImportedIssueOverlayVisible(state.imported_issue_overlays,
                                      path.issue_flags);
      if (has_visible_issue_overlay) {
        const ImRect path_rect = ImportedElementScreenRect(
            state, canvas_rect.Min, artwork, path.bounds_min, path.bounds_max);
        draw_list->AddRect(
            path_rect.Min, path_rect.Max,
            ImportedIssueOverlayColor(
                state.theme, state.imported_issue_overlays, path.issue_flags),
            3.0f, 0, 2.0f);
      }
      if (state.highlight_last_imported_operation_issue_elements &&
          show_issue_overlays && !has_visible_issue_overlay &&
          IsLastOperationIssueElement(state, artwork.id,
                                      ImportedElementKind::Path, path.id)) {
        const ImRect path_rect = ImportedElementScreenRect(
            state, canvas_rect.Min, artwork, path.bounds_min, path.bounds_max);
        draw_list->AddRect(path_rect.Min, path_rect.Max, operation_issue_color,
                           5.0f, 0, 2.0f);
      }

      if (IsImportedElementSelected(state, artwork.id,
                                    ImportedElementKind::Path, path.id)) {
        const ImRect path_rect = ImportedElementScreenRect(
            state, canvas_rect.Min, artwork, path.bounds_min, path.bounds_max);
        draw_list->AddRect(path_rect.Min, path_rect.Max,
                           element_selection_color, 3.0f, 0, 2.0f);
      }
    }

    ImRect screen_rect;
    const bool has_debug_focus = TryGetImportedDebugScreenRect(
        state, canvas_rect.Min, artwork, &screen_rect);
    if (!show_group_selection_bounds && (artwork_selected || has_debug_focus)) {
      if (!has_debug_focus) {
        screen_rect =
            ImportedArtworkScreenRect(state, canvas_rect.Min, artwork);
      }
      draw_list->AddRect(screen_rect.Min, screen_rect.Max, selected_color, 4.0f,
                         0, 2.0f);
      if ((!has_debug_focus || state.selected_imported_debug.kind ==
                                   ImportedDebugSelectionKind::Artwork) &&
          HasImportedArtworkFlag(artwork.flags, ImportedArtworkFlagResizable)) {
        const ImRect handle_rect(
            ImVec2(screen_rect.Max.x - options.resize_handle_size,
                   screen_rect.Max.y - options.resize_handle_size),
            screen_rect.Max);
        draw_list->AddRectFilled(handle_rect.Min, handle_rect.Max,
                                 selected_color, 2.0f);
      }
    }
  }

  if (show_group_selection_bounds) {
    ImRect selection_screen_rect;
    if (TryGetSelectedImportedArtworkScreenRect(state, canvas_rect.Min,
                                                &selection_screen_rect)) {
      draw_list->AddRect(selection_screen_rect.Min, selection_screen_rect.Max,
                         selected_color, 4.0f, 0, 2.0f);
      const ImRect handle_rect = ImportedArtworkResizeHandleRect(
          selection_screen_rect, options.resize_handle_size);
      draw_list->AddRectFilled(handle_rect.Min, handle_rect.Max, selected_color,
                               2.0f);
    }
  }

  draw_list->PopClipRect();
}

void DrawImportedMarquee(ImDrawList *draw_list, const CanvasState &state,
                         const ImRect &canvas_rect,
                         const TransientCanvasState &transient_state) {
  if (!IsMarqueeInteractionSelecting(transient_state)) {
    return;
  }

  const ImRect marquee_world_rect(
      ImVec2(std::min(transient_state.marquee_start_world.x,
                      transient_state.marquee_end_world.x),
             std::min(transient_state.marquee_start_world.y,
                      transient_state.marquee_end_world.y)),
      ImVec2(std::max(transient_state.marquee_start_world.x,
                      transient_state.marquee_end_world.x),
             std::max(transient_state.marquee_start_world.y,
                      transient_state.marquee_end_world.y)));
  const ImRect marquee_screen_rect =
      WorldRectToScreenRect(state, canvas_rect.Min, marquee_world_rect);

  const float w = marquee_screen_rect.Max.x - marquee_screen_rect.Min.x;
  const float h = marquee_screen_rect.Max.y - marquee_screen_rect.Min.y;
  if (w < 1.0f && h < 1.0f) {
    return;
  }

  draw_list->PushClipRect(canvas_rect.Min, canvas_rect.Max, true);

  const ImU32 outline_color =
      ImGui::ColorConvertFloat4ToU32(state.theme.guide_hovered);
  const ImU32 fill_color = ImGui::ColorConvertFloat4ToU32(
      ImVec4(state.theme.guide_hovered.x, state.theme.guide_hovered.y,
             state.theme.guide_hovered.z, kMarqueeFillAlpha));

  if (state.imported_artwork_edit_mode == ImportedArtworkEditMode::SelectOval) {
    const ImVec2 center(
        (marquee_screen_rect.Min.x + marquee_screen_rect.Max.x) * 0.5f,
        (marquee_screen_rect.Min.y + marquee_screen_rect.Max.y) * 0.5f);
    const ImVec2 radius(
        (marquee_screen_rect.Max.x - marquee_screen_rect.Min.x) * 0.5f,
        (marquee_screen_rect.Max.y - marquee_screen_rect.Min.y) * 0.5f);
    draw_list->AddEllipseFilled(center, radius, fill_color);
    draw_list->AddEllipse(center, radius, outline_color, 0.0f, 0,
                          kMarqueeOutlineThickness);
    draw_list->PopClipRect();
    return;
  }

  draw_list->AddRectFilled(marquee_screen_rect.Min, marquee_screen_rect.Max,
                           fill_color, 3.0f);
  draw_list->AddRect(marquee_screen_rect.Min, marquee_screen_rect.Max,
                     outline_color, 3.0f, 0, kMarqueeOutlineThickness);
  draw_list->PopClipRect();
}

void DrawWorkingAreas(ImDrawList *draw_list, const CanvasState &state,
                      const ImRect &canvas_rect, int selected_working_area_id,
                      const CanvasWidgetOptions &options) {
  draw_list->PushClipRect(canvas_rect.Min, canvas_rect.Max, true);

  const ImU32 fill_color =
      ImGui::ColorConvertFloat4ToU32(state.theme.working_area_fill);
  const ImU32 exclusion_fill =
      ImGui::ColorConvertFloat4ToU32(state.theme.exclusion_area_fill);
  const ImU32 exclusion_outline =
      ImGui::ColorConvertFloat4ToU32(state.theme.exclusion_area_outline);
  const ImU32 exclusion_selected =
      ImGui::ColorConvertFloat4ToU32(state.theme.exclusion_area_selected);

  for (const WorkingArea &area : state.working_areas) {
    if (!area.visible) {
      continue;
    }

    const ImRect screen_rect =
        WorkingAreaScreenRect(state, canvas_rect.Min, area);
    const bool selected = area.id == selected_working_area_id;
    const ImU32 active_border = ImGui::ColorConvertFloat4ToU32(
        selected ? area.selected_border_color : area.border_color);
    const float thickness =
        selected ? area.selected_outline_thickness : area.outline_thickness;

    constexpr float corner_radius = 0.0f;
    draw_list->AddRectFilled(screen_rect.Min, screen_rect.Max, fill_color,
                             corner_radius);
    draw_list->AddRect(screen_rect.Min, screen_rect.Max, active_border,
                       corner_radius, 0, thickness);
    draw_list->AddText(
        ImVec2(screen_rect.Min.x + 8.0f, screen_rect.Min.y + 8.0f),
        active_border, area.name.c_str());

    constexpr float handle_radius = 2.0f;
    if (selected && HasWorkingAreaFlag(area.flags, WorkingAreaFlagResizable)) {
      const ImRect handle_rect(
          ImVec2(screen_rect.Max.x - options.resize_handle_size,
                 screen_rect.Max.y - options.resize_handle_size),
          screen_rect.Max);
      draw_list->AddRectFilled(handle_rect.Min, handle_rect.Max, active_border,
                               handle_radius);
    }
  }

  for (const ExportArea &area : state.export_areas) {
    if (!area.visible) {
      continue;
    }

    const ImRect screen_rect(
        WorldToScreen(state, canvas_rect.Min, area.origin),
        WorldToScreen(
            state, canvas_rect.Min,
            ImVec2(area.origin.x + area.size.x, area.origin.y + area.size.y)));
    if (!area.hide_fill) {
      draw_list->AddRectFilled(screen_rect.Min, screen_rect.Max,
                               ImGui::ColorConvertFloat4ToU32(area.fill_color),
                               0.0f);
    }
    draw_list->AddRect(screen_rect.Min, screen_rect.Max,
                       ImGui::ColorConvertFloat4ToU32(area.outline_color), 0.0f,
                       0, 1.0f);
  }

  for (const ExclusionArea &area : state.exclusion_areas) {
    if (!area.visible) {
      continue;
    }

    const ImRect screen_rect(
        WorldToScreen(state, canvas_rect.Min, area.origin),
        WorldToScreen(
            state, canvas_rect.Min,
            ImVec2(area.origin.x + area.size.x, area.origin.y + area.size.y)));
    if (!area.hide_fill) {
      draw_list->AddRectFilled(screen_rect.Min, screen_rect.Max, exclusion_fill,
                               0.0f);
    }
    draw_list->AddRect(screen_rect.Min, screen_rect.Max,
                       area.selected ? exclusion_selected : exclusion_outline,
                       0.0f, 0, area.selected ? 2.0f : 1.0f);
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
      UnitsToPixels(1.0f, state.ruler_unit, state.calibration) *
      state.view.zoom;
  if (pixels_per_unit <= 0.0f) {
    return;
  }

  const float major_step_units = NiceStep(80.0f / pixels_per_unit);
  const float minor_step_units = major_step_units / 5.0f;
  const float major_step_world =
      UnitsToPixels(major_step_units, state.ruler_unit, state.calibration);
  const float minor_step_world =
      UnitsToPixels(minor_step_units, state.ruler_unit, state.calibration);

  if (minor_step_world <= 0.0f || major_step_world <= 0.0f) {
    return;
  }

  draw_list->PushClipRect(rect.Min, rect.Max, true);

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
      PixelsToUnits(TransformRulerWorld(state, visible_min_world, horizontal),
                    state.ruler_unit, state.calibration);
  const float max_units =
      PixelsToUnits(TransformRulerWorld(state, visible_max_world, horizontal),
                    state.ruler_unit, state.calibration);
  const float start_units =
      std::floor(std::min(min_units, max_units) / minor_step_units) *
      minor_step_units;
  const float end_units =
      std::ceil(std::max(min_units, max_units) / minor_step_units) *
      minor_step_units;

  for (float tick_units = start_units; tick_units <= end_units;
       tick_units += minor_step_units) {
    const float display_tick_world =
        UnitsToPixels(tick_units, state.ruler_unit, state.calibration);
    const float tick_world =
        InverseTransformRulerWorld(state, display_tick_world, horizontal);
    const float remainder = std::fmod(std::fabs(tick_units), major_step_units);
    const bool major = remainder < 0.0001f ||
                       std::fabs(remainder - major_step_units) < 0.0001f;
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
        const std::string label =
            FormatRulerTickLabel(tick_units, major_step_units);
        draw_list->AddText(ImVec2(x + 4.0f, rect.Min.y + 4.0f), text_color,
                           label.c_str());
      }
    } else {
      const float y = WorldToScreen(state, ImVec2(rect.Max.x, rect.Min.y),
                                    ImVec2(0.0f, tick_world))
                          .y;
      draw_list->AddLine(ImVec2(rect.Max.x, y),
                         ImVec2(rect.Max.x - tick_length, y), tick_color, 1.0f);
      if (major) {
        const std::string label =
            FormatRulerTickLabel(tick_units, major_step_units);
        const float label_width = ImGui::CalcTextSize(label.c_str()).x;
        const float label_x =
            std::max(rect.Min.x + 4.0f, rect.Max.x - label_width - 4.0f);
        draw_list->AddText(ImVec2(label_x, y + 2.0f), text_color,
                           label.c_str());
      }
    }
  }

  draw_list->PopClipRect();
}

} // namespace im2d::detail
