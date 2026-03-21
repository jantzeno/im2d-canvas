#pragma once

#include "../canvas/im2d_canvas.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace im2d::importer {

struct ImportSvgOptions {
  bool mark_text_placeholders = false;
  unsigned int text_placeholder_color = 0;
  unsigned int text_filled_glyph_color = 0;
  ImVec4 text_placeholder_display_color = ImVec4(0.92f, 0.94f, 0.97f, 1.0f);
};

struct ImportResult {
  bool success = false;
  int artwork_id = 0;
  int warnings_count = 0;
  int skipped_items_count = 0;
  std::string message;
  std::vector<std::string> notes;
};

ImportResult ImportSvgData(CanvasState &state, std::string_view svg_data,
                           std::string_view display_name,
                           std::string_view source_path);
ImportResult ImportSvgData(CanvasState &state, std::string_view svg_data,
                           std::string_view display_name,
                           std::string_view source_path,
                           const ImportSvgOptions &options);
ImportResult ImportSvgFile(CanvasState &state,
                           const std::filesystem::path &file_path);
ImportResult ImportDxfFile(CanvasState &state,
                           const std::filesystem::path &file_path);

} // namespace im2d::importer