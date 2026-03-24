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

const ExportArea *FindExportArea(const CanvasState &state, int export_area_id) {
  if (state.export_areas.empty()) {
    return nullptr;
  }
  if (export_area_id == 0) {
    return &state.export_areas.front();
  }
  auto it = std::find_if(state.export_areas.begin(), state.export_areas.end(),
                         [export_area_id](const ExportArea &area) {
                           return area.id == export_area_id;
                         });
  return it == state.export_areas.end() ? nullptr : &(*it);
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
                          SvgExportResult *result) {
  if (path.segments.empty()) {
    return;
  }

  std::ostringstream path_data;
  AppendSvgPathData(path_data, artwork, path.segments, origin_offset,
                    &result->line_segment_count, &result->cubic_segment_count);
  if (path.closed) {
    path_data << " Z";
  }

  svg << "  <path d=\"" << path_data.str() << "\" fill=\"none\""
      << " stroke=\"" << ColorToHex(path.stroke_color) << "\""
      << " stroke-width=\"" << FormatNumber(kSvgPreviewStrokeWidth)
      << "\" vector-effect=\"non-scaling-stroke\""
      << " data-im2d-kind=\"path\""
      << " data-im2d-source-artwork-id=\"" << artwork.id << "\""
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
                                 SvgExportResult *result) {
  if (contour.segments.empty()) {
    return;
  }

  std::ostringstream path_data;
  AppendSvgPathData(path_data, artwork, contour.segments, origin_offset,
                    &result->line_segment_count, &result->cubic_segment_count);
  if (contour.closed) {
    path_data << " Z";
  }

  svg << "  <path d=\"" << path_data.str() << "\" fill=\"none\""
      << " stroke=\"" << ColorToHex(text.stroke_color) << "\""
      << " stroke-width=\"" << FormatNumber(kSvgPreviewStrokeWidth)
      << "\" vector-effect=\"non-scaling-stroke\""
      << " data-im2d-kind=\"dxf-text\""
      << " data-im2d-source-artwork-id=\"" << artwork.id << "\""
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

std::optional<std::vector<ExportItem>>
GatherSelectionItems(const CanvasState &state, const SvgExportRequest &request,
                     SvgExportResult *result, WorldRect *world_bounds) {
  const int artwork_id = request.imported_artwork_id != 0
                             ? request.imported_artwork_id
                             : state.selected_imported_artwork_id;
  const ImportedArtwork *artwork = FindImportedArtwork(state, artwork_id);
  if (artwork == nullptr) {
    result->message = "Selection export requires a selected imported artwork.";
    return std::nullopt;
  }
  if (state.selected_imported_elements.empty()) {
    result->message =
        "Selection export requires one or more selected imported elements.";
    return std::nullopt;
  }

  std::unordered_set<int> seen_paths;
  std::unordered_set<int> seen_text;
  std::vector<ExportItem> items;
  items.reserve(state.selected_imported_elements.size());
  for (const ImportedElementSelection &selection :
       state.selected_imported_elements) {
    if (selection.kind == ImportedElementKind::Path) {
      if (!seen_paths.insert(selection.item_id).second) {
        continue;
      }
      const ImportedPath *path = FindImportedPath(*artwork, selection.item_id);
      if (path == nullptr || path->segments.empty()) {
        continue;
      }
      const WorldRect bounds = PathWorldBounds(*artwork, *path);
      items.push_back({artwork, path, nullptr, bounds});
      ExpandRect(world_bounds, bounds);
      continue;
    }

    if (!seen_text.insert(selection.item_id).second) {
      continue;
    }
    const ImportedDxfText *text =
        FindImportedDxfText(*artwork, selection.item_id);
    if (text == nullptr) {
      continue;
    }
    const WorldRect bounds = TextWorldBounds(*artwork, *text);
    items.push_back({artwork, nullptr, text, bounds});
    ExpandRect(world_bounds, bounds);
  }

  if (items.empty()) {
    result->message =
        "Selection export found no serializable imported elements.";
    return std::nullopt;
  }
  return items;
}

std::optional<std::vector<ExportItem>>
GatherExportAreaItems(const CanvasState &state, const SvgExportRequest &request,
                      SvgExportResult *result, WorldRect *world_bounds) {
  const ExportArea *area = FindExportArea(state, request.export_area_id);
  if (area == nullptr) {
    result->message = "Export-area export requires at least one export area.";
    return std::nullopt;
  }

  *world_bounds = MakeRect(area->origin, ImVec2(area->origin.x + area->size.x,
                                                area->origin.y + area->size.y));
  std::vector<ExportItem> items;
  for (const ImportedArtwork &artwork : state.imported_artwork) {
    if (!artwork.visible) {
      continue;
    }

    for (const ImportedPath &path : artwork.paths) {
      if (path.segments.empty()) {
        continue;
      }
      const WorldRect bounds = PathWorldBounds(artwork, path);
      if (RectsIntersect(*world_bounds, bounds)) {
        items.push_back({&artwork, &path, nullptr, bounds});
      }
    }

    for (const ImportedDxfText &text : artwork.dxf_text) {
      const WorldRect bounds = TextWorldBounds(artwork, text);
      if (RectsIntersect(*world_bounds, bounds)) {
        items.push_back({&artwork, nullptr, &text, bounds});
      }
    }
  }

  if (items.empty()) {
    result->message =
        "No imported elements intersect the requested export area.";
    return std::nullopt;
  }
  return items;
}

} // namespace

SvgExportResult ExportSvg(const CanvasState &state,
                          const SvgExportRequest &request) {
  SvgExportResult result;
  WorldRect world_bounds;
  std::optional<std::vector<ExportItem>> items;
  if (request.scope == SvgExportScope::ActiveSelection) {
    items = GatherSelectionItems(state, request, &result, &world_bounds);
  } else {
    items = GatherExportAreaItems(state, request, &result, &world_bounds);
  }

  if (!items.has_value() || !world_bounds.valid) {
    return result;
  }

  result.bounds_min = world_bounds.min;
  result.bounds_max = world_bounds.max;
  PreflightExportDiagnostics(*items, &result);
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

  for (const ExportItem &item : *items) {
    if (item.path != nullptr) {
      AppendSerializedPath(svg, *item.artwork, *item.path, world_bounds.min,
                           &result);
      continue;
    }

    if (item.text == nullptr) {
      continue;
    }

    for (const ImportedTextGlyph &glyph : item.text->glyphs) {
      for (const ImportedTextContour &contour : glyph.contours) {
        AppendSerializedTextContour(svg, *item.artwork, *item.text, contour,
                                    world_bounds.min, false, &result);
      }
    }
    for (const ImportedTextContour &contour : item.text->placeholder_contours) {
      AppendSerializedTextContour(svg, *item.artwork, *item.text, contour,
                                  world_bounds.min, true, &result);
    }
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