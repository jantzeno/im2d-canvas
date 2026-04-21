#pragma once

#include "../canvas/im2d_canvas_types.h"

#include <filesystem>
#include <string>
#include <vector>

namespace im2d::exporter {

enum class SvgExportScope {
  ActiveSelection,
  ExportArea,
};

enum class SvgExportAreaTarget {
  SpecificExportArea,
  AllExportAreas,
};

enum class SvgExportAllMode {
  Combined,
  FilePerExportArea,
};

struct SvgExportRequest {
  SvgExportScope scope = SvgExportScope::ActiveSelection;
  int imported_artwork_id = 0;
  int export_area_id = 0;
  SvgExportAreaTarget export_area_target =
      SvgExportAreaTarget::SpecificExportArea;
  SvgExportAllMode all_export_mode = SvgExportAllMode::Combined;
  bool allow_placeholder_text = false;
  bool include_working_area_geometry = false;
};

struct SvgExportResult {
  bool success = false;
  std::string message;
  std::string svg;
  std::string output_path;
  std::vector<std::string> output_paths;
  ImVec2 bounds_min = ImVec2(0.0f, 0.0f);
  ImVec2 bounds_max = ImVec2(0.0f, 0.0f);
  int path_count = 0;
  int text_count = 0;
  int export_area_count = 0;
  int working_area_count = 0;
  int placeholder_text_count = 0;
  int substituted_font_text_count = 0;
  int open_geometry_item_count = 0;
  int line_segment_count = 0;
  int cubic_segment_count = 0;
  int warning_count = 0;
  std::vector<std::string> warnings;
};

SvgExportResult ExportSvg(const CanvasState &state,
                          const SvgExportRequest &request);
SvgExportResult ExportSvgToFile(const CanvasState &state,
                                const SvgExportRequest &request,
                                const std::filesystem::path &output_path);
std::vector<std::filesystem::path> DefaultSvgExportPaths(
    const CanvasState &state, const SvgExportRequest &request);

} // namespace im2d::exporter
