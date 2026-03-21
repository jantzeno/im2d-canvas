#include "im2d_import.h"

#include "../common/im2d_log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

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

  for (const NSVGshape *shape = image.shapes; shape != nullptr;
       shape = shape->next) {
    if ((shape->flags & NSVG_FLAGS_VISIBLE) == 0) {
      diagnostics->hidden_shapes += 1;
      continue;
    }

    for (const NSVGpath *path = shape->paths; path != nullptr;
         path = path->next) {
      if (path->npts < 4) {
        diagnostics->skipped_paths += 1;
        continue;
      }

      ImportedPath imported_path;
      const bool is_text_placeholder = IsTextPlaceholderShape(*shape, options);
      const bool is_filled_text = IsFilledTextShape(*shape, options);
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
        artwork.paths.push_back(std::move(imported_path));
        diagnostics->imported_paths += 1;
      } else {
        diagnostics->skipped_paths += 1;
      }
    }
  }

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