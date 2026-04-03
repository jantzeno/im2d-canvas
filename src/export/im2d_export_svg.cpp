#include "im2d_export_svg.h"

#include "../canvas/im2d_canvas_document.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace im2d::exporter {
namespace {

struct WorldRect {
  ImVec2 min = ImVec2(0.0f, 0.0f);
  ImVec2 max = ImVec2(0.0f, 0.0f);
  bool valid = false;
};

struct ExportItem {
  const ImportedArtwork *artwork = nullptr;
  const ImportedPath *path = nullptr;
  const ImportedDxfText *text = nullptr;
  WorldRect world_bounds;
};

struct ExportArtworkSelection {
  const ImportedArtwork *artwork = nullptr;
  std::unordered_set<int> group_ids;
  std::unordered_set<int> path_ids;
  std::unordered_set<int> text_ids;
};

struct ExportPlan {
  std::vector<ExportArtworkSelection> artwork_selections;
  std::vector<ExportItem> items;
};

std::string FormatNumber(float value) {
  std::ostringstream stream;
  stream << std::fixed << std::setprecision(6) << value;
  std::string text = stream.str();
  const size_t decimal = text.find('.');
  if (decimal == std::string::npos) {
    return text;
  }
  size_t trim = text.size();
  while (trim > decimal + 1 && text[trim - 1] == '0') {
    --trim;
  }
  if (trim > decimal && text[trim - 1] == '.') {
    --trim;
  }
  text.resize(trim);
  if (text.empty() || text == "-0") {
    return "0";
  }
  return text;
}

std::string ColorToHex(const ImVec4 &color) {
  const auto to_channel = [](float value) {
    return std::clamp(static_cast<int>(std::lround(value * 255.0f)), 0, 255);
  };

  std::ostringstream stream;
  stream << '#' << std::hex << std::nouppercase << std::setfill('0')
         << std::setw(2) << to_channel(color.x) << std::setw(2)
         << to_channel(color.y) << std::setw(2) << to_channel(color.z);
  return stream.str();
}

std::string EscapeXml(std::string_view value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (const char character : value) {
    switch (character) {
    case '&':
      escaped += "&amp;";
      break;
    case '<':
      escaped += "&lt;";
      break;
    case '>':
      escaped += "&gt;";
      break;
    case '"':
      escaped += "&quot;";
      break;
    case '\'':
      escaped += "&apos;";
      break;
    default:
      escaped.push_back(character);
      break;
    }
  }
  return escaped;
}

WorldRect MakeRect(const ImVec2 &a, const ImVec2 &b) {
  WorldRect rect;
  rect.min = ImVec2(std::min(a.x, b.x), std::min(a.y, b.y));
  rect.max = ImVec2(std::max(a.x, b.x), std::max(a.y, b.y));
  rect.valid = true;
  return rect;
}

WorldRect PathWorldBounds(const ImportedArtwork &artwork,
                          const ImportedPath &path) {
  ImVec2 world_min;
  ImVec2 world_max;
  ImportedLocalBoundsToWorldBounds(artwork, path.bounds_min, path.bounds_max,
                                   &world_min, &world_max);
  return MakeRect(world_min, world_max);
}

WorldRect TextWorldBounds(const ImportedArtwork &artwork,
                          const ImportedDxfText &text) {
  ImVec2 world_min;
  ImVec2 world_max;
  ImportedLocalBoundsToWorldBounds(artwork, text.bounds_min, text.bounds_max,
                                   &world_min, &world_max);
  return MakeRect(world_min, world_max);
}

void ExpandRect(WorldRect *bounds, const WorldRect &other) {
  if (!other.valid) {
    return;
  }
  if (!bounds->valid) {
    *bounds = other;
    return;
  }
  bounds->min.x = std::min(bounds->min.x, other.min.x);
  bounds->min.y = std::min(bounds->min.y, other.min.y);
  bounds->max.x = std::max(bounds->max.x, other.max.x);
  bounds->max.y = std::max(bounds->max.y, other.max.y);
}

bool RectsIntersect(const WorldRect &a, const WorldRect &b) {
  if (!a.valid || !b.valid) {
    return false;
  }
  return !(a.max.x < b.min.x || a.min.x > b.max.x || a.max.y < b.min.y ||
           a.min.y > b.max.y);
}

constexpr float kSvgPreviewStrokeWidth = 1.0f;

void AddWarning(SvgExportResult *result, std::string message) {
  if (result == nullptr || message.empty()) {
    return;
  }
  result->warnings.push_back(std::move(message));
}

void PreflightExportDiagnostics(const std::vector<ExportItem> &items,
                                SvgExportResult *result) {
  if (result == nullptr) {
    return;
  }

  for (const ExportItem &item : items) {
    if (item.path != nullptr) {
      if (HasImportedElementIssueFlag(item.path->issue_flags,
                                      ImportedElementIssueFlagOpenGeometry)) {
        result->open_geometry_item_count += 1;
      }
      continue;
    }

    if (item.text == nullptr) {
      continue;
    }

    result->text_count += 1;
    if (item.text->placeholder_only) {
      result->placeholder_text_count += 1;
    }
    if (item.text->substituted_font) {
      result->substituted_font_text_count += 1;
    }
    if (HasImportedElementIssueFlag(item.text->issue_flags,
                                    ImportedElementIssueFlagOpenGeometry)) {
      result->open_geometry_item_count += 1;
    }
  }
}

void FinalizeExportWarnings(SvgExportResult *result,
                            bool placeholder_text_blocked) {
  if (result == nullptr) {
    return;
  }

  if (result->placeholder_text_count > 0) {
    if (placeholder_text_blocked) {
      AddWarning(result,
                 std::to_string(result->placeholder_text_count) +
                     " DXF text item(s) are placeholder-only; strict SVG "
                     "export is blocked because only guide bounds are "
                     "available.");
    } else {
      AddWarning(result, std::to_string(result->placeholder_text_count) +
                             " DXF text item(s) are placeholder-only; exported "
                             "contours are guide bounds rather than true glyph "
                             "outlines.");
    }
  }
  if (result->substituted_font_text_count > 0) {
    AddWarning(result,
               std::to_string(result->substituted_font_text_count) +
                   " DXF text item(s) used substituted fonts; exported glyph "
                   "shape may differ from the source drawing.");
  }
  if (result->open_geometry_item_count > 0) {
    AddWarning(result, std::to_string(result->open_geometry_item_count) +
                           " exported item(s) still report open geometry and "
                           "may not be cut-ready.");
  }
  result->warning_count = static_cast<int>(result->warnings.size());
}

bool WriteSvgFile(const std::filesystem::path &output_path,
                  std::string_view contents, std::string *error_message) {
  std::error_code error;
  const std::filesystem::path parent_path = output_path.parent_path();
  if (!parent_path.empty()) {
    std::filesystem::create_directories(parent_path, error);
    if (error) {
      if (error_message != nullptr) {
        *error_message =
            "Failed to create export directory: " + parent_path.string() +
            " (" + error.message() + ")";
      }
      return false;
    }
  }

  std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
  if (!output.is_open()) {
    if (error_message != nullptr) {
      *error_message = "Failed to open export file: " + output_path.string();
    }
    return false;
  }

  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  output.close();
  if (!output) {
    if (error_message != nullptr) {
      *error_message = "Failed to write export file: " + output_path.string();
    }
    return false;
  }
  return true;
}

std::string PathRoleName(ImportedTextContourRole role) {
  switch (role) {
  case ImportedTextContourRole::Outline:
    return "outline";
  case ImportedTextContourRole::Hole:
    return "hole";
  case ImportedTextContourRole::Guide:
    return "guide";
  }
  return "outline";
}

void AppendSvgPathData(std::ostringstream &path_data,
                       const ImportedArtwork &artwork,
                       const std::vector<ImportedPathSegment> &segments,
                       const ImVec2 &origin_offset, int *line_segment_count,
                       int *cubic_segment_count) {
  if (segments.empty()) {
    return;
  }

  const ImVec2 first_world =
      ImportedArtworkPointToWorld(artwork, segments.front().start);
  path_data << 'M' << FormatNumber(first_world.x - origin_offset.x) << ' '
            << FormatNumber(first_world.y - origin_offset.y);
  for (const ImportedPathSegment &segment : segments) {
    const ImVec2 end_world = ImportedArtworkPointToWorld(artwork, segment.end);
    if (segment.kind == ImportedPathSegmentKind::Line) {
      path_data << " L" << FormatNumber(end_world.x - origin_offset.x) << ' '
                << FormatNumber(end_world.y - origin_offset.y);
      if (line_segment_count != nullptr) {
        *line_segment_count += 1;
      }
      continue;
    }

    const ImVec2 control1_world =
        ImportedArtworkPointToWorld(artwork, segment.control1);
    const ImVec2 control2_world =
        ImportedArtworkPointToWorld(artwork, segment.control2);
    path_data << " C" << FormatNumber(control1_world.x - origin_offset.x) << ' '
              << FormatNumber(control1_world.y - origin_offset.y) << ' '
              << FormatNumber(control2_world.x - origin_offset.x) << ' '
              << FormatNumber(control2_world.y - origin_offset.y) << ' '
              << FormatNumber(end_world.x - origin_offset.x) << ' '
              << FormatNumber(end_world.y - origin_offset.y);
    if (cubic_segment_count != nullptr) {
      *cubic_segment_count += 1;
    }
  }
}

void AppendSerializedPath(std::ostringstream &svg,
                          const ImportedArtwork &artwork,
                          const ImportedPath &path, const ImVec2 &origin_offset,
                          SvgExportResult *result, int indent_level) {
  if (path.segments.empty()) {
    return;
  }

  std::ostringstream path_data;
  AppendSvgPathData(path_data, artwork, path.segments, origin_offset,
                    &result->line_segment_count, &result->cubic_segment_count);
  if (path.closed) {
    path_data << " Z";
  }

  svg << std::string(static_cast<size_t>(indent_level) * 2, ' ') << "<path d=\""
      << path_data.str() << "\" fill=\"none\""
      << " stroke=\"" << ColorToHex(path.stroke_color) << "\""
      << " stroke-width=\"" << FormatNumber(kSvgPreviewStrokeWidth)
      << "\" vector-effect=\"non-scaling-stroke\""
      << " data-im2d-kind=\"path\""
      << " data-im2d-source-artwork-id=\"" << artwork.id << "\""
      << " data-im2d-parent-group-id=\"" << path.parent_group_id << "\""
      << " data-im2d-source-item-id=\"" << path.id << "\"";
  if (!path.label.empty()) {
    svg << " id=\"path-" << artwork.id << '-' << path.id << "\""
        << " data-im2d-label=\"" << EscapeXml(path.label) << "\"";
  }
  svg << " />\n";
  result->path_count += 1;
}

void AppendSerializedTextContour(std::ostringstream &svg,
                                 const ImportedArtwork &artwork,
                                 const ImportedDxfText &text,
                                 const ImportedTextContour &contour,
                                 const ImVec2 &origin_offset, bool placeholder,
                                 SvgExportResult *result, int indent_level) {
  if (contour.segments.empty()) {
    return;
  }

  std::ostringstream path_data;
  AppendSvgPathData(path_data, artwork, contour.segments, origin_offset,
                    &result->line_segment_count, &result->cubic_segment_count);
  if (contour.closed) {
    path_data << " Z";
  }

  svg << std::string(static_cast<size_t>(indent_level) * 2, ' ') << "<path d=\""
      << path_data.str() << "\" fill=\"none\""
      << " stroke=\"" << ColorToHex(text.stroke_color) << "\""
      << " stroke-width=\"" << FormatNumber(kSvgPreviewStrokeWidth)
      << "\" vector-effect=\"non-scaling-stroke\""
      << " data-im2d-kind=\"dxf-text\""
      << " data-im2d-source-artwork-id=\"" << artwork.id << "\""
      << " data-im2d-parent-group-id=\"" << text.parent_group_id << "\""
      << " data-im2d-source-item-id=\"" << text.id << "\""
      << " data-im2d-text-role=\"" << PathRoleName(contour.role) << "\"";
  if (placeholder) {
    svg << " data-im2d-placeholder=\"true\"";
  }
  if (text.substituted_font) {
    svg << " data-im2d-font-substituted=\"true\"";
  }
  if (!text.source_text.empty()) {
    svg << " data-im2d-source-text=\"" << EscapeXml(text.source_text) << "\"";
  }
  if (!text.requested_font_file.empty()) {
    svg << " data-im2d-requested-font=\"" << EscapeXml(text.requested_font_file)
        << "\"";
  }
  if (!text.resolved_font_path.empty()) {
    svg << " data-im2d-resolved-font=\"" << EscapeXml(text.resolved_font_path)
        << "\"";
  }
  if (!text.label.empty()) {
    svg << " data-im2d-label=\"" << EscapeXml(text.label) << "\"";
  }
  svg << " />\n";
}

ExportArtworkSelection *
FindOrAppendArtworkSelection(std::vector<ExportArtworkSelection> *selections,
                             const ImportedArtwork *artwork) {
  if (selections == nullptr || artwork == nullptr) {
    return nullptr;
  }
  auto it = std::find_if(selections->begin(), selections->end(),
                         [artwork](const ExportArtworkSelection &selection) {
                           return selection.artwork == artwork;
                         });
  if (it != selections->end()) {
    return &(*it);
  }
  selections->push_back({});
  selections->back().artwork = artwork;
  return &selections->back();
}

void CollectAncestorGroupIdsForExport(const ImportedArtwork &artwork,
                                      int group_id,
                                      std::unordered_set<int> *group_ids) {
  int current_group_id = group_id;
  while (current_group_id != 0 && group_ids != nullptr &&
         group_ids->insert(current_group_id).second) {
    const ImportedGroup *group = FindImportedGroup(artwork, current_group_id);
    if (group == nullptr) {
      break;
    }
    current_group_id = group->parent_group_id;
  }
}

void AddPathToExportSelection(ExportArtworkSelection *selection,
                              std::vector<ExportItem> *items,
                              WorldRect *world_bounds, int path_id) {
  if (selection == nullptr || selection->artwork == nullptr) {
    return;
  }
  if (!selection->path_ids.insert(path_id).second) {
    return;
  }

  const ImportedPath *path = FindImportedPath(*selection->artwork, path_id);
  if (path == nullptr || path->segments.empty()) {
    selection->path_ids.erase(path_id);
    return;
  }

  CollectAncestorGroupIdsForExport(*selection->artwork, path->parent_group_id,
                                   &selection->group_ids);
  const WorldRect bounds = PathWorldBounds(*selection->artwork, *path);
  if (items != nullptr) {
    items->push_back({selection->artwork, path, nullptr, bounds});
  }
  if (world_bounds != nullptr) {
    ExpandRect(world_bounds, bounds);
  }
}

void AddTextToExportSelection(ExportArtworkSelection *selection,
                              std::vector<ExportItem> *items,
                              WorldRect *world_bounds, int text_id) {
  if (selection == nullptr || selection->artwork == nullptr) {
    return;
  }
  if (!selection->text_ids.insert(text_id).second) {
    return;
  }

  const ImportedDxfText *text =
      FindImportedDxfText(*selection->artwork, text_id);
  if (text == nullptr) {
    selection->text_ids.erase(text_id);
    return;
  }

  CollectAncestorGroupIdsForExport(*selection->artwork, text->parent_group_id,
                                   &selection->group_ids);
  const WorldRect bounds = TextWorldBounds(*selection->artwork, *text);
  if (items != nullptr) {
    items->push_back({selection->artwork, nullptr, text, bounds});
  }
  if (world_bounds != nullptr) {
    ExpandRect(world_bounds, bounds);
  }
}

void AddGroupSubtreeToExportSelection(ExportArtworkSelection *selection,
                                      std::vector<ExportItem> *items,
                                      WorldRect *world_bounds, int group_id) {
  if (selection == nullptr || selection->artwork == nullptr) {
    return;
  }

  const ImportedGroup *group = FindImportedGroup(*selection->artwork, group_id);
  if (group == nullptr) {
    return;
  }

  selection->group_ids.insert(group->id);
  for (const int child_group_id : group->child_group_ids) {
    AddGroupSubtreeToExportSelection(selection, items, world_bounds,
                                     child_group_id);
  }
  for (const int path_id : group->path_ids) {
    AddPathToExportSelection(selection, items, world_bounds, path_id);
  }
  for (const int text_id : group->dxf_text_ids) {
    AddTextToExportSelection(selection, items, world_bounds, text_id);
  }
}

bool HasSerializableSelection(const ExportPlan &plan) {
  return std::any_of(
      plan.artwork_selections.begin(), plan.artwork_selections.end(),
      [](const ExportArtworkSelection &selection) {
        return !selection.path_ids.empty() || !selection.text_ids.empty();
      });
}

std::optional<ExportPlan> GatherSelectionItems(const CanvasState &state,
                                               const SvgExportRequest &request,
                                               SvgExportResult *result,
                                               WorldRect *world_bounds) {
  const int artwork_id = request.imported_artwork_id != 0
                             ? request.imported_artwork_id
                             : state.selected_imported_artwork_id;
  const ImportedArtwork *artwork = FindImportedArtwork(state, artwork_id);
  if (artwork == nullptr) {
    result->message = "Selection export requires a selected imported artwork.";
    return std::nullopt;
  }
  ExportPlan plan;
  ExportArtworkSelection *selection =
      FindOrAppendArtworkSelection(&plan.artwork_selections, artwork);
  if (selection == nullptr) {
    result->message = "Selection export could not initialize export state.";
    return std::nullopt;
  }

  if (!state.selected_imported_elements.empty() &&
      state.selected_imported_artwork_id == artwork->id) {
    plan.items.reserve(state.selected_imported_elements.size());
    for (const ImportedElementSelection &selected_element :
         state.selected_imported_elements) {
      if (selected_element.kind == ImportedElementKind::Path) {
        AddPathToExportSelection(selection, &plan.items, world_bounds,
                                 selected_element.item_id);
      } else {
        AddTextToExportSelection(selection, &plan.items, world_bounds,
                                 selected_element.item_id);
      }
    }
  } else if (state.selected_imported_debug.artwork_id == artwork->id) {
    switch (state.selected_imported_debug.kind) {
    case ImportedDebugSelectionKind::Artwork:
      if (artwork->root_group_id != 0) {
        AddGroupSubtreeToExportSelection(selection, &plan.items, world_bounds,
                                         artwork->root_group_id);
      } else {
        for (const ImportedPath &path : artwork->paths) {
          AddPathToExportSelection(selection, &plan.items, world_bounds,
                                   path.id);
        }
        for (const ImportedDxfText &text : artwork->dxf_text) {
          AddTextToExportSelection(selection, &plan.items, world_bounds,
                                   text.id);
        }
      }
      break;
    case ImportedDebugSelectionKind::Group:
      CollectAncestorGroupIdsForExport(*artwork,
                                       state.selected_imported_debug.item_id,
                                       &selection->group_ids);
      AddGroupSubtreeToExportSelection(selection, &plan.items, world_bounds,
                                       state.selected_imported_debug.item_id);
      break;
    case ImportedDebugSelectionKind::Path:
      AddPathToExportSelection(selection, &plan.items, world_bounds,
                               state.selected_imported_debug.item_id);
      break;
    case ImportedDebugSelectionKind::DxfText:
      AddTextToExportSelection(selection, &plan.items, world_bounds,
                               state.selected_imported_debug.item_id);
      break;
    case ImportedDebugSelectionKind::None:
      break;
    }
  }

  if (!HasSerializableSelection(plan)) {
    result->message =
        "Selection export requires selected imported elements, a selected "
        "group, or the artwork root.";
    return std::nullopt;
  }
  return plan;
}

std::optional<ExportPlan> GatherExportAreaItems(const CanvasState &state,
                                                const SvgExportRequest &request,
                                                SvgExportResult *result,
                                                WorldRect *world_bounds) {
  const ExportArea *area = FindExportArea(state, request.export_area_id);
  if (area == nullptr) {
    result->message = "Export-area export requires at least one export area.";
    return std::nullopt;
  }

  *world_bounds = MakeRect(area->origin, ImVec2(area->origin.x + area->size.x,
                                                area->origin.y + area->size.y));
  ExportPlan plan;
  for (const ImportedArtwork &artwork : state.imported_artwork) {
    if (!artwork.visible) {
      continue;
    }

    ExportArtworkSelection *selection = nullptr;

    for (const ImportedPath &path : artwork.paths) {
      if (path.segments.empty()) {
        continue;
      }
      const WorldRect bounds = PathWorldBounds(artwork, path);
      if (RectsIntersect(*world_bounds, bounds)) {
        if (selection == nullptr) {
          selection =
              FindOrAppendArtworkSelection(&plan.artwork_selections, &artwork);
        }
        AddPathToExportSelection(selection, &plan.items, nullptr, path.id);
      }
    }

    for (const ImportedDxfText &text : artwork.dxf_text) {
      const WorldRect bounds = TextWorldBounds(artwork, text);
      if (RectsIntersect(*world_bounds, bounds)) {
        if (selection == nullptr) {
          selection =
              FindOrAppendArtworkSelection(&plan.artwork_selections, &artwork);
        }
        AddTextToExportSelection(selection, &plan.items, nullptr, text.id);
      }
    }
  }

  if (!HasSerializableSelection(plan)) {
    result->message =
        "No imported elements intersect the requested export area.";
    return std::nullopt;
  }
  return plan;
}

bool SelectionContainsGroup(const ExportArtworkSelection &selection,
                            int group_id) {
  return selection.group_ids.contains(group_id);
}

bool SelectionContainsPath(const ExportArtworkSelection &selection,
                           int path_id) {
  return selection.path_ids.contains(path_id);
}

bool SelectionContainsText(const ExportArtworkSelection &selection,
                           int text_id) {
  return selection.text_ids.contains(text_id);
}

bool TextHasSerializableContours(const ImportedDxfText &text) {
  for (const ImportedTextGlyph &glyph : text.glyphs) {
    for (const ImportedTextContour &contour : glyph.contours) {
      if (!contour.segments.empty()) {
        return true;
      }
    }
  }
  return std::any_of(text.placeholder_contours.begin(),
                     text.placeholder_contours.end(),
                     [](const ImportedTextContour &contour) {
                       return !contour.segments.empty();
                     });
}

void AppendSerializedText(std::ostringstream &svg,
                          const ImportedArtwork &artwork,
                          const ImportedDxfText &text,
                          const ImVec2 &origin_offset, SvgExportResult *result,
                          int indent_level) {
  if (!TextHasSerializableContours(text)) {
    return;
  }

  const std::string indent(static_cast<size_t>(indent_level) * 2, ' ');
  svg << indent << "<g data-im2d-kind=\"dxf-text-object\""
      << " data-im2d-source-artwork-id=\"" << artwork.id << "\""
      << " data-im2d-source-item-id=\"" << text.id << "\""
      << " data-im2d-parent-group-id=\"" << text.parent_group_id << "\"";
  if (!text.label.empty()) {
    svg << " id=\"dxf-text-" << artwork.id << '-' << text.id << "\""
        << " data-im2d-label=\"" << EscapeXml(text.label) << "\"";
  }
  if (!text.source_text.empty()) {
    svg << " data-im2d-source-text=\"" << EscapeXml(text.source_text) << "\"";
  }
  svg << ">\n";

  for (const ImportedTextGlyph &glyph : text.glyphs) {
    for (const ImportedTextContour &contour : glyph.contours) {
      AppendSerializedTextContour(svg, artwork, text, contour, origin_offset,
                                  false, result, indent_level + 1);
    }
  }
  for (const ImportedTextContour &contour : text.placeholder_contours) {
    AppendSerializedTextContour(svg, artwork, text, contour, origin_offset,
                                true, result, indent_level + 1);
  }

  svg << indent << "</g>\n";
}

void AppendSerializedGroupContents(std::ostringstream &svg,
                                   const ExportArtworkSelection &selection,
                                   const ImportedGroup &group,
                                   const ImVec2 &origin_offset,
                                   SvgExportResult *result, int indent_level);

void AppendSerializedGroup(std::ostringstream &svg,
                           const ExportArtworkSelection &selection,
                           const ImportedGroup &group,
                           const ImVec2 &origin_offset, SvgExportResult *result,
                           int indent_level) {
  const std::string indent(static_cast<size_t>(indent_level) * 2, ' ');
  svg << indent << "<g data-im2d-kind=\"group\""
      << " data-im2d-source-artwork-id=\"" << selection.artwork->id << "\""
      << " data-im2d-source-group-id=\"" << group.id << "\""
      << " data-im2d-parent-group-id=\"" << group.parent_group_id << "\"";
  if (!group.label.empty()) {
    svg << " id=\"group-" << selection.artwork->id << '-' << group.id << "\""
        << " data-im2d-label=\"" << EscapeXml(group.label) << "\"";
  }
  svg << ">\n";
  AppendSerializedGroupContents(svg, selection, group, origin_offset, result,
                                indent_level + 1);
  svg << indent << "</g>\n";
}

void AppendSerializedGroupContents(std::ostringstream &svg,
                                   const ExportArtworkSelection &selection,
                                   const ImportedGroup &group,
                                   const ImVec2 &origin_offset,
                                   SvgExportResult *result, int indent_level) {
  for (const int child_group_id : group.child_group_ids) {
    if (!SelectionContainsGroup(selection, child_group_id)) {
      continue;
    }
    const ImportedGroup *child_group =
        FindImportedGroup(*selection.artwork, child_group_id);
    if (child_group != nullptr) {
      AppendSerializedGroup(svg, selection, *child_group, origin_offset, result,
                            indent_level);
    }
  }

  for (const int path_id : group.path_ids) {
    if (!SelectionContainsPath(selection, path_id)) {
      continue;
    }
    const ImportedPath *path = FindImportedPath(*selection.artwork, path_id);
    if (path != nullptr) {
      AppendSerializedPath(svg, *selection.artwork, *path, origin_offset,
                           result, indent_level);
    }
  }

  for (const int text_id : group.dxf_text_ids) {
    if (!SelectionContainsText(selection, text_id)) {
      continue;
    }
    const ImportedDxfText *text =
        FindImportedDxfText(*selection.artwork, text_id);
    if (text != nullptr) {
      AppendSerializedText(svg, *selection.artwork, *text, origin_offset,
                           result, indent_level);
    }
  }
}

void AppendSerializedArtwork(std::ostringstream &svg,
                             const ExportArtworkSelection &selection,
                             const ImVec2 &origin_offset,
                             SvgExportResult *result) {
  if (selection.artwork == nullptr) {
    return;
  }

  svg << "  <g data-im2d-kind=\"artwork\""
      << " data-im2d-source-artwork-id=\"" << selection.artwork->id << "\"";
  if (!selection.artwork->name.empty()) {
    svg << " id=\"artwork-" << selection.artwork->id << "\""
        << " data-im2d-label=\"" << EscapeXml(selection.artwork->name) << "\"";
  }
  if (!selection.artwork->source_path.empty()) {
    svg << " data-im2d-source-path=\""
        << EscapeXml(selection.artwork->source_path) << "\"";
  }
  if (!selection.artwork->source_format.empty()) {
    svg << " data-im2d-source-format=\""
        << EscapeXml(selection.artwork->source_format) << "\"";
  }
  svg << ">\n";

  const ImportedGroup *root_group =
      FindImportedGroup(*selection.artwork, selection.artwork->root_group_id);
  if (root_group != nullptr) {
    AppendSerializedGroupContents(svg, selection, *root_group, origin_offset,
                                  result, 2);
  } else {
    for (const ImportedPath &path : selection.artwork->paths) {
      if (SelectionContainsPath(selection, path.id)) {
        AppendSerializedPath(svg, *selection.artwork, path, origin_offset,
                             result, 2);
      }
    }
    for (const ImportedDxfText &text : selection.artwork->dxf_text) {
      if (SelectionContainsText(selection, text.id)) {
        AppendSerializedText(svg, *selection.artwork, text, origin_offset,
                             result, 2);
      }
    }
  }

  svg << "  </g>\n";
}

} // namespace

SvgExportResult ExportSvg(const CanvasState &state,
                          const SvgExportRequest &request) {
  SvgExportResult result;
  WorldRect world_bounds;
  std::optional<ExportPlan> plan;
  if (request.scope == SvgExportScope::ActiveSelection) {
    plan = GatherSelectionItems(state, request, &result, &world_bounds);
  } else {
    plan = GatherExportAreaItems(state, request, &result, &world_bounds);
  }

  if (!plan.has_value() || !world_bounds.valid) {
    return result;
  }

  result.bounds_min = world_bounds.min;
  result.bounds_max = world_bounds.max;
  PreflightExportDiagnostics(plan->items, &result);
  const bool placeholder_text_blocked =
      result.placeholder_text_count > 0 && !request.allow_placeholder_text;
  FinalizeExportWarnings(&result, placeholder_text_blocked);
  if (placeholder_text_blocked) {
    result.message =
        "Export blocked: " + std::to_string(result.placeholder_text_count) +
        " DXF text item(s) are placeholder-only. Resolve the source font "
        "outlines or explicitly allow placeholder export for diagnostics.";
    return result;
  }

  const float width = std::max(0.001f, world_bounds.max.x - world_bounds.min.x);
  const float height =
      std::max(0.001f, world_bounds.max.y - world_bounds.min.y);

  std::ostringstream svg;
  svg << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
  svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" version=\"1.1\" width=\""
      << FormatNumber(width) << "\" height=\"" << FormatNumber(height)
      << "\" viewBox=\"0 0 " << FormatNumber(width) << ' '
      << FormatNumber(height) << "\" data-im2d-scope=\""
      << (request.scope == SvgExportScope::ActiveSelection ? "selection"
                                                           : "export-area")
      << "\">\n";

  for (const ExportArtworkSelection &selection : plan->artwork_selections) {
    AppendSerializedArtwork(svg, selection, world_bounds.min, &result);
  }

  if (!result.warnings.empty()) {
    svg << "  <desc>";
    for (size_t index = 0; index < result.warnings.size(); ++index) {
      if (index != 0) {
        svg << " ";
      }
      svg << EscapeXml(result.warnings[index]);
    }
    svg << "</desc>\n";
  }

  svg << "</svg>\n";
  result.success = true;
  result.svg = svg.str();
  result.message = "Exported " + std::to_string(result.path_count) +
                   " paths and " + std::to_string(result.text_count) +
                   " DXF text items to SVG";
  if (result.warning_count > 0) {
    result.message += " with " + std::to_string(result.warning_count) +
                      " warning" + (result.warning_count == 1 ? "" : "s");
  }
  result.message += ".";
  return result;
}

SvgExportResult ExportSvgToFile(const CanvasState &state,
                                const SvgExportRequest &request,
                                const std::filesystem::path &output_path) {
  SvgExportResult result = ExportSvg(state, request);
  if (!result.success) {
    return result;
  }

  std::string error_message;
  if (!WriteSvgFile(output_path, result.svg, &error_message)) {
    result.success = false;
    result.output_path.clear();
    result.message = std::move(error_message);
    return result;
  }

  result.output_path = output_path.lexically_normal().string();
  result.message += " Saved to " + result.output_path + ".";
  return result;
}

} // namespace im2d::exporter
