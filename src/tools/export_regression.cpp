#include "../canvas/im2d_canvas_document.h"
#include "../canvas/im2d_canvas_imported_artwork_ops.h"
#include "../export/im2d_export_svg.h"
#include "../import/im2d_import.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <regex>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

using im2d::AppendImportedArtwork;
using im2d::AutoCloseImportedArtworkToPolyline;
using im2d::CanvasState;
using im2d::ClearSelectedImportedElements;
using im2d::ExportArea;
using im2d::FindImportedArtwork;
using im2d::ImportedArtwork;
using im2d::ImportedArtworkOperationResult;
using im2d::ImportedArtworkPrepareMode;
using im2d::ImportedDxfText;
using im2d::ImportedElementIssueFlagPlaceholderText;
using im2d::ImportedElementKind;
using im2d::ImportedElementSelection;
using im2d::ImportedPathSegment;
using im2d::ImportedPathSegmentKind;
using im2d::ImportedTextContour;
using im2d::ImportedTextContourRole;
using im2d::InitializeDefaultDocument;
using im2d::PrepareImportedArtworkForCutting;
using im2d::exporter::ExportSvg;
using im2d::exporter::SvgExportRequest;
using im2d::exporter::SvgExportResult;
using im2d::exporter::SvgExportScope;
using im2d::importer::ImportDxfFile;
using im2d::importer::ImportResult;
using im2d::importer::ImportSvgFile;

struct TestRun {
  std::string name;
  int failures = 0;
  std::vector<std::string> messages;
};

bool Check(TestRun *run, bool condition, std::string message) {
  if (condition) {
    return true;
  }
  run->failures += 1;
  run->messages.push_back(std::move(message));
  return false;
}

float BoundsWidth(const SvgExportResult &result) {
  return result.bounds_max.x - result.bounds_min.x;
}

float BoundsHeight(const SvgExportResult &result) {
  return result.bounds_max.y - result.bounds_min.y;
}

bool NearlyEqual(float left, float right, float epsilon = 0.05f) {
  return std::fabs(left - right) <= epsilon;
}

std::string MakeSingleLineDxf(int measurement, int insunits, double length) {
  std::ostringstream stream;
  stream << "0\nSECTION\n2\nHEADER\n"
         << "9\n$ACADVER\n1\nAC1015\n"
         << "9\n$MEASUREMENT\n70\n"
         << measurement << "\n"
         << "9\n$INSUNITS\n70\n"
         << insunits << "\n"
         << "0\nENDSEC\n"
         << "0\nSECTION\n2\nENTITIES\n"
         << "0\nLINE\n8\n0\n"
         << "10\n0.0\n20\n0.0\n30\n0.0\n"
         << "11\n"
         << length << "\n21\n0.0\n31\n0.0\n"
         << "0\nENDSEC\n0\nEOF\n";
  return stream.str();
}

bool HasCubicCommand(const std::string &svg) {
  static const std::regex pattern(R"(<path[^>]*d="[^"]*[Cc][^"]*")");
  return std::regex_search(svg, pattern);
}

bool HasNonScalingStroke(const std::string &svg) {
  static const std::regex vector_effect_pattern(
      R"(<path[^>]*vector-effect="non-scaling-stroke")");
  static const std::regex stroke_width_pattern(R"(<path[^>]*stroke-width="1")");
  return std::regex_search(svg, vector_effect_pattern) &&
         std::regex_search(svg, stroke_width_pattern);
}

bool RegressionTraceEnabled() {
  return std::getenv("IM2D_REGRESSION_TRACE") != nullptr;
}

void TraceRegressionStep(const std::string &message) {
  if (!RegressionTraceEnabled()) {
    return;
  }
  std::cout << "[TRACE] " << message << std::endl;
}

struct SvgLineArtifactStats {
  int line_count = 0;
  int short_line_count = 0;
  int very_short_line_count = 0;
  int suspicious_spike_count = 0;
  int suspicious_kink_count = 0;
  float shortest_line_length = std::numeric_limits<float>::infinity();
  struct Hotspot {
    int item_id = 0;
    ImVec2 point = ImVec2(0.0f, 0.0f);
    float score = 0.0f;
    const char *kind = "";
  };
  std::vector<Hotspot> hotspots;
};

float DotProduct(const ImVec2 &left, const ImVec2 &right) {
  return left.x * right.x + left.y * right.y;
}

float VectorLength(const ImVec2 &vector) {
  return std::sqrt(DotProduct(vector, vector));
}

bool NormalizeVector(const ImVec2 &vector, ImVec2 *normalized) {
  if (normalized == nullptr) {
    return false;
  }
  const float length = VectorLength(vector);
  if (length <= 0.000001f) {
    return false;
  }
  *normalized = ImVec2(vector.x / length, vector.y / length);
  return true;
}

void RecordSvgVertexArtifact(const ImVec2 &previous_segment,
                             float previous_length,
                             const ImVec2 &current_segment,
                             float current_length, int item_id,
                             const ImVec2 &vertex_point,
                             SvgLineArtifactStats *stats) {
  if (stats == nullptr) {
    return;
  }

  ImVec2 previous_direction;
  ImVec2 current_direction;
  if (!NormalizeVector(previous_segment, &previous_direction) ||
      !NormalizeVector(current_segment, &current_direction)) {
    return;
  }

  const float direction_dot = DotProduct(previous_direction, current_direction);
  constexpr float kShortAdjacentThreshold = 0.35f;
  constexpr float kSpikeDotThreshold = -0.6f;
  constexpr float kKinkDotThreshold = -0.15f;

  auto record_hotspot = [&](float score, const char *kind) {
    SvgLineArtifactStats::Hotspot hotspot;
    hotspot.item_id = item_id;
    hotspot.point = vertex_point;
    hotspot.score = score;
    hotspot.kind = kind;
    stats->hotspots.push_back(hotspot);
  };

  if ((previous_length < kShortAdjacentThreshold ||
       current_length < kShortAdjacentThreshold) &&
      direction_dot <= kSpikeDotThreshold) {
    stats->suspicious_spike_count += 1;
    record_hotspot((1.0f - direction_dot) +
                       (kShortAdjacentThreshold -
                        std::min(previous_length, current_length)),
                   "spike");
    return;
  }

  if (previous_length < kShortAdjacentThreshold &&
      current_length < kShortAdjacentThreshold &&
      direction_dot <= kKinkDotThreshold) {
    stats->suspicious_kink_count += 1;
    record_hotspot((0.5f - direction_dot) +
                       (kShortAdjacentThreshold - previous_length) +
                       (kShortAdjacentThreshold - current_length),
                   "kink");
  }
}

bool ParseSvgIntAttribute(const std::string &tag, const std::string &name,
                          int *value) {
  if (value == nullptr) {
    return false;
  }
  const std::string needle = name + "=\"";
  const size_t start = tag.find(needle);
  if (start == std::string::npos) {
    return false;
  }
  const size_t value_start = start + needle.size();
  const size_t value_end = tag.find('"', value_start);
  if (value_end == std::string::npos) {
    return false;
  }
  try {
    *value = std::stoi(tag.substr(value_start, value_end - value_start));
    return true;
  } catch (...) {
    return false;
  }
}

void SkipSvgPathSeparators(const std::string &path_data, size_t *index) {
  while (*index < path_data.size() &&
         (std::isspace(static_cast<unsigned char>(path_data[*index])) ||
          path_data[*index] == ',')) {
    *index += 1;
  }
}

bool ParseSvgFloat(const std::string &path_data, size_t *index, float *value) {
  SkipSvgPathSeparators(path_data, index);
  if (*index >= path_data.size()) {
    return false;
  }

  const char *begin = path_data.c_str() + *index;
  char *end = nullptr;
  *value = std::strtof(begin, &end);
  if (end == begin) {
    return false;
  }

  *index = static_cast<size_t>(end - path_data.c_str());
  return true;
}

void RecordSvgLineLength(float length, SvgLineArtifactStats *stats) {
  if (stats == nullptr) {
    return;
  }
  stats->line_count += 1;
  stats->shortest_line_length = std::min(stats->shortest_line_length, length);

  constexpr float kShortLineThreshold = 0.35f;
  constexpr float kVeryShortLineThreshold = 0.15f;
  if (length < kShortLineThreshold) {
    stats->short_line_count += 1;
  }
  if (length < kVeryShortLineThreshold) {
    stats->very_short_line_count += 1;
  }
}

SvgLineArtifactStats AnalyzeSvgLineArtifacts(const std::string &svg) {
  SvgLineArtifactStats stats;
  size_t search_index = 0;
  while (true) {
    const size_t path_start = svg.find("<path", search_index);
    if (path_start == std::string::npos) {
      break;
    }
    const size_t path_end = svg.find('>', path_start);
    if (path_end == std::string::npos) {
      break;
    }
    const size_t data_start = svg.find(" d=\"", path_start);
    if (data_start == std::string::npos || data_start > path_end) {
      search_index = path_end + 1;
      continue;
    }
    const std::string path_tag =
        svg.substr(path_start, path_end - path_start + 1);
    const size_t value_start = data_start + 4;
    const size_t value_end = svg.find('"', value_start);
    if (value_end == std::string::npos || value_end > path_end) {
      search_index = path_end + 1;
      continue;
    }

    const std::string path_data =
        svg.substr(value_start, value_end - value_start);
    search_index = path_end + 1;
    int item_id = 0;
    ParseSvgIntAttribute(path_tag, "data-im2d-source-item-id", &item_id);
    size_t index = 0;
    char command = '\0';
    ImVec2 current(0.0f, 0.0f);
    ImVec2 subpath_start(0.0f, 0.0f);
    ImVec2 previous_line_segment(0.0f, 0.0f);
    float previous_line_length = 0.0f;
    bool has_current = false;
    bool has_previous_line = false;

    while (index < path_data.size()) {
      SkipSvgPathSeparators(path_data, &index);
      if (index >= path_data.size()) {
        break;
      }

      if (std::isalpha(static_cast<unsigned char>(path_data[index]))) {
        command = static_cast<char>(
            std::toupper(static_cast<unsigned char>(path_data[index])));
        index += 1;
        if (command == 'Z') {
          current = subpath_start;
          has_current = true;
          has_previous_line = false;
          continue;
        }
      }

      float x1 = 0.0f;
      float y1 = 0.0f;
      float x2 = 0.0f;
      float y2 = 0.0f;
      float x = 0.0f;
      float y = 0.0f;
      switch (command) {
      case 'M':
        if (!ParseSvgFloat(path_data, &index, &x) ||
            !ParseSvgFloat(path_data, &index, &y)) {
          index = path_data.size();
          break;
        }
        current = ImVec2(x, y);
        subpath_start = current;
        has_current = true;
        has_previous_line = false;
        break;
      case 'L':
        if (!ParseSvgFloat(path_data, &index, &x) ||
            !ParseSvgFloat(path_data, &index, &y)) {
          index = path_data.size();
          break;
        }
        if (has_current) {
          const ImVec2 current_segment(x - current.x, y - current.y);
          const float current_length = VectorLength(current_segment);
          RecordSvgLineLength(current_length, &stats);
          if (has_previous_line) {
            RecordSvgVertexArtifact(previous_line_segment, previous_line_length,
                                    current_segment, current_length, item_id,
                                    current, &stats);
          }
          previous_line_segment = current_segment;
          previous_line_length = current_length;
          has_previous_line = true;
        }
        current = ImVec2(x, y);
        has_current = true;
        break;
      case 'C':
        if (!ParseSvgFloat(path_data, &index, &x1) ||
            !ParseSvgFloat(path_data, &index, &y1) ||
            !ParseSvgFloat(path_data, &index, &x2) ||
            !ParseSvgFloat(path_data, &index, &y2) ||
            !ParseSvgFloat(path_data, &index, &x) ||
            !ParseSvgFloat(path_data, &index, &y)) {
          index = path_data.size();
          break;
        }
        current = ImVec2(x, y);
        has_current = true;
        has_previous_line = false;
        break;
      default:
        index += 1;
        break;
      }
    }
  }

  if (!std::isfinite(stats.shortest_line_length)) {
    stats.shortest_line_length = 0.0f;
  }
  std::sort(stats.hotspots.begin(), stats.hotspots.end(),
            [](const SvgLineArtifactStats::Hotspot &left,
               const SvgLineArtifactStats::Hotspot &right) {
              if (std::fabs(left.score - right.score) > 0.0001f) {
                return left.score > right.score;
              }
              if (left.item_id != right.item_id) {
                return left.item_id < right.item_id;
              }
              if (std::fabs(left.point.x - right.point.x) > 0.0001f) {
                return left.point.x < right.point.x;
              }
              return left.point.y < right.point.y;
            });
  if (stats.hotspots.size() > 5) {
    stats.hotspots.resize(5);
  }
  return stats;
}

std::string FormatHotspots(const SvgLineArtifactStats &stats) {
  if (stats.hotspots.empty()) {
    return "none";
  }
  std::string formatted;
  for (size_t index = 0; index < stats.hotspots.size(); ++index) {
    const auto &hotspot = stats.hotspots[index];
    if (!formatted.empty()) {
      formatted += "; ";
    }
    formatted += hotspot.kind;
    formatted += " item=" + std::to_string(hotspot.item_id);
    formatted += " @(" + std::to_string(hotspot.point.x) + "," +
                 std::to_string(hotspot.point.y) + ")";
    formatted += " score=" + std::to_string(hotspot.score);
  }
  return formatted;
}

ImportedPathSegment MakeLineSegment(const ImVec2 &start, const ImVec2 &end) {
  ImportedPathSegment segment;
  segment.kind = ImportedPathSegmentKind::Line;
  segment.start = start;
  segment.end = end;
  return segment;
}

ImportedPathSegment MakeCubicSegment(const ImVec2 &start,
                                     const ImVec2 &control1,
                                     const ImVec2 &control2,
                                     const ImVec2 &end) {
  ImportedPathSegment segment;
  segment.kind = ImportedPathSegmentKind::CubicBezier;
  segment.start = start;
  segment.control1 = control1;
  segment.control2 = control2;
  segment.end = end;
  return segment;
}

ImportedTextContour MakeRectangleContour(float min_x, float min_y, float max_x,
                                         float max_y,
                                         const std::string &label) {
  ImportedTextContour contour;
  contour.label = label;
  contour.role = ImportedTextContourRole::Guide;
  contour.closed = true;
  contour.segments = {
      MakeLineSegment(ImVec2(min_x, min_y), ImVec2(max_x, min_y)),
      MakeLineSegment(ImVec2(max_x, min_y), ImVec2(max_x, max_y)),
      MakeLineSegment(ImVec2(max_x, max_y), ImVec2(min_x, max_y)),
      MakeLineSegment(ImVec2(min_x, max_y), ImVec2(min_x, min_y)),
  };
  contour.bounds_min = ImVec2(min_x, min_y);
  contour.bounds_max = ImVec2(max_x, max_y);
  return contour;
}

im2d::ImportedPath MakePath(int id,
                            std::initializer_list<ImportedPathSegment> segments,
                            bool closed, const std::string &label = "Path") {
  im2d::ImportedPath path;
  path.id = id;
  path.label = label;
  path.closed = closed;
  path.stroke_width = 1.0f;
  path.stroke_color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
  path.segments.assign(segments.begin(), segments.end());
  return path;
}

const im2d::ImportedPath *FindPathById(const ImportedArtwork &artwork,
                                       int path_id) {
  for (const im2d::ImportedPath &path : artwork.paths) {
    if (path.id == path_id) {
      return &path;
    }
  }
  return nullptr;
}

void SelectAllImportedElements(CanvasState &state,
                               const ImportedArtwork &artwork) {
  ClearSelectedImportedElements(state);
  state.selected_imported_artwork_id = artwork.id;
  state.selected_imported_elements.reserve(artwork.paths.size() +
                                           artwork.dxf_text.size());
  for (const auto &path : artwork.paths) {
    state.selected_imported_elements.push_back(
        ImportedElementSelection{ImportedElementKind::Path, path.id});
  }
  for (const auto &text : artwork.dxf_text) {
    state.selected_imported_elements.push_back(
        ImportedElementSelection{ImportedElementKind::DxfText, text.id});
  }
}

bool ConfigureExportArea(CanvasState &state, const ImportedArtwork &artwork,
                         float margin = 1.0f) {
  if (state.export_areas.empty()) {
    return false;
  }
  ExportArea &area = state.export_areas.front();
  area.origin =
      ImVec2(artwork.bounds_min.x - margin, artwork.bounds_min.y - margin);
  area.size = ImVec2(
      std::max((artwork.bounds_max.x - artwork.bounds_min.x) + margin * 2.0f,
               1.0f),
      std::max((artwork.bounds_max.y - artwork.bounds_min.y) + margin * 2.0f,
               1.0f));
  return true;
}

std::string FormatPathEndpointSummary(const im2d::ImportedPath &path) {
  if (path.segments.empty()) {
    return "empty";
  }
  const ImVec2 start = path.segments.front().start;
  const ImVec2 end = path.segments.back().end;
  int cubic_count = 0;
  for (const ImportedPathSegment &segment : path.segments) {
    if (segment.kind == ImportedPathSegmentKind::CubicBezier) {
      cubic_count += 1;
    }
  }
  return "id=" + std::to_string(path.id) +
         " closed=" + std::string(path.closed ? "true" : "false") +
         " segments=" + std::to_string(path.segments.size()) +
         " cubic=" + std::to_string(cubic_count) + " start=(" +
         std::to_string(start.x) + "," + std::to_string(start.y) + ")" +
         " end=(" + std::to_string(end.x) + "," + std::to_string(end.y) + ")";
}

void TraceSelectedPathSummaries(const ImportedArtwork &artwork,
                                const std::vector<int> &path_ids,
                                const std::string &label) {
  if (!RegressionTraceEnabled()) {
    return;
  }
  for (const int path_id : path_ids) {
    const im2d::ImportedPath *path = FindPathById(artwork, path_id);
    if (path == nullptr) {
      TraceRegressionStep(label + " path=" + std::to_string(path_id) +
                          " missing");
      continue;
    }
    TraceRegressionStep(label + " " + FormatPathEndpointSummary(*path));
  }
}

SvgExportResult ExportSelection(const CanvasState &state, int artwork_id) {
  SvgExportRequest request;
  request.scope = SvgExportScope::ActiveSelection;
  request.imported_artwork_id = artwork_id;
  return ExportSvg(state, request);
}

SvgExportResult ExportArea(const CanvasState &state, int export_area_id) {
  SvgExportRequest request;
  request.scope = SvgExportScope::ExportArea;
  request.export_area_id = export_area_id;
  return ExportSvg(state, request);
}

CanvasState MakeDocument() {
  CanvasState state;
  InitializeDefaultDocument(state);
  return state;
}

TestRun RunShapefontRegression(const fs::path &project_root) {
  TestRun run{"Shapefont DXF text export"};
  CanvasState state = MakeDocument();
  const fs::path file_path = project_root / "samples/dxf/Shapefont.dxf";

  const ImportResult import = ImportDxfFile(state, file_path);
  if (!Check(&run, import.success, "DXF import failed: " + import.message)) {
    return run;
  }

  const ImportedArtwork *artwork =
      FindImportedArtwork(state, import.artwork_id);
  if (!Check(&run, artwork != nullptr,
             "Imported artwork was not added to canvas state.")) {
    return run;
  }
  if (!Check(&run, ConfigureExportArea(state, *artwork),
             "Document did not contain an export area to configure.")) {
    return run;
  }

  SelectAllImportedElements(state, *artwork);
  const SvgExportResult selection = ExportSelection(state, artwork->id);
  const SvgExportResult area = ExportArea(state, state.export_areas.front().id);

  Check(&run, selection.success,
        "Selection export failed: " + selection.message);
  Check(&run, area.success, "Export-area export failed: " + area.message);
  if (!selection.success || !area.success) {
    return run;
  }

  Check(&run, selection.text_count > 0,
        "Expected Shapefont export to contain DXF text items.");
  Check(&run, selection.svg.find("data-im2d-source-text=") != std::string::npos,
        "Expected exported SVG to include DXF source-text metadata.");
  Check(&run, HasNonScalingStroke(selection.svg),
        "Expected exported SVG paths to use non-scaling preview strokes.");
  Check(&run, selection.text_count == area.text_count,
        "Selection and export-area exports disagreed on DXF text count.");
  Check(&run, selection.path_count == area.path_count,
        "Selection and export-area exports disagreed on path count.");
  Check(&run,
        selection.placeholder_text_count +
                selection.substituted_font_text_count <=
            selection.text_count,
        "DXF text warning counters exceeded the exported text count.");
  if (selection.placeholder_text_count > 0 ||
      selection.substituted_font_text_count > 0) {
    Check(&run, selection.warning_count > 0,
          "Expected warning text when DXF text export used placeholders or "
          "substituted fonts.");
    Check(&run, selection.svg.find("<desc>") != std::string::npos,
          "Expected warning summary to be embedded in exported SVG metadata.");
  }
  return run;
}

TestRun RunDxfInchUnitScaleRegression(const fs::path &project_root) {
  TestRun run{"DXF inch unit scaling"};
  CanvasState state = MakeDocument();
  const fs::path file_path =
      project_root / "build/dxf-inch-unit-scale-regression.dxf";
  fs::create_directories(file_path.parent_path());

  {
    std::ofstream output(file_path);
    if (!Check(&run, output.is_open(),
               "Could not create temporary DXF regression fixture.")) {
      return run;
    }
    output << MakeSingleLineDxf(/*measurement=*/0, /*insunits=*/1,
                                /*length=*/1.0);
  }

  const ImportResult import = ImportDxfFile(state, file_path);
  std::error_code remove_error;
  fs::remove(file_path, remove_error);

  if (!Check(&run, import.success, "DXF import failed: " + import.message)) {
    return run;
  }

  const ImportedArtwork *artwork =
      FindImportedArtwork(state, import.artwork_id);
  if (!Check(&run, artwork != nullptr,
             "Imported artwork was not added to canvas state.")) {
    return run;
  }

  const float imported_width = artwork->bounds_max.x - artwork->bounds_min.x;
  Check(&run, NearlyEqual(imported_width, 96.0f, 0.5f),
        "Expected a 1-inch DXF line to import at about 96 px, got " +
            std::to_string(imported_width) + ".");
  return run;
}

TestRun RunAutobotPrepareRegression(const fs::path &project_root) {
  TestRun run{"Autobot fidelity-first prepare"};
  CanvasState state = MakeDocument();
  const fs::path file_path =
      project_root / "samples/dxf/Autobot Insignia Vector.dxf";

  const ImportResult import = ImportDxfFile(state, file_path);
  if (!Check(&run, import.success, "DXF import failed: " + import.message)) {
    return run;
  }

  const ImportedArtwork *original_artwork =
      FindImportedArtwork(state, import.artwork_id);
  if (!Check(&run, original_artwork != nullptr,
             "Imported artwork was not added to canvas state.")) {
    return run;
  }
  if (!Check(&run, ConfigureExportArea(state, *original_artwork),
             "Document did not contain an export area to configure.")) {
    return run;
  }

  SelectAllImportedElements(state, *original_artwork);
  const SvgExportResult before = ExportSelection(state, original_artwork->id);
  Check(&run, before.success,
        "Baseline selection export failed: " + before.message);
  if (!before.success) {
    return run;
  }

  const ImportedArtworkOperationResult prepare =
      PrepareImportedArtworkForCutting(
          state, original_artwork->id, 0.5f,
          ImportedArtworkPrepareMode::FidelityFirst);
  Check(&run, prepare.success,
        "Prepare-for-cutting failed: " + prepare.message);
  Check(&run, prepare.prepare_mode == ImportedArtworkPrepareMode::FidelityFirst,
        "Prepare-for-cutting reported the wrong prepare mode.");
  Check(
      &run, prepare.preserved_count > 0,
      "Expected fidelity-first preparation to preserve at least one contour.");
  if (!prepare.success) {
    return run;
  }

  const int prepared_artwork_id = prepare.created_artwork_id != 0
                                      ? prepare.created_artwork_id
                                      : prepare.artwork_id;

  const ImportedArtwork *prepared_artwork =
      FindImportedArtwork(state, prepared_artwork_id);
  if (!Check(&run, prepared_artwork != nullptr,
             "Prepared artwork was not available after prepare-for-cutting.")) {
    return run;
  }
  if (!Check(&run, ConfigureExportArea(state, *prepared_artwork),
             "Failed to reconfigure export area for prepared artwork.")) {
    return run;
  }

  SelectAllImportedElements(state, *prepared_artwork);
  const SvgExportResult after = ExportSelection(state, prepared_artwork_id);
  Check(&run, after.success,
        "Prepared selection export failed: " + after.message);
  if (!after.success) {
    return run;
  }

  Check(&run, before.path_count == after.path_count,
        "Fidelity-first prepare changed the exported path count for Autobot.");
  Check(&run, before.line_segment_count == after.line_segment_count,
        "Fidelity-first prepare changed the exported line segment count for "
        "Autobot.");
  Check(&run,
        NearlyEqual(BoundsWidth(before), BoundsWidth(after)) &&
            NearlyEqual(BoundsHeight(before), BoundsHeight(after)),
        "Fidelity-first prepare changed the exported bounds for Autobot.");
  return run;
}

TestRun RunAutobotAutoCloseWorkflowRegression(const fs::path &project_root) {
  TestRun run{"Autobot auto-close workflow"};
  const fs::path file_path =
      project_root / "samples/dxf/Autobot Insignia Vector.dxf";

  TraceRegressionStep("Autobot: baseline import");
  CanvasState baseline_state = MakeDocument();
  const ImportResult baseline_import = ImportDxfFile(baseline_state, file_path);
  if (!Check(&run, baseline_import.success,
             "Baseline Autobot import failed: " + baseline_import.message)) {
    return run;
  }

  const ImportedArtwork *baseline_artwork =
      FindImportedArtwork(baseline_state, baseline_import.artwork_id);
  if (!Check(&run, baseline_artwork != nullptr,
             "Baseline Autobot artwork missing after import.")) {
    return run;
  }
  TraceSelectedPathSummaries(*baseline_artwork, {2, 3, 4},
                             "Autobot baseline hotspot path");
  if (!Check(&run, ConfigureExportArea(baseline_state, *baseline_artwork),
             "Could not configure export area for baseline Autobot.")) {
    return run;
  }

  SelectAllImportedElements(baseline_state, *baseline_artwork);
  const SvgExportResult baseline_export =
      ExportSelection(baseline_state, baseline_artwork->id);
  if (!Check(&run, baseline_export.success,
             "Baseline Autobot export failed: " + baseline_export.message)) {
    return run;
  }

  TraceRegressionStep("Autobot: auto-close import");
  CanvasState auto_close_state = MakeDocument();
  const ImportResult auto_close_import =
      ImportDxfFile(auto_close_state, file_path);
  if (!Check(&run, auto_close_import.success,
             "Auto-close Autobot import failed: " +
                 auto_close_import.message)) {
    return run;
  }

  const ImportedArtworkOperationResult auto_close_result =
      AutoCloseImportedArtworkToPolyline(auto_close_state,
                                         auto_close_import.artwork_id, 0.5f);
  TraceRegressionStep("Autobot: auto-close complete");
  if (!Check(&run, auto_close_result.success,
             "Auto-close Autobot failed: " + auto_close_result.message)) {
    return run;
  }

  const ImportedArtwork *auto_close_artwork =
      FindImportedArtwork(auto_close_state, auto_close_import.artwork_id);
  if (!Check(&run, auto_close_artwork != nullptr,
             "Auto-closed Autobot artwork missing after auto-close.")) {
    return run;
  }
  TraceSelectedPathSummaries(*auto_close_artwork, {2, 3, 4},
                             "Autobot auto-close hotspot path");
  if (!Check(&run, ConfigureExportArea(auto_close_state, *auto_close_artwork),
             "Could not configure export area for auto-closed Autobot.")) {
    return run;
  }

  SelectAllImportedElements(auto_close_state, *auto_close_artwork);
  const SvgExportResult auto_close_export =
      ExportSelection(auto_close_state, auto_close_artwork->id);
  if (!Check(&run, auto_close_export.success,
             "Auto-closed Autobot export failed: " +
                 auto_close_export.message)) {
    return run;
  }

  Check(&run, auto_close_export.path_count > 0,
        "Auto-closed Autobot export should contain drawable paths.");
  Check(&run, auto_close_export.path_count <= baseline_export.path_count,
        "Auto-close Autobot should not increase exported path count. "
        "Baseline=" +
            std::to_string(baseline_export.path_count) +
            ", after=" + std::to_string(auto_close_export.path_count) + ".");
  Check(&run,
        NearlyEqual(BoundsWidth(baseline_export),
                    BoundsWidth(auto_close_export)) &&
            NearlyEqual(BoundsHeight(baseline_export),
                        BoundsHeight(auto_close_export)),
        "Auto-close Autobot should preserve overall export bounds.");

  TraceRegressionStep("Autobot: prepare+auto-close import");
  CanvasState prepare_auto_close_state = MakeDocument();
  const ImportResult prepare_auto_close_import =
      ImportDxfFile(prepare_auto_close_state, file_path);
  if (!Check(&run, prepare_auto_close_import.success,
             "Prepare+auto-close Autobot import failed: " +
                 prepare_auto_close_import.message)) {
    return run;
  }

  const ImportedArtworkOperationResult prepare_auto_close_result =
      PrepareImportedArtworkForCutting(
          prepare_auto_close_state, prepare_auto_close_import.artwork_id, 0.5f,
          ImportedArtworkPrepareMode::FidelityFirst, true);
  TraceRegressionStep("Autobot: prepare+auto-close complete");
  if (!Check(&run, prepare_auto_close_result.success,
             "Prepare+auto-close Autobot failed: " +
                 prepare_auto_close_result.message)) {
    return run;
  }

  const int prepared_artwork_id =
      prepare_auto_close_result.created_artwork_id != 0
          ? prepare_auto_close_result.created_artwork_id
          : prepare_auto_close_result.artwork_id;
  const ImportedArtwork *prepared_artwork =
      FindImportedArtwork(prepare_auto_close_state, prepared_artwork_id);
  if (!Check(&run, prepared_artwork != nullptr,
             "Prepared Autobot artwork missing after auto-close prepare.")) {
    return run;
  }
  if (!Check(&run,
             ConfigureExportArea(prepare_auto_close_state, *prepared_artwork),
             "Could not configure export area for prepared Autobot.")) {
    return run;
  }

  SelectAllImportedElements(prepare_auto_close_state, *prepared_artwork);
  const SvgExportResult prepared_export =
      ExportSelection(prepare_auto_close_state, prepared_artwork_id);
  if (!Check(&run, prepared_export.success,
             "Prepared Autobot export failed: " + prepared_export.message)) {
    return run;
  }

  const SvgLineArtifactStats auto_close_artifacts =
      AnalyzeSvgLineArtifacts(auto_close_export.svg);
  const SvgLineArtifactStats prepared_artifacts =
      AnalyzeSvgLineArtifacts(prepared_export.svg);
  TraceRegressionStep(
      "Autobot artifacts: auto-close short=" +
      std::to_string(auto_close_artifacts.short_line_count) + ", very-short=" +
      std::to_string(auto_close_artifacts.very_short_line_count) + ", spikes=" +
      std::to_string(auto_close_artifacts.suspicious_spike_count) +
      ", kinks=" + std::to_string(auto_close_artifacts.suspicious_kink_count) +
      ", prepare short=" + std::to_string(prepared_artifacts.short_line_count) +
      ", prepare very-short=" +
      std::to_string(prepared_artifacts.very_short_line_count) +
      ", prepare spikes=" +
      std::to_string(prepared_artifacts.suspicious_spike_count) +
      ", prepare kinks=" +
      std::to_string(prepared_artifacts.suspicious_kink_count));
  TraceRegressionStep("Autobot hotspots auto-close: " +
                      FormatHotspots(auto_close_artifacts));
  TraceRegressionStep("Autobot hotspots prepare: " +
                      FormatHotspots(prepared_artifacts));

  Check(&run,
        prepared_export.line_segment_count ==
            auto_close_export.line_segment_count,
        "Prepare+auto-close Autobot should not inject extra line segments. "
        "AutoClose=" +
            std::to_string(auto_close_export.line_segment_count) +
            ", Prepare+AutoClose=" +
            std::to_string(prepared_export.line_segment_count) + ".");
  Check(&run, prepared_export.path_count == auto_close_export.path_count,
        "Prepare+auto-close Autobot should not change exported path count "
        "relative to auto-close. AutoClose=" +
            std::to_string(auto_close_export.path_count) +
            ", Prepare+AutoClose=" +
            std::to_string(prepared_export.path_count) + ".");
  Check(
      &run,
      NearlyEqual(BoundsWidth(auto_close_export),
                  BoundsWidth(prepared_export)) &&
          NearlyEqual(BoundsHeight(auto_close_export),
                      BoundsHeight(prepared_export)),
      "Prepare+auto-close Autobot should preserve auto-closed export bounds.");
  Check(&run,
        prepare_auto_close_result.open_count <= auto_close_result.open_count,
        "Prepare+auto-close Autobot should not increase open contours. "
        "AutoClose=" +
            std::to_string(auto_close_result.open_count) +
            ", Prepare+AutoClose=" +
            std::to_string(prepare_auto_close_result.open_count) + ".");
  Check(&run,
        prepared_artifacts.short_line_count <=
            auto_close_artifacts.short_line_count,
        "Prepare+auto-close Autobot should not add more short line fragments. "
        "AutoClose=" +
            std::to_string(auto_close_artifacts.short_line_count) +
            ", Prepare+AutoClose=" +
            std::to_string(prepared_artifacts.short_line_count) + ".");
  Check(&run,
        prepared_artifacts.very_short_line_count <=
            auto_close_artifacts.very_short_line_count,
        "Prepare+auto-close Autobot should not add more sub-tolerance line "
        "fragments. AutoClose=" +
            std::to_string(auto_close_artifacts.very_short_line_count) +
            ", Prepare+AutoClose=" +
            std::to_string(prepared_artifacts.very_short_line_count) + ".");
  Check(&run,
        prepared_artifacts.suspicious_spike_count <=
            auto_close_artifacts.suspicious_spike_count,
        "Prepare+auto-close Autobot should not add more suspicious spike "
        "vertices. AutoClose=" +
            std::to_string(auto_close_artifacts.suspicious_spike_count) +
            ", Prepare+AutoClose=" +
            std::to_string(prepared_artifacts.suspicious_spike_count) + ".");
  Check(&run,
        prepared_artifacts.suspicious_kink_count <=
            auto_close_artifacts.suspicious_kink_count,
        "Prepare+auto-close Autobot should not add more suspicious short "
        "kinks. AutoClose=" +
            std::to_string(auto_close_artifacts.suspicious_kink_count) +
            ", Prepare+AutoClose=" +
            std::to_string(prepared_artifacts.suspicious_kink_count) + ".");

  return run;
}

TestRun RunSvgCubicRegression(const fs::path &project_root) {
  TestRun run{"SVG cubic preservation"};
  CanvasState state = MakeDocument();
  const fs::path file_path = project_root / "samples/svg/nano.svg";

  const ImportResult import = ImportSvgFile(state, file_path);
  if (!Check(&run, import.success, "SVG import failed: " + import.message)) {
    return run;
  }

  const ImportedArtwork *artwork =
      FindImportedArtwork(state, import.artwork_id);
  if (!Check(&run, artwork != nullptr,
             "Imported SVG artwork was not added to canvas state.")) {
    return run;
  }
  if (!Check(&run, ConfigureExportArea(state, *artwork),
             "Document did not contain an export area to configure.")) {
    return run;
  }

  SelectAllImportedElements(state, *artwork);
  const SvgExportResult selection = ExportSelection(state, artwork->id);
  const SvgExportResult area = ExportArea(state, state.export_areas.front().id);

  Check(&run, selection.success,
        "Selection export failed: " + selection.message);
  Check(&run, area.success, "Export-area export failed: " + area.message);
  if (!selection.success || !area.success) {
    return run;
  }

  Check(&run, selection.cubic_segment_count > 0,
        "Expected cubic SVG segments to survive export.");
  Check(&run, HasCubicCommand(selection.svg),
        "Expected exported SVG markup to contain cubic path commands.");
  Check(&run, HasNonScalingStroke(selection.svg),
        "Expected exported SVG paths to use non-scaling preview strokes.");
  Check(&run, selection.cubic_segment_count == area.cubic_segment_count,
        "Selection and export-area exports disagreed on cubic segment count.");
  Check(&run, selection.path_count == area.path_count,
        "Selection and export-area exports disagreed on path count.");
  return run;
}

TestRun RunPlaceholderPolicyRegression() {
  TestRun run{"Placeholder DXF text policy"};
  CanvasState state = MakeDocument();

  ImportedArtwork artwork;
  artwork.name = "Placeholder Policy";
  artwork.source_format = "DXF";

  ImportedDxfText text;
  text.id = 1;
  text.label = "TEXT";
  text.source_text = "PLACEHOLDER";
  text.placeholder_only = true;
  text.issue_flags = ImportedElementIssueFlagPlaceholderText;
  text.placeholder_contours.push_back(
      MakeRectangleContour(0.0f, 0.0f, 40.0f, 12.0f, "Placeholder Bounds"));
  text.bounds_min = ImVec2(0.0f, 0.0f);
  text.bounds_max = ImVec2(40.0f, 12.0f);
  artwork.dxf_text.push_back(std::move(text));

  const int artwork_id = AppendImportedArtwork(state, std::move(artwork));
  const ImportedArtwork *stored_artwork =
      FindImportedArtwork(state, artwork_id);
  if (!Check(&run, stored_artwork != nullptr,
             "Synthetic placeholder artwork was not added to canvas state.")) {
    return run;
  }
  if (!Check(&run, ConfigureExportArea(state, *stored_artwork),
             "Document did not contain an export area to configure.")) {
    return run;
  }

  SelectAllImportedElements(state, *stored_artwork);

  SvgExportRequest strict_request;
  strict_request.scope = SvgExportScope::ActiveSelection;
  strict_request.imported_artwork_id = artwork_id;
  const SvgExportResult strict_result = ExportSvg(state, strict_request);
  Check(&run, !strict_result.success,
        "Strict export should block placeholder-only DXF text.");
  Check(&run, strict_result.placeholder_text_count == 1,
        "Strict export should count the blocked placeholder text item.");
  Check(&run, strict_result.svg.empty(),
        "Blocked export should not emit SVG content.");
  Check(&run, strict_result.warning_count > 0,
        "Blocked export should surface a warning explaining the policy.");
  Check(&run,
        strict_result.message.find("Export blocked:") != std::string::npos,
        "Blocked export should report an explicit blocking message.");

  SvgExportRequest allowed_request = strict_request;
  allowed_request.allow_placeholder_text = true;
  const SvgExportResult allowed_result = ExportSvg(state, allowed_request);
  Check(&run, allowed_result.success,
        "Explicitly allowed placeholder export should succeed.");
  if (!allowed_result.success) {
    return run;
  }
  Check(&run, allowed_result.placeholder_text_count == 1,
        "Allowed placeholder export should preserve the placeholder count.");
  Check(&run,
        allowed_result.svg.find("data-im2d-placeholder=\"true\"") !=
            std::string::npos,
        "Allowed placeholder export should mark placeholder contours in SVG.");
  return run;
}

TestRun RunFidelityFirstPreservesUntouchedContoursRegression() {
  TestRun run{"Fidelity-first preserves untouched contours"};
  CanvasState state = MakeDocument();

  ImportedArtwork artwork;
  artwork.name = "Fidelity First Preserve";
  artwork.source_format = "DXF";
  artwork.paths.push_back(
      MakePath(1,
               {
                   MakeLineSegment(ImVec2(0.0f, 0.0f), ImVec2(10.0f, 0.0f)),
                   MakeLineSegment(ImVec2(10.0f, 0.0f), ImVec2(10.0f, 10.0f)),
                   MakeLineSegment(ImVec2(10.0f, 10.0f), ImVec2(0.0f, 10.0f)),
                   MakeLineSegment(ImVec2(0.0f, 10.0f), ImVec2(0.0f, 0.0f)),
               },
               true, "Preserved Rectangle"));
  artwork.paths.push_back(
      MakePath(2,
               {
                   MakeLineSegment(ImVec2(20.0f, 0.0f), ImVec2(30.0f, 0.0f)),
                   MakeLineSegment(ImVec2(30.0f, 0.0f), ImVec2(30.0f, 10.0f)),
                   MakeLineSegment(ImVec2(30.0f, 10.0f), ImVec2(20.0f, 0.3f)),
               },
               false, "Repair Candidate"));

  const int artwork_id = AppendImportedArtwork(state, std::move(artwork));
  const ImportedArtworkOperationResult prepare =
      PrepareImportedArtworkForCutting(
          state, artwork_id, 0.5f, ImportedArtworkPrepareMode::FidelityFirst);
  Check(&run, prepare.success,
        "Prepare-for-cutting failed for synthetic fidelity-first artwork.");
  Check(&run, prepare.preserved_count == 1,
        "Expected preserved_count=1 but got preserved_count=" +
            std::to_string(prepare.preserved_count) +
            ", cleaned_count=" + std::to_string(prepare.cleaned_count) +
            ", stitched_count=" + std::to_string(prepare.stitched_count) +
            ", closed_count=" + std::to_string(prepare.closed_count) +
            ", open_count=" + std::to_string(prepare.open_count) + ".");
  Check(&run, prepare.cleaned_count == 1,
        "Expected cleaned_count=1 but got preserved_count=" +
            std::to_string(prepare.preserved_count) +
            ", cleaned_count=" + std::to_string(prepare.cleaned_count) +
            ", stitched_count=" + std::to_string(prepare.stitched_count) +
            ", closed_count=" + std::to_string(prepare.closed_count) +
            ", open_count=" + std::to_string(prepare.open_count) + ".");
  if (!prepare.success) {
    return run;
  }

  const ImportedArtwork *prepared_artwork =
      FindImportedArtwork(state, artwork_id);
  if (!Check(&run, prepared_artwork != nullptr,
             "Prepared synthetic artwork was not available after prepare.")) {
    return run;
  }

  const im2d::ImportedPath *preserved_path = FindPathById(*prepared_artwork, 1);
  Check(&run, preserved_path != nullptr,
        "Fidelity-first prepare should not replace untouched closed contours.");
  if (preserved_path != nullptr) {
    Check(&run, preserved_path->closed,
          "The preserved contour should remain closed after prepare.");
    Check(&run, preserved_path->segments.size() == 4,
          "The preserved contour should keep its original segment count.");
  }
  return run;
}

TestRun RunAutoCloseToPolylineRegression() {
  TestRun run{"Auto close to polyline"};
  CanvasState state = MakeDocument();

  ImportedArtwork artwork;
  artwork.name = "Auto Close Polyline";
  artwork.source_format = "DXF";
  artwork.paths.push_back(
      MakePath(1,
               {
                   MakeLineSegment(ImVec2(0.0f, 0.0f), ImVec2(10.0f, 0.0f)),
                   MakeLineSegment(ImVec2(10.0f, 0.0f), ImVec2(10.0f, 10.0f)),
                   MakeLineSegment(ImVec2(10.0f, 10.0f), ImVec2(0.0f, 0.2f)),
               },
               false, "Almost Closed"));

  const int artwork_id = AppendImportedArtwork(state, std::move(artwork));
  const ImportedArtworkOperationResult close_result =
      AutoCloseImportedArtworkToPolyline(state, artwork_id, 0.5f);
  Check(&run, close_result.success,
        "Auto-close-to-polyline failed: " + close_result.message);
  Check(&run, close_result.closed_count == 1,
        "Expected auto-close-to-polyline to produce one closed contour.");
  Check(&run, close_result.open_count == 0,
        "Expected auto-close-to-polyline to leave no open contours.");
  Check(&run, close_result.auto_close_pass_count >= 1,
        "Expected auto-close profiling to record at least one graph pass.");
  Check(&run, close_result.auto_close_elapsed_ms >= 0.0f,
        "Expected auto-close profiling to populate elapsed time.");
  if (!close_result.success) {
    return run;
  }

  const ImportedArtwork *closed_artwork =
      FindImportedArtwork(state, artwork_id);
  if (!Check(&run, closed_artwork != nullptr,
             "Closed artwork was not available after auto-close.")) {
    return run;
  }

  const im2d::ImportedPath *closed_path = FindPathById(*closed_artwork, 1);
  Check(&run, closed_path != nullptr,
        "Auto-close-to-polyline should keep the original path id.");
  if (closed_path != nullptr) {
    Check(&run, closed_path->closed,
          "Auto-close-to-polyline should mark the path closed.");
    Check(&run,
          std::all_of(closed_path->segments.begin(),
                      closed_path->segments.end(),
                      [](const ImportedPathSegment &segment) {
                        return segment.kind == ImportedPathSegmentKind::Line;
                      }),
          "Auto-close-to-polyline should emit only line segments.");
  }
  return run;
}

TestRun RunAutoClosePreservesClosedCubicRegression() {
  TestRun run{"Auto close preserves closed cubic"};
  CanvasState state = MakeDocument();

  ImportedArtwork artwork;
  artwork.name = "Auto Close Preserve Cubic";
  artwork.source_format = "DXF";
  artwork.paths.push_back(
      MakePath(1,
               {MakeCubicSegment(ImVec2(0.0f, 0.0f), ImVec2(0.0f, 5.0f),
                                 ImVec2(10.0f, 5.0f), ImVec2(10.0f, 0.0f))},
               true, "Closed Cubic"));

  const int artwork_id = AppendImportedArtwork(state, std::move(artwork));
  const ImportedArtworkOperationResult close_result =
      AutoCloseImportedArtworkToPolyline(state, artwork_id, 0.5f);
  Check(&run, close_result.success,
        "Auto-close should leave pre-closed cubic contours valid: " +
            close_result.message);
  if (!close_result.success) {
    return run;
  }

  const ImportedArtwork *closed_artwork =
      FindImportedArtwork(state, artwork_id);
  if (!Check(&run, closed_artwork != nullptr,
             "Closed cubic artwork was not available after auto-close.")) {
    return run;
  }

  const im2d::ImportedPath *closed_path = FindPathById(*closed_artwork, 1);
  Check(&run, closed_path != nullptr,
        "Auto-close should preserve the pre-closed cubic path.");
  if (closed_path != nullptr) {
    Check(&run, closed_path->closed,
          "Pre-closed cubic path should remain closed after auto-close.");
    Check(&run, closed_path->segments.size() == 1,
          "Pre-closed cubic path should keep its original segment count.");
    Check(&run,
          closed_path->segments.front().kind ==
              ImportedPathSegmentKind::CubicBezier,
          "Pre-closed cubic path should keep its cubic segment.");
  }

  return run;
}

TestRun RunAutoCloseReconstructsChainRegression() {
  TestRun run{"Auto close reconstructs chain"};
  CanvasState state = MakeDocument();

  ImportedArtwork artwork;
  artwork.name = "Auto Close Chain";
  artwork.source_format = "DXF";
  artwork.paths.push_back(
      MakePath(1, {MakeLineSegment(ImVec2(10.0f, 10.0f), ImVec2(0.0f, 10.0f))},
               false, "Top"));
  artwork.paths.push_back(
      MakePath(2, {MakeLineSegment(ImVec2(10.0f, 0.0f), ImVec2(10.0f, 10.0f))},
               false, "Right"));
  artwork.paths.push_back(
      MakePath(3, {MakeLineSegment(ImVec2(0.0f, 0.0f), ImVec2(10.0f, 0.0f))},
               false, "Bottom"));
  artwork.paths.push_back(
      MakePath(4, {MakeLineSegment(ImVec2(0.0f, 10.0f), ImVec2(0.0f, 0.0f))},
               false, "Left"));

  const int artwork_id = AppendImportedArtwork(state, std::move(artwork));
  const ImportedArtworkOperationResult close_result =
      AutoCloseImportedArtworkToPolyline(state, artwork_id, 0.01f);
  Check(&run, close_result.success,
        "Auto-close chain reconstruction failed: " + close_result.message);
  Check(&run, close_result.closed_count == 1,
        "Expected one reconstructed closed contour from four open edges.");
  Check(&run, close_result.open_count == 0,
        "Expected no unresolved edges in the rectangle chain regression.");
  Check(&run, close_result.stitched_count == 3,
        "Expected three stitch operations when reconstructing four edges into "
        "one loop.");
  if (!close_result.success) {
    return run;
  }

  const ImportedArtwork *closed_artwork =
      FindImportedArtwork(state, artwork_id);
  if (!Check(&run, closed_artwork != nullptr,
             "Reconstructed artwork was not available after auto-close.")) {
    return run;
  }

  Check(&run, closed_artwork->paths.size() == 1,
        "Expected the reconstructed chain to collapse into one path.");
  if (closed_artwork->paths.size() == 1) {
    Check(&run, closed_artwork->paths.front().segments.size() == 4,
          "Expected reconstructed rectangle to contain four line segments.");
  }
  return run;
}

TestRun RunAutoCloseRepairsOrthogonalCornerRegression() {
  TestRun run{"Auto close repairs orthogonal corner"};
  CanvasState state = MakeDocument();

  ImportedArtwork artwork;
  artwork.name = "Auto Close Orthogonal Corner";
  artwork.source_format = "DXF";
  artwork.paths.push_back(
      MakePath(1, {MakeLineSegment(ImVec2(0.0f, 10.0f), ImVec2(0.0f, 0.2f))},
               false, "Vertical"));
  artwork.paths.push_back(
      MakePath(2, {MakeLineSegment(ImVec2(0.2f, 0.0f), ImVec2(10.0f, 0.0f))},
               false, "Horizontal"));

  const int artwork_id = AppendImportedArtwork(state, std::move(artwork));
  const ImportedArtworkOperationResult close_result =
      AutoCloseImportedArtworkToPolyline(state, artwork_id, 0.5f);
  Check(&run, close_result.success,
        "Auto-close orthogonal corner failed: " + close_result.message);
  Check(&run, close_result.stitched_count == 1,
        "Expected one stitch for orthogonal corner repair.");
  Check(&run, close_result.open_count == 1,
        "Expected orthogonal corner repair to produce one open chain.");
  if (!close_result.success) {
    return run;
  }

  const ImportedArtwork *closed_artwork =
      FindImportedArtwork(state, artwork_id);
  if (!Check(&run, closed_artwork != nullptr,
             "Orthogonal-corner artwork missing after auto-close.")) {
    return run;
  }
  if (!Check(&run, closed_artwork->paths.size() == 1,
             "Expected orthogonal-corner repair to collapse into one path.")) {
    return run;
  }

  const im2d::ImportedPath &path = closed_artwork->paths.front();
  if (!Check(&run, path.segments.size() == 2,
             "Expected orthogonal-corner repair to preserve two segments.")) {
    return run;
  }
  const ImVec2 expected_join(0.0f, 0.0f);
  Check(&run,
        NearlyEqual(path.segments.front().end.x, expected_join.x, 0.01f) &&
            NearlyEqual(path.segments.front().end.y, expected_join.y, 0.01f),
        "Expected vertical segment to extend to the inferred corner.");
  Check(&run,
        NearlyEqual(path.segments.back().start.x, expected_join.x, 0.01f) &&
            NearlyEqual(path.segments.back().start.y, expected_join.y, 0.01f),
        "Expected horizontal segment to begin at the inferred corner.");
  return run;
}

TestRun RunPrepareRepairsCubicLineCornerRegression() {
  TestRun run{"Prepare repairs cubic-line corner"};
  CanvasState state = MakeDocument();

  ImportedArtwork artwork;
  artwork.name = "Prepare Cubic Corner";
  artwork.source_format = "DXF";
  artwork.paths.push_back(
      MakePath(1,
               {MakeCubicSegment(ImVec2(0.0f, 10.0f), ImVec2(0.0f, 7.0f),
                                 ImVec2(0.0f, 3.0f), ImVec2(0.0f, 0.2f))},
               false, "Cubic Vertical"));
  artwork.paths.push_back(
      MakePath(2, {MakeLineSegment(ImVec2(0.2f, 0.0f), ImVec2(10.0f, 0.0f))},
               false, "Horizontal"));

  const int artwork_id = AppendImportedArtwork(state, std::move(artwork));
  const ImportedArtworkOperationResult prepare_result =
      PrepareImportedArtworkForCutting(
          state, artwork_id, 0.5f, ImportedArtworkPrepareMode::FidelityFirst,
          false);
  Check(&run, prepare_result.success,
        "Prepare cubic-line corner failed: " + prepare_result.message);
  Check(&run, prepare_result.stitched_count == 1,
        "Expected one stitch for cubic-line corner repair.");
  Check(&run, prepare_result.open_count == 1,
        "Expected cubic-line corner repair to produce one open chain.");
  if (!prepare_result.success) {
    return run;
  }

  const ImportedArtwork *prepared_artwork =
      FindImportedArtwork(state, artwork_id);
  if (!Check(&run, prepared_artwork != nullptr,
             "Prepared cubic-line artwork missing after prepare.")) {
    return run;
  }
  if (!Check(&run, prepared_artwork->paths.size() == 1,
             "Expected cubic-line repair to collapse into one path.")) {
    return run;
  }

  const im2d::ImportedPath &path = prepared_artwork->paths.front();
  if (!Check(&run, path.segments.size() == 2,
             "Expected cubic-line repair to preserve two segments.")) {
    return run;
  }
  const int cubic_count = static_cast<int>(std::count_if(
      path.segments.begin(), path.segments.end(),
      [](const ImportedPathSegment &segment) {
        return segment.kind == ImportedPathSegmentKind::CubicBezier;
      }));
  const int line_count = static_cast<int>(
      std::count_if(path.segments.begin(), path.segments.end(),
                    [](const ImportedPathSegment &segment) {
                      return segment.kind == ImportedPathSegmentKind::Line;
                    }));
  Check(&run, cubic_count == 1,
        "Expected cubic-line repair to keep exactly one cubic segment.");
  Check(&run, line_count == 1,
        "Expected cubic-line repair to keep exactly one line segment.");

  const ImVec2 expected_join(0.0f, 0.0f);
  Check(
      &run,
      NearlyEqual(path.segments.front().end.x, expected_join.x, 0.01f) &&
          NearlyEqual(path.segments.front().end.y, expected_join.y, 0.01f) &&
          NearlyEqual(path.segments.back().start.x, expected_join.x, 0.01f) &&
          NearlyEqual(path.segments.back().start.y, expected_join.y, 0.01f),
      "Expected the stitched cubic-line path to meet at the inferred corner.");
  return run;
}

TestRun RunAutoCloseCollapsesOpenChainRegression() {
  TestRun run{"Auto close collapses open chain"};
  CanvasState state = MakeDocument();

  ImportedArtwork artwork;
  artwork.name = "Auto Close Open Chain";
  artwork.source_format = "DXF";
  artwork.paths.push_back(
      MakePath(1, {MakeLineSegment(ImVec2(0.0f, 0.0f), ImVec2(10.0f, 0.0f))},
               false, "First"));
  artwork.paths.push_back(
      MakePath(2, {MakeLineSegment(ImVec2(10.0f, 0.0f), ImVec2(20.0f, 0.0f))},
               false, "Second"));
  artwork.paths.push_back(
      MakePath(3, {MakeLineSegment(ImVec2(20.0f, 0.0f), ImVec2(30.0f, 0.0f))},
               false, "Third"));

  const int artwork_id = AppendImportedArtwork(state, std::move(artwork));
  const ImportedArtworkOperationResult close_result =
      AutoCloseImportedArtworkToPolyline(state, artwork_id, 0.01f);
  Check(&run, close_result.success,
        "Auto-close open chain failed: " + close_result.message);
  Check(&run, close_result.stitched_count == 2,
        "Expected two stitch operations when collapsing a three-edge open "
        "chain.");
  Check(&run, close_result.closed_count == 0,
        "Open chain regression should not create a closed contour.");
  Check(&run, close_result.open_count == 1,
        "Open chain regression should collapse to one open contour.");
  Check(&run, close_result.auto_close_component_count >= 1,
        "Expected auto-close profiling to count at least one chain component.");
  if (!close_result.success) {
    return run;
  }

  const ImportedArtwork *collapsed_artwork =
      FindImportedArtwork(state, artwork_id);
  if (!Check(
          &run, collapsed_artwork != nullptr,
          "Collapsed open-chain artwork was not available after auto-close.")) {
    return run;
  }

  Check(&run, collapsed_artwork->paths.size() == 1,
        "Expected the open chain to collapse into one path.");
  if (collapsed_artwork->paths.size() == 1) {
    Check(&run, !collapsed_artwork->paths.front().closed,
          "Collapsed open chain should remain open.");
    Check(&run, collapsed_artwork->paths.front().segments.size() == 3,
          "Collapsed open chain should contain three line segments.");
  }
  return run;
}

TestRun RunPreparePrefersAlignedBranchRegression() {
  TestRun run{"Prepare prefers aligned branch"};
  CanvasState state = MakeDocument();

  ImportedArtwork artwork;
  artwork.name = "Prepare Branch Choice";
  artwork.source_format = "DXF";
  artwork.paths.push_back(
      MakePath(1, {MakeLineSegment(ImVec2(0.0f, 0.0f), ImVec2(10.0f, 0.0f))},
               false, "Stem"));
  artwork.paths.push_back(
      MakePath(2, {MakeLineSegment(ImVec2(10.2f, 0.0f), ImVec2(20.0f, 0.0f))},
               false, "Aligned Branch"));
  artwork.paths.push_back(
      MakePath(3, {MakeLineSegment(ImVec2(10.2f, 0.0f), ImVec2(18.0f, 4.0f))},
               false, "Angled Branch"));

  const int artwork_id = AppendImportedArtwork(state, std::move(artwork));
  const ImportedArtworkOperationResult prepare_result =
      PrepareImportedArtworkForCutting(
          state, artwork_id, 0.5f, ImportedArtworkPrepareMode::FidelityFirst,
          false);
  Check(&run, prepare_result.success,
        "Prepare branch-choice regression failed: " + prepare_result.message);
  Check(&run, prepare_result.stitched_count == 1,
        "Expected one stitch in the branch-choice regression.");
  Check(&run, prepare_result.open_count == 2,
        "Expected branch-choice regression to leave two open contours.");
  if (!prepare_result.success) {
    return run;
  }

  const ImportedArtwork *prepared_artwork =
      FindImportedArtwork(state, artwork_id);
  if (!Check(&run, prepared_artwork != nullptr,
             "Prepared branch-choice artwork missing after prepare.")) {
    return run;
  }
  Check(&run, prepared_artwork->paths.size() == 2,
        "Expected aligned merge to leave two paths in the artwork.");
  if (prepared_artwork->paths.size() != 2) {
    return run;
  }

  const im2d::ImportedPath *merged_path = nullptr;
  for (const im2d::ImportedPath &path : prepared_artwork->paths) {
    if (path.segments.size() == 2) {
      merged_path = &path;
      break;
    }
  }
  if (!Check(&run, merged_path != nullptr,
             "Expected a merged two-segment path after aligned branch "
             "selection.")) {
    return run;
  }

  const ImVec2 merged_start = merged_path->segments.front().start;
  const ImVec2 merged_end = merged_path->segments.back().end;
  const bool forward = NearlyEqual(merged_start.x, 0.0f, 0.01f) &&
                       NearlyEqual(merged_start.y, 0.0f, 0.01f) &&
                       NearlyEqual(merged_end.x, 20.0f, 0.01f) &&
                       NearlyEqual(merged_end.y, 0.0f, 0.01f);
  const bool reverse = NearlyEqual(merged_start.x, 20.0f, 0.01f) &&
                       NearlyEqual(merged_start.y, 0.0f, 0.01f) &&
                       NearlyEqual(merged_end.x, 0.0f, 0.01f) &&
                       NearlyEqual(merged_end.y, 0.0f, 0.01f);
  Check(&run, forward || reverse,
        "Expected prepare to prefer the straight continuation over the angled "
        "branch.");
  return run;
}

TestRun RunAutoCloseRejectsAmbiguousEndpointRegression() {
  TestRun run{"Auto close rejects ambiguous endpoint"};
  CanvasState state = MakeDocument();

  ImportedArtwork artwork;
  artwork.name = "Auto Close Ambiguous";
  artwork.source_format = "DXF";
  artwork.paths.push_back(
      MakePath(1, {MakeLineSegment(ImVec2(0.0f, 0.0f), ImVec2(10.0f, 0.0f))},
               false, "Stem"));
  artwork.paths.push_back(
      MakePath(2, {MakeLineSegment(ImVec2(10.2f, 0.0f), ImVec2(20.0f, 0.0f))},
               false, "Branch A"));
  artwork.paths.push_back(
      MakePath(3, {MakeLineSegment(ImVec2(10.2f, 0.0f), ImVec2(20.0f, 5.0f))},
               false, "Branch B"));

  const int artwork_id = AppendImportedArtwork(state, std::move(artwork));
  const ImportedArtworkOperationResult close_result =
      AutoCloseImportedArtworkToPolyline(state, artwork_id, 0.5f);
  Check(&run, close_result.success,
        "Auto-close ambiguous endpoint regression failed: " +
            close_result.message);
  Check(&run, close_result.stitched_count == 0,
        "Ambiguous endpoint should not be stitched automatically.");
  Check(&run, close_result.closed_count == 0,
        "Ambiguous endpoint should not produce a closed contour.");
  Check(&run, close_result.open_count == 3,
        "Ambiguous endpoint should leave all fragments open for inspection.");
  return run;
}

TestRun RunTyrannosaurusWorkflowRegression(const fs::path &project_root) {
  TestRun run{"Tyrannosaurus open-path workflow"};
  const fs::path file_path = project_root / "samples/dxf/Tyrannosaurus.DXF";

  TraceRegressionStep("Tyrannosaurus: baseline import");
  CanvasState baseline_state = MakeDocument();
  const ImportResult baseline_import = ImportDxfFile(baseline_state, file_path);
  if (!Check(&run, baseline_import.success,
             "Baseline DXF import failed: " + baseline_import.message)) {
    return run;
  }
  const ImportedArtwork *baseline_artwork =
      FindImportedArtwork(baseline_state, baseline_import.artwork_id);
  if (!Check(&run, baseline_artwork != nullptr,
             "Baseline Tyrannosaurus artwork missing after import.")) {
    return run;
  }
  const int baseline_open = baseline_artwork->part.open_contour_count;
  const int baseline_closed = baseline_artwork->part.closed_contour_count;
  if (!Check(&run, ConfigureExportArea(baseline_state, *baseline_artwork),
             "Could not configure export area for baseline Tyrannosaurus.")) {
    return run;
  }
  SelectAllImportedElements(baseline_state, *baseline_artwork);
  const SvgExportResult baseline_export =
      ExportSelection(baseline_state, baseline_artwork->id);
  if (!Check(&run, baseline_export.success,
             "Baseline Tyrannosaurus export failed: " +
                 baseline_export.message)) {
    return run;
  }

  Check(&run, baseline_open > 0,
        "Expected Tyrannosaurus import to contain open contours.");

  TraceRegressionStep("Tyrannosaurus: auto-close import");
  CanvasState auto_close_state = MakeDocument();
  const ImportResult auto_close_import =
      ImportDxfFile(auto_close_state, file_path);
  if (!Check(&run, auto_close_import.success,
             "Auto-close Tyrannosaurus import failed: " +
                 auto_close_import.message)) {
    return run;
  }
  const ImportedArtworkOperationResult auto_close_result =
      AutoCloseImportedArtworkToPolyline(auto_close_state,
                                         auto_close_import.artwork_id, 0.5f);
  TraceRegressionStep("Tyrannosaurus: auto-close complete");
  Check(&run, auto_close_result.success,
        "Auto-close Tyrannosaurus failed: " + auto_close_result.message);
  Check(&run, auto_close_result.closed_count > baseline_closed,
        "Auto-close Tyrannosaurus should increase closed contours. Baseline=" +
            std::to_string(baseline_closed) +
            ", after=" + std::to_string(auto_close_result.closed_count) + ".");
  Check(&run, auto_close_result.open_count < baseline_open,
        "Auto-close Tyrannosaurus should reduce open contours. Baseline=" +
            std::to_string(baseline_open) +
            ", after=" + std::to_string(auto_close_result.open_count) + ".");
  Check(&run, auto_close_result.auto_close_endpoint_count > 0,
        "Expected Tyrannosaurus auto-close profiling to report endpoints.");
  Check(&run, auto_close_result.auto_close_cluster_count > 0,
        "Expected Tyrannosaurus auto-close profiling to report clusters.");
  Check(&run, auto_close_result.auto_close_pass_count >= 1,
        "Expected Tyrannosaurus auto-close profiling to report graph passes.");

  TraceRegressionStep("Tyrannosaurus: prepare-only import");
  CanvasState prepare_state = MakeDocument();
  const ImportResult prepare_import = ImportDxfFile(prepare_state, file_path);
  if (!Check(&run, prepare_import.success,
             "Prepare-only Tyrannosaurus import failed: " +
                 prepare_import.message)) {
    return run;
  }
  const ImportedArtworkOperationResult prepare_result =
      PrepareImportedArtworkForCutting(
          prepare_state, prepare_import.artwork_id, 0.5f,
          ImportedArtworkPrepareMode::FidelityFirst, false);
  TraceRegressionStep("Tyrannosaurus: prepare-only complete");
  Check(&run, prepare_result.success,
        "Prepare-only Tyrannosaurus failed: " + prepare_result.message);

  TraceRegressionStep("Tyrannosaurus: prepare+auto-close import");
  CanvasState prepare_auto_close_state = MakeDocument();
  const ImportResult prepare_auto_close_import =
      ImportDxfFile(prepare_auto_close_state, file_path);
  if (!Check(&run, prepare_auto_close_import.success,
             "Prepare+auto-close Tyrannosaurus import failed: " +
                 prepare_auto_close_import.message)) {
    return run;
  }
  const ImportedArtworkOperationResult prepare_auto_close_result =
      PrepareImportedArtworkForCutting(
          prepare_auto_close_state, prepare_auto_close_import.artwork_id, 0.5f,
          ImportedArtworkPrepareMode::FidelityFirst, true);
  TraceRegressionStep("Tyrannosaurus: prepare+auto-close complete");
  Check(&run, prepare_auto_close_result.success,
        "Prepare+auto-close Tyrannosaurus failed: " +
            prepare_auto_close_result.message);
  const int allowed_closed_delta = 3;
  Check(&run,
        prepare_auto_close_result.closed_count + allowed_closed_delta >=
            prepare_result.closed_count,
        "Prepare+auto-close Tyrannosaurus should stay within " +
            std::to_string(allowed_closed_delta) +
            " closed contours of legacy prepare while keeping the safer "
            "auto-close stage. Prepare=" +
            std::to_string(prepare_result.closed_count) +
            ", Prepare+AutoClose=" +
            std::to_string(prepare_auto_close_result.closed_count) + ".");
  Check(&run, prepare_auto_close_result.open_count <= prepare_result.open_count,
        "Prepare+auto-close Tyrannosaurus should not increase open contours. "
        "Prepare=" +
            std::to_string(prepare_result.open_count) + ", Prepare+AutoClose=" +
            std::to_string(prepare_auto_close_result.open_count) + ".");

  const ImportedArtwork *prepared_auto_close_artwork = FindImportedArtwork(
      prepare_auto_close_state, prepare_auto_close_import.artwork_id);
  if (!Check(
          &run, prepared_auto_close_artwork != nullptr,
          "Prepared Tyrannosaurus artwork missing after auto-close prepare.")) {
    return run;
  }
  if (!Check(&run,
             ConfigureExportArea(prepare_auto_close_state,
                                 *prepared_auto_close_artwork),
             "Could not configure export area for prepared Tyrannosaurus.")) {
    return run;
  }
  SelectAllImportedElements(prepare_auto_close_state,
                            *prepared_auto_close_artwork);
  const SvgExportResult prepared_export = ExportSelection(
      prepare_auto_close_state, prepared_auto_close_artwork->id);
  Check(&run, prepared_export.success,
        "Prepared Tyrannosaurus export failed: " + prepared_export.message);
  if (prepared_export.success) {
    Check(&run,
          NearlyEqual(BoundsWidth(baseline_export),
                      BoundsWidth(prepared_export), 1.0f) &&
              NearlyEqual(BoundsHeight(baseline_export),
                          BoundsHeight(prepared_export), 1.0f),
          "Prepared Tyrannosaurus should preserve overall export bounds. "
          "Baseline=" +
              std::to_string(BoundsWidth(baseline_export)) + "x" +
              std::to_string(BoundsHeight(baseline_export)) +
              ", Prepared=" + std::to_string(BoundsWidth(prepared_export)) +
              "x" + std::to_string(BoundsHeight(prepared_export)) + ".");
  }

  return run;
}

} // namespace

int main() {
  const fs::path project_root = fs::current_path();
  std::vector<TestRun> results;
  results.push_back(RunPlaceholderPolicyRegression());
  results.push_back(RunAutoCloseToPolylineRegression());
  results.push_back(RunAutoClosePreservesClosedCubicRegression());
  results.push_back(RunAutoCloseReconstructsChainRegression());
  results.push_back(RunAutoCloseRepairsOrthogonalCornerRegression());
  results.push_back(RunPrepareRepairsCubicLineCornerRegression());
  results.push_back(RunPreparePrefersAlignedBranchRegression());
  results.push_back(RunAutoCloseCollapsesOpenChainRegression());
  results.push_back(RunAutoCloseRejectsAmbiguousEndpointRegression());
  results.push_back(RunTyrannosaurusWorkflowRegression(project_root));
  results.push_back(RunFidelityFirstPreservesUntouchedContoursRegression());
  results.push_back(RunDxfInchUnitScaleRegression(project_root));
  results.push_back(RunShapefontRegression(project_root));
  results.push_back(RunAutobotAutoCloseWorkflowRegression(project_root));
  results.push_back(RunAutobotPrepareRegression(project_root));
  results.push_back(RunSvgCubicRegression(project_root));

  int failures = 0;
  for (const TestRun &result : results) {
    if (result.failures == 0) {
      std::cout << "[PASS] " << result.name << '\n';
      continue;
    }
    std::cout << "[FAIL] " << result.name << '\n';
    for (const std::string &message : result.messages) {
      std::cout << "  - " << message << '\n';
    }
    failures += result.failures;
  }

  if (failures == 0) {
    std::cout << "All export regression checks passed." << std::endl;
    return 0;
  }

  std::cout << failures << " regression check(s) failed." << std::endl;
  return 1;
}