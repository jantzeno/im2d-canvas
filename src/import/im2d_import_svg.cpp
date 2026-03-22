#include "im2d_import.h"

#include "../common/im2d_log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_map>

#define NANOSVG_ALL_COLOR_KEYWORDS
#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"

#include <lunasvg.h>

namespace im2d::importer {
namespace {

struct SvgBuildDiagnostics {
  int skipped_paths = 0;
  int hidden_shapes = 0;
  int imported_paths = 0;
  bool used_image_bounds_fallback = false;
};

struct SvgHierarchyContext {
  std::unordered_map<std::string, int> source_to_group_id;
  std::vector<int> ordered_leaf_group_ids;
};

int AddImportedGroup(ImportedArtwork &artwork, int parent_group_id,
                     std::string label, std::string source_id) {
  ImportedGroup group;
  group.id = artwork.next_group_id++;
  group.parent_group_id = parent_group_id;
  group.label = std::move(label);
  group.source_id = std::move(source_id);
  artwork.groups.push_back(std::move(group));

  if (ImportedGroup *parent = FindImportedGroup(artwork, parent_group_id);
      parent != nullptr) {
    parent->child_group_ids.push_back(artwork.groups.back().id);
  }

  return artwork.groups.back().id;
}

std::vector<lunasvg::Element>
GetChildElements(const lunasvg::Element &element) {
  std::vector<lunasvg::Element> children;
  for (const lunasvg::Node &child : element.children()) {
    if (!child.isElement()) {
      continue;
    }
    children.push_back(child.toElement());
  }
  return children;
}

std::string ResolveElementLabel(const lunasvg::Element &element,
                                const std::string &fallback) {
  const std::string custom_label = element.getAttribute("data-im2d-label");
  if (!custom_label.empty()) {
    return custom_label;
  }

  const std::string source_id = element.getAttribute("id");
  if (!source_id.empty()) {
    return source_id;
  }

  return fallback;
}

void BuildSvgHierarchy(ImportedArtwork &artwork,
                       const lunasvg::Element &element, int parent_group_id,
                       SvgHierarchyContext &context,
                       int *next_group_label_index,
                       bool skip_group_for_element = false) {
  const std::vector<lunasvg::Element> children = GetChildElements(element);
  const std::string source_id = element.getAttribute("id");

  if (children.empty()) {
    context.ordered_leaf_group_ids.push_back(parent_group_id);
    if (!source_id.empty()) {
      context.source_to_group_id[source_id] = parent_group_id;
    }
    return;
  }

  int owner_group_id = parent_group_id;
  if (!skip_group_for_element) {
    owner_group_id = AddImportedGroup(
        artwork, parent_group_id,
        ResolveElementLabel(
            element, "Group " + std::to_string((*next_group_label_index)++)),
        source_id);
    if (!source_id.empty()) {
      context.source_to_group_id[source_id] = owner_group_id;
    }
  } else if (!source_id.empty()) {
    context.source_to_group_id[source_id] = owner_group_id;
  }

  for (const lunasvg::Element &child : children) {
    BuildSvgHierarchy(artwork, child, owner_group_id, context,
                      next_group_label_index, false);
  }
}

int ResolveShapeGroupId(const ImportedArtwork &artwork,
                        const lunasvg::Document &document,
                        const SvgHierarchyContext &context,
                        const NSVGshape &shape, int fallback_group_id,
                        size_t shape_index) {
  if (shape_index < context.ordered_leaf_group_ids.size()) {
    fallback_group_id = context.ordered_leaf_group_ids[shape_index];
  }

  if (shape.id[0] == '\0') {
    return fallback_group_id;
  }

  const std::string source_id(shape.id);
  auto direct_match = context.source_to_group_id.find(source_id);
  if (direct_match != context.source_to_group_id.end()) {
    return direct_match->second;
  }

  for (lunasvg::Element current = document.getElementById(source_id); current;
       current = current.parentElement()) {
    const std::string current_id = current.getAttribute("id");
    if (current_id.empty()) {
      continue;
    }

    auto it = context.source_to_group_id.find(current_id);
    if (it != context.source_to_group_id.end()) {
      return it->second;
    }
  }

  return fallback_group_id;
}

std::string ResolveShapeLabel(const NSVGshape &shape, int shape_index,
                              int part_index, int total_parts) {
  std::string label = shape.id[0] != '\0'
                          ? std::string(shape.id)
                          : "Path " + std::to_string(shape_index + 1);
  if (total_parts > 1) {
    label += " / Part " + std::to_string(part_index + 1);
  }
  return label;
}

bool MatchesRgbColor(unsigned int color, unsigned int expected_color) {
  return (color & 0x00ffffffu) == (expected_color & 0x00ffffffu);
}

ImVec4 SvgColorToImVec4(unsigned int color, float alpha) {
  const float red = static_cast<float>(color & 0xffu) / 255.0f;
  const float green = static_cast<float>((color >> 8u) & 0xffu) / 255.0f;
  const float blue = static_cast<float>((color >> 16u) & 0xffu) / 255.0f;
  return ImVec4(red, green, blue, alpha);
}

ImVec4 ResolveStrokeColor(const NSVGshape &shape) {
  const float alpha = std::clamp(shape.opacity, 0.0f, 1.0f);
  if (shape.stroke.type == NSVG_PAINT_COLOR) {
    return SvgColorToImVec4(shape.stroke.color, alpha);
  }
  if (shape.fill.type == NSVG_PAINT_COLOR) {
    return SvgColorToImVec4(shape.fill.color, alpha);
  }
  return ImVec4(0.92f, 0.94f, 0.97f, alpha <= 0.0f ? 1.0f : alpha);
}

bool IsTextPlaceholderShape(const NSVGshape &shape,
                            const ImportSvgOptions &options) {
  if (!options.mark_text_placeholders) {
    return false;
  }

  return (shape.stroke.type == NSVG_PAINT_COLOR &&
          MatchesRgbColor(shape.stroke.color,
                          options.text_placeholder_color)) ||
         (shape.fill.type == NSVG_PAINT_COLOR &&
          (MatchesRgbColor(shape.fill.color, options.text_placeholder_color) ||
           MatchesRgbColor(shape.fill.color, options.text_filled_glyph_color)));
}

bool IsFilledTextShape(const NSVGshape &shape,
                       const ImportSvgOptions &options) {
  if (!options.mark_text_placeholders || options.text_filled_glyph_color == 0) {
    return false;
  }

  return shape.fill.type == NSVG_PAINT_COLOR &&
         MatchesRgbColor(shape.fill.color, options.text_filled_glyph_color);
}

bool IsHoleTextShape(const NSVGshape &shape, const ImportSvgOptions &options) {
  if (!options.mark_text_placeholders || options.text_hole_glyph_color == 0) {
    return false;
  }

  return shape.fill.type == NSVG_PAINT_COLOR &&
         MatchesRgbColor(shape.fill.color, options.text_hole_glyph_color);
}

ImportedArtwork BuildArtworkFromSvg(const std::string &display_name,
                                    const std::string &source_path,
                                    const lunasvg::Document &document,
                                    const NSVGimage &image,
                                    const ImportSvgOptions &options,
                                    SvgBuildDiagnostics *diagnostics) {
  ImportedArtwork artwork;
  artwork.name = display_name;
  artwork.source_path = source_path;
  artwork.source_format = "SVG";

  lunasvg::Box bounds = document.boundingBox();
  float min_x = bounds.x;
  float min_y = bounds.y;
  float width = bounds.w;
  float height = bounds.h;
  if (width <= 0.0f || height <= 0.0f) {
    min_x = 0.0f;
    min_y = 0.0f;
    width = image.width;
    height = image.height;
    diagnostics->used_image_bounds_fallback = true;
  }

  const float offset_x = -min_x;
  const float offset_y = -min_y;
  artwork.bounds_min = ImVec2(0.0f, 0.0f);
  artwork.bounds_max = ImVec2(width, height);
  artwork.root_group_id =
      AddImportedGroup(artwork, 0, "Document", std::string{});

  SvgHierarchyContext hierarchy_context;
  int next_group_label_index = 1;
  BuildSvgHierarchy(artwork, document.documentElement(), artwork.root_group_id,
                    hierarchy_context, &next_group_label_index, true);

  size_t visible_shape_index = 0;

  for (const NSVGshape *shape = image.shapes; shape != nullptr;
       shape = shape->next) {
    if ((shape->flags & NSVG_FLAGS_VISIBLE) == 0) {
      diagnostics->hidden_shapes += 1;
      continue;
    }

    const int parent_group_id =
        ResolveShapeGroupId(artwork, document, hierarchy_context, *shape,
                            artwork.root_group_id, visible_shape_index);
    int total_path_parts = 0;
    for (const NSVGpath *path = shape->paths; path != nullptr;
         path = path->next) {
      if (path->npts >= 4) {
        total_path_parts += 1;
      }
    }
    int part_index = 0;

    const bool is_text_placeholder = IsTextPlaceholderShape(*shape, options);
    const bool is_filled_text = IsFilledTextShape(*shape, options);
    const bool is_hole_text = IsHoleTextShape(*shape, options);

    for (const NSVGpath *path = shape->paths; path != nullptr;
         path = path->next) {
      if (path->npts < 4) {
        diagnostics->skipped_paths += 1;
        continue;
      }

      ImportedPath imported_path;
      imported_path.id = artwork.next_path_id++;
      imported_path.parent_group_id = parent_group_id;
      imported_path.label =
          ResolveShapeLabel(*shape, static_cast<int>(visible_shape_index),
                            part_index, total_path_parts);
      imported_path.stroke_color = ResolveStrokeColor(*shape);
      imported_path.stroke_width =
          shape->strokeWidth > 0.0f ? shape->strokeWidth : 1.0f;
      imported_path.closed = path->closed != 0;
      if (is_text_placeholder) {
        imported_path.flags |= ImportedPathFlagTextPlaceholder;
      }
      if (is_filled_text) {
        imported_path.flags |= ImportedPathFlagFilledText;
      }
      if (is_hole_text) {
        imported_path.flags |= ImportedPathFlagHoleContour;
      }

      for (int point_index = 0; point_index + 3 < path->npts;
           point_index += 3) {
        const int base = point_index * 2;
        ImportedPathSegment segment;
        segment.kind = ImportedPathSegmentKind::CubicBezier;
        segment.start =
            ImVec2(path->pts[base] + offset_x, path->pts[base + 1] + offset_y);
        segment.control1 = ImVec2(path->pts[base + 2] + offset_x,
                                  path->pts[base + 3] + offset_y);
        segment.control2 = ImVec2(path->pts[base + 4] + offset_x,
                                  path->pts[base + 5] + offset_y);
        segment.end = ImVec2(path->pts[base + 6] + offset_x,
                             path->pts[base + 7] + offset_y);
        imported_path.segments.push_back(segment);
      }

      if (!imported_path.segments.empty()) {
        if (ImportedGroup *group = FindImportedGroup(artwork, parent_group_id);
            group != nullptr) {
          group->path_ids.push_back(imported_path.id);
        }
        artwork.paths.push_back(std::move(imported_path));
        diagnostics->imported_paths += 1;
        part_index += 1;
      } else {
        diagnostics->skipped_paths += 1;
      }
    }

    visible_shape_index += 1;
  }

  RecomputeImportedHierarchyBounds(artwork);

  return artwork;
}

ImportResult ImportSvgDocument(CanvasState &state, std::string_view svg_data,
                               std::string_view display_name,
                               std::string_view source_path,
                               const ImportSvgOptions &options) {
  auto document =
      lunasvg::Document::loadFromData(svg_data.data(), svg_data.size());
  if (!document) {
    log::GetLogger()->error("Failed to parse SVG with lunasvg: {}",
                            source_path);
    return {.success = false, .message = "Failed to parse SVG with lunasvg."};
  }

  std::string mutable_svg(svg_data);
  NSVGimage *image = nsvgParse(mutable_svg.data(), "px", 96.0f);
  if (image == nullptr) {
    log::GetLogger()->error("Failed to parse SVG geometry with nanosvg: {}",
                            source_path);
    return {.success = false,
            .message = "Failed to parse SVG geometry with nanosvg."};
  }

  SvgBuildDiagnostics diagnostics;
  ImportedArtwork artwork =
      BuildArtworkFromSvg(std::string(display_name), std::string(source_path),
                          *document, *image, options, &diagnostics);
  nsvgDelete(image);

  ImportResult result;
  result.skipped_items_count = diagnostics.skipped_paths;

  if (artwork.paths.empty()) {
    result.success = false;
    result.message = "The SVG did not produce any visible path geometry.";
    if (diagnostics.skipped_paths > 0) {
      result.notes.push_back("Skipped " +
                             std::to_string(diagnostics.skipped_paths) +
                             " degenerate or unsupported SVG paths.");
    }
    if (diagnostics.hidden_shapes > 0) {
      result.notes.push_back("Ignored " +
                             std::to_string(diagnostics.hidden_shapes) +
                             " hidden SVG shapes.");
    }
    result.warnings_count = static_cast<int>(result.notes.size());
    log::GetLogger()->warn("SVG import produced no visible geometry: {}",
                           source_path);
    return result;
  }

  const int artwork_id = AppendImportedArtwork(state, std::move(artwork));
  result.success = true;
  result.artwork_id = artwork_id;
  result.message = "Imported SVG sample.";
  if (diagnostics.used_image_bounds_fallback) {
    result.notes.push_back("SVG bounds were empty; used parsed image size as "
                           "the fallback bounds.");
  }
  if (diagnostics.hidden_shapes > 0) {
    result.notes.push_back("Ignored " +
                           std::to_string(diagnostics.hidden_shapes) +
                           " hidden SVG shapes.");
  }
  if (diagnostics.skipped_paths > 0) {
    result.notes.push_back("Skipped " +
                           std::to_string(diagnostics.skipped_paths) +
                           " degenerate or unsupported SVG paths.");
  }
  result.warnings_count = static_cast<int>(result.notes.size());
  if (result.warnings_count > 0) {
    log::GetLogger()->warn("SVG import completed with {} warning(s): {}",
                           result.warnings_count, source_path);
  } else {
    log::GetLogger()->info("Imported SVG sample: {}", source_path);
  }
  return result;
}

} // namespace

ImportResult ImportSvgData(CanvasState &state, std::string_view svg_data,
                           std::string_view display_name,
                           std::string_view source_path) {
  return ImportSvgDocument(state, svg_data, display_name, source_path,
                           ImportSvgOptions{});
}

ImportResult ImportSvgData(CanvasState &state, std::string_view svg_data,
                           std::string_view display_name,
                           std::string_view source_path,
                           const ImportSvgOptions &options) {
  return ImportSvgDocument(state, svg_data, display_name, source_path, options);
}

ImportResult ImportSvgFile(CanvasState &state,
                           const std::filesystem::path &file_path) {
  std::ifstream input(file_path, std::ios::binary);
  if (!input) {
    log::GetLogger()->error("Unable to open SVG file: {}", file_path.string());
    return {.success = false,
            .message = "Unable to open SVG file: " + file_path.string()};
  }

  std::ostringstream buffer;
  buffer << input.rdbuf();
  const std::string svg_data = buffer.str();
  if (svg_data.empty()) {
    log::GetLogger()->error("SVG file is empty: {}", file_path.string());
    return {.success = false,
            .message = "SVG file is empty: " + file_path.string()};
  }

  return ImportSvgDocument(state, svg_data, file_path.filename().string(),
                           file_path.string(), ImportSvgOptions{});
}

} // namespace im2d::importer