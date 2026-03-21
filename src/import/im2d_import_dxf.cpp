#include "im2d_import.h"

#include "../canvas/im2d_canvas_document.h"
#include "../common/im2d_log.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

#include <libdxfrw.h>

namespace im2d::importer {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kEpsilon = 1e-6;
constexpr int kMaxImported3dFaces = 20000;
constexpr int kPolylineFlagClosed = 0x01;
constexpr int kPolylineFlagPolyfaceMesh = 0x40;
constexpr unsigned int kDxfTextPlaceholderColor = 0x00ff01ffu;
constexpr unsigned int kDxfFilledTextColor = 0x00ff02ffu;

std::string FormatNumber(double value) {
  std::ostringstream stream;
  stream << std::fixed << std::setprecision(3) << value;
  return stream.str();
}

std::string SvgHexColor(unsigned int color) {
  std::ostringstream stream;
  stream << '#' << std::hex << std::setw(6) << std::setfill('0')
         << (color & 0x00ffffffu);
  return stream.str();
}

std::string StrokeColorForEntity(const DRW_Entity &entity) {
  if (entity.color24 >= 0) {
    const unsigned int color = static_cast<unsigned int>(entity.color24);
    std::ostringstream stream;
    stream << '#' << std::hex << std::setw(6) << std::setfill('0')
           << (color & 0x00ffffffu);
    return stream.str();
  }

  static constexpr const char *kIndexedColors[] = {
      "#000000", "#ff0000", "#ffff00", "#00ff00", "#00ffff",
      "#0000ff", "#ff00ff", "#ffffff", "#808080", "#c0c0c0"};
  if (entity.color >= 0 &&
      entity.color < static_cast<int>(sizeof(kIndexedColors) /
                                      sizeof(kIndexedColors[0]))) {
    return kIndexedColors[entity.color];
  }

  return "#1d1d1d";
}

double StrokeWidthForEntity(const DRW_Entity &entity) {
  if (entity.lWeight == DRW_LW_Conv::widthByLayer ||
      entity.lWeight == DRW_LW_Conv::widthByBlock ||
      entity.lWeight == DRW_LW_Conv::widthDefault) {
    return 1.0;
  }

  const int hundredths_of_mm = DRW_LW_Conv::lineWidth2dxfInt(entity.lWeight);
  if (hundredths_of_mm <= 0) {
    return 1.0;
  }

  return std::clamp(static_cast<double>(hundredths_of_mm) / 35.0, 0.75, 6.0);
}

double NormalizeArcDelta(double start_angle, double end_angle) {
  double delta = end_angle - start_angle;
  while (delta <= 0.0) {
    delta += 2.0 * kPi;
  }
  return delta;
}

std::string NormalizeDxfText(const std::string &text) {
  std::string normalized;
  normalized.reserve(text.size());
  for (size_t index = 0; index < text.size(); ++index) {
    const char character = text[index];
    if (character == '%' && index + 2 < text.size() && text[index + 1] == '%') {
      const char escape = static_cast<char>(
          std::tolower(static_cast<unsigned char>(text[index + 2])));
      if (escape == 'd') {
        normalized += "\xC2\xB0";
        index += 2;
        continue;
      }
      if (escape == 'p') {
        normalized += "\xC2\xB1";
        index += 2;
        continue;
      }
      if (escape == 'c') {
        normalized += "\xC3\x98";
        index += 2;
        continue;
      }
      if (escape == 'u') {
        index += 2;
        continue;
      }
    }

    if (character == '\\' && index + 1 < text.size()) {
      const char escape = text[index + 1];
      if (escape == 'P' || escape == 'p') {
        normalized.push_back('\n');
        index += 1;
        continue;
      }

      if (escape == '~') {
        normalized.push_back(' ');
        index += 1;
        continue;
      }

      if (escape == '\\' || escape == '{' || escape == '}') {
        normalized.push_back(escape);
        index += 1;
        continue;
      }

      if (std::isalpha(static_cast<unsigned char>(escape)) != 0) {
        index += 1;
        while (index + 1 < text.size() && text[index + 1] != ';' &&
               text[index + 1] != '\\' && text[index + 1] != '{' &&
               text[index + 1] != '}') {
          index += 1;
        }
        if (index + 1 < text.size() && text[index + 1] == ';') {
          index += 1;
        }
        continue;
      }

      index += 1;
      continue;
    }

    if (character == '\n' || character == '\r') {
      normalized.push_back(' ');
      continue;
    }

    if (character == '{' || character == '}') {
      continue;
    }

    normalized.push_back(character);
  }
  return normalized;
}

std::filesystem::path ResolveVectorFontPath() {
  if (const char *override_path = std::getenv("IM2D_DXF_TEXT_FONT");
      override_path != nullptr && override_path[0] != '\0') {
    const std::filesystem::path candidate(override_path);
    std::error_code error;
    if (std::filesystem::exists(candidate, error)) {
      return candidate;
    }
  }

  static constexpr const char *kFontCandidates[] = {
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf",
      "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
      "/usr/share/fonts/TTF/DejaVuSans.ttf"};

  std::error_code error;
  for (const char *path : kFontCandidates) {
    const std::filesystem::path candidate(path);
    if (std::filesystem::exists(candidate, error)) {
      return candidate;
    }
  }

  return {};
}

std::vector<std::string> SplitTextLines(const std::string &text) {
  std::vector<std::string> lines;
  std::string current_line;
  for (const char character : text) {
    if (character == '\n') {
      lines.push_back(current_line);
      current_line.clear();
      continue;
    }
    current_line.push_back(character);
  }
  lines.push_back(current_line);
  return lines;
}

std::vector<uint32_t> DecodeUtf8(const std::string &text) {
  std::vector<uint32_t> codepoints;
  for (size_t index = 0; index < text.size();) {
    const unsigned char first = static_cast<unsigned char>(text[index]);
    if (first < 0x80) {
      codepoints.push_back(first);
      index += 1;
      continue;
    }

    uint32_t codepoint = 0;
    size_t extra_bytes = 0;
    if ((first & 0xE0u) == 0xC0u) {
      codepoint = first & 0x1Fu;
      extra_bytes = 1;
    } else if ((first & 0xF0u) == 0xE0u) {
      codepoint = first & 0x0Fu;
      extra_bytes = 2;
    } else if ((first & 0xF8u) == 0xF0u) {
      codepoint = first & 0x07u;
      extra_bytes = 3;
    } else {
      codepoints.push_back('?');
      index += 1;
      continue;
    }

    if (index + extra_bytes >= text.size()) {
      codepoints.push_back('?');
      break;
    }

    bool valid = true;
    for (size_t offset = 1; offset <= extra_bytes; ++offset) {
      const unsigned char continuation =
          static_cast<unsigned char>(text[index + offset]);
      if ((continuation & 0xC0u) != 0x80u) {
        valid = false;
        break;
      }
      codepoint =
          (codepoint << 6u) | static_cast<uint32_t>(continuation & 0x3Fu);
    }

    codepoints.push_back(valid ? codepoint : static_cast<uint32_t>('?'));
    index += extra_bytes + 1;
  }
  return codepoints;
}

enum class HorizontalTextAlignment {
  Left,
  Center,
  Right,
};

enum class VerticalTextAlignment {
  Baseline,
  Bottom,
  Middle,
  Top,
};

HorizontalTextAlignment HorizontalAlignmentForText(const DRW_Text &data,
                                                   bool multiline) {
  if (multiline) {
    switch (static_cast<int>(data.alignV)) {
    case DRW_MText::TopCenter:
    case DRW_MText::MiddleCenter:
    case DRW_MText::BottomCenter:
      return HorizontalTextAlignment::Center;
    case DRW_MText::TopRight:
    case DRW_MText::MiddleRight:
    case DRW_MText::BottomRight:
      return HorizontalTextAlignment::Right;
    default:
      return HorizontalTextAlignment::Left;
    }
  }

  switch (data.alignH) {
  case DRW_Text::HCenter:
  case DRW_Text::HMiddle:
    return HorizontalTextAlignment::Center;
  case DRW_Text::HRight:
  case DRW_Text::HAligned:
  case DRW_Text::HFit:
    return HorizontalTextAlignment::Right;
  default:
    return HorizontalTextAlignment::Left;
  }
}

VerticalTextAlignment VerticalAlignmentForText(const DRW_Text &data,
                                               bool multiline) {
  if (multiline) {
    switch (static_cast<int>(data.alignV)) {
    case DRW_MText::MiddleLeft:
    case DRW_MText::MiddleCenter:
    case DRW_MText::MiddleRight:
      return VerticalTextAlignment::Middle;
    case DRW_MText::BottomLeft:
    case DRW_MText::BottomCenter:
    case DRW_MText::BottomRight:
      return VerticalTextAlignment::Bottom;
    default:
      return VerticalTextAlignment::Top;
    }
  }

  switch (data.alignV) {
  case DRW_Text::VBottom:
    return VerticalTextAlignment::Bottom;
  case DRW_Text::VMiddle:
    return VerticalTextAlignment::Middle;
  case DRW_Text::VTop:
    return VerticalTextAlignment::Top;
  default:
    return VerticalTextAlignment::Baseline;
  }
}

class VectorGlyphFont {
public:
  VectorGlyphFont() {
    if (FT_Init_FreeType(&library_) != 0) {
      library_ = nullptr;
      return;
    }

    const std::filesystem::path font_path = ResolveVectorFontPath();
    if (font_path.empty()) {
      return;
    }

    if (FT_New_Face(library_, font_path.string().c_str(), 0, &face_) != 0) {
      face_ = nullptr;
      return;
    }

    font_path_ = font_path.string();
  }

  ~VectorGlyphFont() {
    if (face_ != nullptr) {
      FT_Done_Face(face_);
    }
    if (library_ != nullptr) {
      FT_Done_FreeType(library_);
    }
  }

  bool available() const { return face_ != nullptr; }

  const std::string &font_path() const { return font_path_; }

  bool AppendTextSvg(const std::string &text, double anchor_x, double anchor_y,
                     double height, double widthscale, double angle_degrees,
                     HorizontalTextAlignment horizontal_alignment,
                     VerticalTextAlignment vertical_alignment,
                     double line_spacing_factor, std::string_view stroke_color,
                     unsigned int marker_color, std::ostringstream &body,
                     const std::function<void(double, double)> &update_bounds);

private:
  struct OutlineSvgBuilder {
    double anchor_x = 0.0;
    double anchor_y = 0.0;
    double scale = 1.0;
    double widthscale = 1.0;
    double pen_x_units = 0.0;
    double line_offset_x = 0.0;
    double baseline_y = 0.0;
    double cosine = 1.0;
    double sine = 0.0;
    std::function<void(double, double)> update_bounds;
    std::ostringstream path;
    double current_x = 0.0;
    double current_y = 0.0;
    bool contour_open = false;
    bool has_geometry = false;

    std::pair<double, double> TransformPoint(double x_units,
                                             double y_units) const;
    void CloseContour();
  };

  static int MoveTo(const FT_Vector *to, void *user);
  static int LineTo(const FT_Vector *to, void *user);
  static int ConicTo(const FT_Vector *control, const FT_Vector *to, void *user);
  static int CubicTo(const FT_Vector *control1, const FT_Vector *control2,
                     const FT_Vector *to, void *user);

  FT_UInt ResolveGlyphIndex(uint32_t codepoint) const;
  double MeasureLineUnits(const std::vector<uint32_t> &line);
  bool AppendLineSvg(const std::vector<uint32_t> &line, double anchor_x,
                     double anchor_y, double scale, double widthscale,
                     double line_offset_x, double baseline_y, double cosine,
                     double sine, std::string_view stroke_color,
                     unsigned int marker_color, std::ostringstream &body,
                     const std::function<void(double, double)> &update_bounds);

  FT_Library library_ = nullptr;
  FT_Face face_ = nullptr;
  std::string font_path_;
};

std::pair<double, double>
VectorGlyphFont::OutlineSvgBuilder::TransformPoint(double x_units,
                                                   double y_units) const {
  const double local_x =
      line_offset_x + (pen_x_units + x_units) * scale * widthscale;
  const double local_y = baseline_y + y_units * scale;
  return {anchor_x + local_x * cosine - local_y * sine,
          anchor_y + local_x * sine + local_y * cosine};
}

void VectorGlyphFont::OutlineSvgBuilder::CloseContour() {
  if (contour_open) {
    path << "Z ";
    contour_open = false;
  }
}

int VectorGlyphFont::MoveTo(const FT_Vector *to, void *user) {
  auto *builder = static_cast<OutlineSvgBuilder *>(user);
  builder->CloseContour();
  const auto [x, y] = builder->TransformPoint(static_cast<double>(to->x),
                                              static_cast<double>(to->y));
  builder->path << "M " << FormatNumber(x) << ' ' << FormatNumber(y) << ' ';
  builder->update_bounds(x, y);
  builder->current_x = static_cast<double>(to->x);
  builder->current_y = static_cast<double>(to->y);
  builder->contour_open = true;
  builder->has_geometry = true;
  return 0;
}

int VectorGlyphFont::LineTo(const FT_Vector *to, void *user) {
  auto *builder = static_cast<OutlineSvgBuilder *>(user);
  const auto [x, y] = builder->TransformPoint(static_cast<double>(to->x),
                                              static_cast<double>(to->y));
  builder->path << "L " << FormatNumber(x) << ' ' << FormatNumber(y) << ' ';
  builder->update_bounds(x, y);
  builder->current_x = static_cast<double>(to->x);
  builder->current_y = static_cast<double>(to->y);
  builder->has_geometry = true;
  return 0;
}

int VectorGlyphFont::ConicTo(const FT_Vector *control, const FT_Vector *to,
                             void *user) {
  auto *builder = static_cast<OutlineSvgBuilder *>(user);
  const double control_x = static_cast<double>(control->x);
  const double control_y = static_cast<double>(control->y);
  const double end_x = static_cast<double>(to->x);
  const double end_y = static_cast<double>(to->y);
  const double cubic1_x =
      builder->current_x + (2.0 / 3.0) * (control_x - builder->current_x);
  const double cubic1_y =
      builder->current_y + (2.0 / 3.0) * (control_y - builder->current_y);
  const double cubic2_x = end_x + (2.0 / 3.0) * (control_x - end_x);
  const double cubic2_y = end_y + (2.0 / 3.0) * (control_y - end_y);
  const auto [x1, y1] = builder->TransformPoint(cubic1_x, cubic1_y);
  const auto [x2, y2] = builder->TransformPoint(cubic2_x, cubic2_y);
  const auto [x3, y3] = builder->TransformPoint(end_x, end_y);
  builder->path << "C " << FormatNumber(x1) << ' ' << FormatNumber(y1) << ' '
                << FormatNumber(x2) << ' ' << FormatNumber(y2) << ' '
                << FormatNumber(x3) << ' ' << FormatNumber(y3) << ' ';
  builder->update_bounds(x1, y1);
  builder->update_bounds(x2, y2);
  builder->update_bounds(x3, y3);
  builder->current_x = end_x;
  builder->current_y = end_y;
  builder->has_geometry = true;
  return 0;
}

int VectorGlyphFont::CubicTo(const FT_Vector *control1,
                             const FT_Vector *control2, const FT_Vector *to,
                             void *user) {
  auto *builder = static_cast<OutlineSvgBuilder *>(user);
  const auto [x1, y1] = builder->TransformPoint(
      static_cast<double>(control1->x), static_cast<double>(control1->y));
  const auto [x2, y2] = builder->TransformPoint(
      static_cast<double>(control2->x), static_cast<double>(control2->y));
  const auto [x3, y3] = builder->TransformPoint(static_cast<double>(to->x),
                                                static_cast<double>(to->y));
  builder->path << "C " << FormatNumber(x1) << ' ' << FormatNumber(y1) << ' '
                << FormatNumber(x2) << ' ' << FormatNumber(y2) << ' '
                << FormatNumber(x3) << ' ' << FormatNumber(y3) << ' ';
  builder->update_bounds(x1, y1);
  builder->update_bounds(x2, y2);
  builder->update_bounds(x3, y3);
  builder->current_x = static_cast<double>(to->x);
  builder->current_y = static_cast<double>(to->y);
  builder->has_geometry = true;
  return 0;
}

FT_UInt VectorGlyphFont::ResolveGlyphIndex(uint32_t codepoint) const {
  FT_UInt glyph_index = FT_Get_Char_Index(face_, codepoint);
  if (glyph_index == 0 && codepoint != static_cast<uint32_t>('?')) {
    glyph_index = FT_Get_Char_Index(face_, static_cast<FT_ULong>('?'));
  }
  return glyph_index;
}

double VectorGlyphFont::MeasureLineUnits(const std::vector<uint32_t> &line) {
  FT_UInt previous_glyph = 0;
  double advance = 0.0;
  for (uint32_t codepoint : line) {
    const FT_UInt glyph_index = ResolveGlyphIndex(codepoint);
    if (glyph_index == 0) {
      advance += std::max<double>(face_->units_per_EM * 0.5, 1.0);
      previous_glyph = 0;
      continue;
    }
    if (FT_HAS_KERNING(face_) && previous_glyph != 0) {
      FT_Vector kerning = {};
      FT_Get_Kerning(face_, previous_glyph, glyph_index, FT_KERNING_UNSCALED,
                     &kerning);
      advance += static_cast<double>(kerning.x);
    }
    if (FT_Load_Glyph(face_, glyph_index,
                      FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING |
                          FT_LOAD_NO_BITMAP) == 0) {
      advance += static_cast<double>(face_->glyph->advance.x);
    }
    previous_glyph = glyph_index;
  }
  return advance;
}

bool VectorGlyphFont::AppendLineSvg(
    const std::vector<uint32_t> &line, double anchor_x, double anchor_y,
    double scale, double widthscale, double line_offset_x, double baseline_y,
    double cosine, double sine, std::string_view stroke_color,
    unsigned int marker_color, std::ostringstream &body,
    const std::function<void(double, double)> &update_bounds) {
  FT_UInt previous_glyph = 0;
  double pen_x_units = 0.0;
  bool emitted_any = false;
  for (uint32_t codepoint : line) {
    const FT_UInt glyph_index = ResolveGlyphIndex(codepoint);
    if (glyph_index == 0) {
      pen_x_units += std::max<double>(face_->units_per_EM * 0.5, 1.0);
      previous_glyph = 0;
      continue;
    }
    if (FT_HAS_KERNING(face_) && previous_glyph != 0) {
      FT_Vector kerning = {};
      FT_Get_Kerning(face_, previous_glyph, glyph_index, FT_KERNING_UNSCALED,
                     &kerning);
      pen_x_units += static_cast<double>(kerning.x);
    }
    if (FT_Load_Glyph(face_, glyph_index,
                      FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING |
                          FT_LOAD_NO_BITMAP) != 0) {
      previous_glyph = glyph_index;
      continue;
    }
    if (face_->glyph->format == FT_GLYPH_FORMAT_OUTLINE &&
        face_->glyph->outline.n_contours > 0) {
      OutlineSvgBuilder builder;
      builder.anchor_x = anchor_x;
      builder.anchor_y = anchor_y;
      builder.scale = scale;
      builder.widthscale = widthscale;
      builder.pen_x_units = pen_x_units;
      builder.line_offset_x = line_offset_x;
      builder.baseline_y = baseline_y;
      builder.cosine = cosine;
      builder.sine = sine;
      builder.update_bounds = update_bounds;
      FT_Outline_Funcs callbacks = {&MoveTo, &LineTo, &ConicTo, &CubicTo, 0, 0};
      if (FT_Outline_Decompose(&face_->glyph->outline, &callbacks, &builder) ==
          0) {
        builder.CloseContour();
        if (builder.has_geometry) {
          body << "<path d=\"" << builder.path.str() << "\" stroke=\""
               << stroke_color << "\" stroke-width=\"1\" fill=\""
               << SvgHexColor(marker_color) << "\" fill-opacity=\"0\" />\n";
          emitted_any = true;
        }
      }
    }
    pen_x_units += static_cast<double>(face_->glyph->advance.x);
    previous_glyph = glyph_index;
  }
  return emitted_any;
}

bool VectorGlyphFont::AppendTextSvg(
    const std::string &text, double anchor_x, double anchor_y, double height,
    double widthscale, double angle_degrees,
    HorizontalTextAlignment horizontal_alignment,
    VerticalTextAlignment vertical_alignment, double line_spacing_factor,
    std::string_view stroke_color, unsigned int marker_color,
    std::ostringstream &body,
    const std::function<void(double, double)> &update_bounds) {
  if (!available() || text.empty()) {
    return false;
  }
  const std::vector<std::string> line_text = SplitTextLines(text);
  if (line_text.empty()) {
    return false;
  }
  std::vector<std::vector<uint32_t>> lines;
  lines.reserve(line_text.size());
  for (const std::string &line : line_text) {
    lines.push_back(DecodeUtf8(line));
  }
  const double units_per_em = std::max<double>(face_->units_per_EM, 1.0);
  const double scale = height / units_per_em;
  const double effective_widthscale = std::max(widthscale, 0.1);
  const double ascender = face_->ascender * scale;
  const double descender = face_->descender * scale;
  const double raw_line_height_units =
      face_->height > 0
          ? static_cast<double>(face_->height)
          : static_cast<double>(face_->ascender - face_->descender);
  const double line_height = std::max(raw_line_height_units * scale *
                                          std::max(line_spacing_factor, 1.0),
                                      height);
  const double block_top = ascender;
  const double block_bottom =
      descender - line_height * static_cast<double>(lines.size() - 1);
  double baseline_shift = 0.0;
  switch (vertical_alignment) {
  case VerticalTextAlignment::Baseline:
    baseline_shift = 0.0;
    break;
  case VerticalTextAlignment::Bottom:
    baseline_shift = -block_bottom;
    break;
  case VerticalTextAlignment::Middle:
    baseline_shift = -(block_top + block_bottom) * 0.5;
    break;
  case VerticalTextAlignment::Top:
    baseline_shift = -block_top;
    break;
  }
  const double radians = angle_degrees * kPi / 180.0;
  const double cosine = std::cos(radians);
  const double sine = std::sin(radians);
  bool emitted_any = false;
  for (size_t line_index = 0; line_index < lines.size(); ++line_index) {
    const double line_width =
        MeasureLineUnits(lines[line_index]) * scale * effective_widthscale;
    double line_offset_x = 0.0;
    switch (horizontal_alignment) {
    case HorizontalTextAlignment::Left:
      break;
    case HorizontalTextAlignment::Center:
      line_offset_x = -line_width * 0.5;
      break;
    case HorizontalTextAlignment::Right:
      line_offset_x = -line_width;
      break;
    }
    emitted_any |= AppendLineSvg(
        lines[line_index], anchor_x, anchor_y, scale, effective_widthscale,
        line_offset_x,
        baseline_shift - line_height * static_cast<double>(line_index), cosine,
        sine, stroke_color, marker_color, body, update_bounds);
  }
  return emitted_any;
}

struct SvgFragment {
  bool has_geometry = false;
  double min_x = std::numeric_limits<double>::max();
  double min_y = std::numeric_limits<double>::max();
  double max_x = std::numeric_limits<double>::lowest();
  double max_y = std::numeric_limits<double>::lowest();
  std::ostringstream body;
};

struct SvgBlock {
  DRW_Coord base_point;
  SvgFragment fragment;
};

struct BulgeArcInfo {
  double radius = 0.0;
  double center_x = 0.0;
  double center_y = 0.0;
  bool large_arc = false;
  bool sweep_positive = false;
};

int FindKnotSpan(int degree, double parameter, const std::vector<double> &knots,
                 int control_point_count) {
  const int last_control_index = control_point_count - 1;
  if (parameter >= knots[last_control_index + 1]) {
    return last_control_index;
  }

  int low = degree;
  int high = last_control_index + 1;
  int mid = (low + high) / 2;
  while (parameter < knots[mid] || parameter >= knots[mid + 1]) {
    if (parameter < knots[mid]) {
      high = mid;
    } else {
      low = mid;
    }
    mid = (low + high) / 2;
  }
  return mid;
}

std::vector<double> EvaluateBasisFunctions(int span, double parameter,
                                           int degree,
                                           const std::vector<double> &knots) {
  std::vector<double> basis(static_cast<size_t>(degree + 1), 0.0);
  std::vector<double> left(static_cast<size_t>(degree + 1), 0.0);
  std::vector<double> right(static_cast<size_t>(degree + 1), 0.0);
  basis[0] = 1.0;
  for (int order = 1; order <= degree; ++order) {
    left[order] = parameter - knots[span + 1 - order];
    right[order] = knots[span + order] - parameter;
    double saved = 0.0;
    for (int index = 0; index < order; ++index) {
      const double denominator = right[index + 1] + left[order - index];
      const double term = denominator == 0.0 ? 0.0 : basis[index] / denominator;
      basis[index] = saved + right[index + 1] * term;
      saved = left[order - index] * term;
    }
    basis[order] = saved;
  }
  return basis;
}

std::vector<DRW_Coord> SampleSplineCurve(const DRW_Spline &spline) {
  std::vector<DRW_Coord> samples;

  if (!spline.fitlist.empty() && spline.controllist.empty()) {
    samples.reserve(spline.fitlist.size());
    for (const std::shared_ptr<DRW_Coord> &point : spline.fitlist) {
      samples.push_back(*point);
    }
    return samples;
  }

  const int control_count = static_cast<int>(spline.controllist.size());
  if (control_count < 2 || spline.degree < 1 ||
      static_cast<int>(spline.knotslist.size()) <
          control_count + spline.degree + 1) {
    return samples;
  }

  const int degree = std::min(spline.degree, control_count - 1);
  const double start = spline.knotslist[degree];
  const double end = spline.knotslist[control_count];
  if (end <= start) {
    return samples;
  }

  bool first_parameter = true;
  for (int knot_index = degree; knot_index < control_count; ++knot_index) {
    const double span_start = spline.knotslist[knot_index];
    const double span_end = spline.knotslist[knot_index + 1];
    if (span_end - span_start <= kEpsilon) {
      continue;
    }

    constexpr int kSamplesPerSpan = 4;
    for (int sample_index = 0; sample_index <= kSamplesPerSpan;
         ++sample_index) {
      if (!first_parameter && sample_index == 0) {
        continue;
      }

      const double interpolation = static_cast<double>(sample_index) /
                                   static_cast<double>(kSamplesPerSpan);
      double parameter = span_start + (span_end - span_start) * interpolation;
      if (knot_index + 1 == control_count && sample_index == kSamplesPerSpan) {
        parameter = end;
      }

      const int span =
          FindKnotSpan(degree, parameter, spline.knotslist, control_count);
      const std::vector<double> basis =
          EvaluateBasisFunctions(span, parameter, degree, spline.knotslist);

      double weighted_x = 0.0;
      double weighted_y = 0.0;
      double weighted_sum = 0.0;
      for (int basis_index = 0; basis_index <= degree; ++basis_index) {
        const int control_index = span - degree + basis_index;
        const double weight =
            control_index < static_cast<int>(spline.weightlist.size())
                ? spline.weightlist[control_index]
                : 1.0;
        const double coefficient = basis[basis_index] * weight;
        weighted_x += spline.controllist[control_index]->x * coefficient;
        weighted_y += spline.controllist[control_index]->y * coefficient;
        weighted_sum += coefficient;
      }

      if (weighted_sum == 0.0) {
        continue;
      }

      samples.emplace_back(weighted_x / weighted_sum, weighted_y / weighted_sum,
                           0.0);
      first_parameter = false;
    }
  }

  return samples;
}

class DxfToSvgAdapter final : public DRW_Interface {
public:
  void addHeader(const DRW_Header *) override {}
  void addLType(const DRW_LType &) override {}
  void addLayer(const DRW_Layer &) override {}
  void addDimStyle(const DRW_Dimstyle &) override {}
  void addVport(const DRW_Vport &) override {}
  void addTextStyle(const DRW_Textstyle &) override {}
  void addAppId(const DRW_AppId &) override {}

  void addBlock(const DRW_Block &data) override {
    current_block_name_ = data.name;
    SvgBlock &block = blocks_[current_block_name_];
    block = SvgBlock{};
    block.base_point = data.basePoint;
  }

  void setBlock(int) override {}
  void endBlock() override { current_block_name_.clear(); }

  void addPoint(const DRW_Point &data) override {
    UpdateBounds(data.basePoint.x, data.basePoint.y);
    CurrentBody() << "<circle cx=\"" << FormatNumber(data.basePoint.x)
                  << "\" cy=\"" << FormatNumber(data.basePoint.y)
                  << "\" r=\"0.8\" fill=\"" << StrokeColorForEntity(data)
                  << "\" />\n";
  }

  void addLine(const DRW_Line &data) override {
    UpdateBounds(data.basePoint.x, data.basePoint.y);
    UpdateBounds(data.secPoint.x, data.secPoint.y);
    CurrentBody() << "<line x1=\"" << FormatNumber(data.basePoint.x)
                  << "\" y1=\"" << FormatNumber(data.basePoint.y) << "\" x2=\""
                  << FormatNumber(data.secPoint.x) << "\" y2=\""
                  << FormatNumber(data.secPoint.y) << "\" stroke=\""
                  << StrokeColorForEntity(data) << "\" stroke-width=\""
                  << FormatNumber(StrokeWidthForEntity(data))
                  << "\" fill=\"none\" />\n";
  }

  void addRay(const DRW_Ray &) override {}
  void addXline(const DRW_Xline &) override {}

  void addArc(const DRW_Arc &data) override {
    const double delta = NormalizeArcDelta(data.staangle, data.endangle);
    const double start_x =
        data.basePoint.x + std::cos(data.staangle) * data.radious;
    const double start_y =
        data.basePoint.y + std::sin(data.staangle) * data.radious;
    const double end_x =
        data.basePoint.x + std::cos(data.endangle) * data.radious;
    const double end_y =
        data.basePoint.y + std::sin(data.endangle) * data.radious;
    UpdateBounds(data.basePoint.x - data.radious,
                 data.basePoint.y - data.radious);
    UpdateBounds(data.basePoint.x + data.radious,
                 data.basePoint.y + data.radious);
    CurrentBody() << "<path d=\"M " << FormatNumber(start_x) << ' '
                  << FormatNumber(start_y) << " A "
                  << FormatNumber(data.radious) << ' '
                  << FormatNumber(data.radious) << " 0 "
                  << (delta > kPi ? 1 : 0) << ' ' << (data.isccw ? 1 : 0) << ' '
                  << FormatNumber(end_x) << ' ' << FormatNumber(end_y)
                  << "\" stroke=\"" << StrokeColorForEntity(data)
                  << "\" stroke-width=\""
                  << FormatNumber(StrokeWidthForEntity(data))
                  << "\" fill=\"none\" />\n";
  }

  void addCircle(const DRW_Circle &data) override {
    UpdateBounds(data.basePoint.x - data.radious,
                 data.basePoint.y - data.radious);
    UpdateBounds(data.basePoint.x + data.radious,
                 data.basePoint.y + data.radious);
    CurrentBody() << "<circle cx=\"" << FormatNumber(data.basePoint.x)
                  << "\" cy=\"" << FormatNumber(data.basePoint.y) << "\" r=\""
                  << FormatNumber(data.radious) << "\" stroke=\""
                  << StrokeColorForEntity(data) << "\" stroke-width=\""
                  << FormatNumber(StrokeWidthForEntity(data))
                  << "\" fill=\"none\" />\n";
  }

  void addEllipse(const DRW_Ellipse &data) override {
    const double major_radius = std::hypot(data.secPoint.x, data.secPoint.y);
    const double minor_radius = std::abs(major_radius * data.ratio);
    const double rotation =
        std::atan2(data.secPoint.y, data.secPoint.x) * 180.0 / kPi;
    UpdateBounds(data.basePoint.x - major_radius,
                 data.basePoint.y - minor_radius);
    UpdateBounds(data.basePoint.x + major_radius,
                 data.basePoint.y + minor_radius);
    CurrentBody() << "<ellipse cx=\"" << FormatNumber(data.basePoint.x)
                  << "\" cy=\"" << FormatNumber(data.basePoint.y) << "\" rx=\""
                  << FormatNumber(major_radius) << "\" ry=\""
                  << FormatNumber(minor_radius) << "\" transform=\"rotate("
                  << FormatNumber(rotation) << ' '
                  << FormatNumber(data.basePoint.x) << ' '
                  << FormatNumber(data.basePoint.y) << ")\" stroke=\""
                  << StrokeColorForEntity(data) << "\" stroke-width=\""
                  << FormatNumber(StrokeWidthForEntity(data))
                  << "\" fill=\"none\" />\n";
  }

  void addLWPolyline(const DRW_LWPolyline &data) override {
    if (data.vertlist.size() < 2) {
      skipped_entities_["LWPOLYLINE"] += 1;
      return;
    }
    AppendLwPolylineEntity(data, StrokeColorForEntity(data));
  }

  void addPolyline(const DRW_Polyline &data) override {
    if (data.vertlist.size() < 2) {
      skipped_entities_["POLYLINE"] += 1;
      return;
    }
    AppendPolylineEntity(data, StrokeColorForEntity(data));
  }

  void addSpline(const DRW_Spline *data) override {
    if (data == nullptr) {
      return;
    }
    const std::vector<DRW_Coord> points = SampleSplineCurve(*data);
    if (points.size() < 2) {
      skipped_entities_["SPLINE"] += 1;
      return;
    }

    const bool closed = (data->flags & 0x01) != 0;
    const std::string stroke_color = StrokeColorForEntity(*data);
    const double stroke_width = StrokeWidthForEntity(*data);

    CurrentBody() << "<path d=\"M " << FormatNumber(points.front().x) << ' '
                  << FormatNumber(points.front().y);
    UpdateBounds(points.front().x, points.front().y);
    for (size_t index = 1; index < points.size(); ++index) {
      UpdateBounds(points[index].x, points[index].y);
      CurrentBody() << " L " << FormatNumber(points[index].x) << ' '
                    << FormatNumber(points[index].y);
    }
    if (closed) {
      CurrentBody() << " Z";
    }
    CurrentBody() << "\" stroke=\"" << stroke_color << "\" stroke-width=\""
                  << FormatNumber(stroke_width) << "\" fill=\"none\" />\n";
  }

  void addKnot(const DRW_Entity &) override {}

  void addInsert(const DRW_Insert &data) override {
    auto it = blocks_.find(data.name);
    if (it == blocks_.end() || !it->second.fragment.has_geometry) {
      skipped_entities_["INSERT"] += 1;
      return;
    }

    for (int row = 0; row < std::max(data.rowcount, 1); ++row) {
      for (int col = 0; col < std::max(data.colcount, 1); ++col) {
        AppendBlockInstance(it->second, data, col, row);
      }
    }
  }

  void addTrace(const DRW_Trace &data) override {
    AppendQuadEntity(data.basePoint, data.secPoint, data.thirdPoint,
                     data.fourPoint, StrokeColorForEntity(data), false);
  }

  void add3dFace(const DRW_3Dface &data) override {
    if (imported_3dface_count_ >= kMaxImported3dFaces) {
      skipped_3dface_limit_count_ += 1;
      return;
    }

    imported_3dface_count_ += 1;
    AppendQuadEntity(data.basePoint, data.secPoint, data.thirdPoint,
                     data.fourPoint, StrokeColorForEntity(data), false);
  }

  void addSolid(const DRW_Solid &data) override {
    AppendQuadEntity(data.basePoint, data.secPoint, data.thirdPoint,
                     data.fourPoint, StrokeColorForEntity(data), true);
  }

  void addMText(const DRW_MText &data) override {
    if (!TryAppendVectorText(data, true, std::max(data.interlin, 0.8))) {
      AppendTextPlaceholder(data, true);
    }
  }

  void addText(const DRW_Text &data) override {
    if (!TryAppendVectorText(data, false, 1.0)) {
      AppendTextPlaceholder(data, false);
    }
  }

  void addDimAlign(const DRW_DimAligned *data) override {
    static_cast<void>(data);
    skipped_entities_["DIMENSION"] += 1;
  }

  void addDimLinear(const DRW_DimLinear *data) override {
    static_cast<void>(data);
    skipped_entities_["DIMENSION"] += 1;
  }

  void addDimRadial(const DRW_DimRadial *data) override {
    static_cast<void>(data);
    skipped_entities_["DIMENSION"] += 1;
  }

  void addDimDiametric(const DRW_DimDiametric *data) override {
    static_cast<void>(data);
    skipped_entities_["DIMENSION"] += 1;
  }

  void addDimAngular(const DRW_DimAngular *data) override {
    static_cast<void>(data);
    skipped_entities_["DIMENSION"] += 1;
  }

  void addDimAngular3P(const DRW_DimAngular3p *data) override {
    static_cast<void>(data);
    skipped_entities_["DIMENSION"] += 1;
  }

  void addDimOrdinate(const DRW_DimOrdinate *data) override {
    static_cast<void>(data);
    skipped_entities_["DIMENSION"] += 1;
  }

  void addLeader(const DRW_Leader *data) override {
    if (data == nullptr || data->vertexlist.size() < 2) {
      skipped_entities_["LEADER"] += 1;
      return;
    }

    CurrentBody() << "<polyline points=\"";
    for (const std::shared_ptr<DRW_Coord> &vertex : data->vertexlist) {
      UpdateBounds(vertex->x, vertex->y);
      CurrentBody() << FormatNumber(vertex->x) << ',' << FormatNumber(vertex->y)
                    << ' ';
    }
    CurrentBody()
        << "\" stroke=\"#1d1d1d\" stroke-width=\"1\" fill=\"none\" />\n";
  }

  void addHatch(const DRW_Hatch *data) override {
    if (data == nullptr || data->looplist.empty()) {
      skipped_entities_["HATCH"] += 1;
      return;
    }

    bool emitted_any = false;
    for (const std::shared_ptr<DRW_HatchLoop> &loop : data->looplist) {
      if (loop == nullptr || loop->objlist.empty()) {
        continue;
      }

      std::ostringstream path_data;
      bool started = false;
      for (const std::shared_ptr<DRW_Entity> &edge : loop->objlist) {
        if (const auto *line = dynamic_cast<const DRW_Line *>(edge.get())) {
          if (!started) {
            path_data << "M " << FormatNumber(line->basePoint.x) << ' '
                      << FormatNumber(line->basePoint.y) << ' ';
            started = true;
          }
          path_data << "L " << FormatNumber(line->secPoint.x) << ' '
                    << FormatNumber(line->secPoint.y) << ' ';
          UpdateBounds(line->basePoint.x, line->basePoint.y);
          UpdateBounds(line->secPoint.x, line->secPoint.y);
          continue;
        }

        if (const auto *arc = dynamic_cast<const DRW_Arc *>(edge.get())) {
          const double start_x =
              arc->basePoint.x + std::cos(arc->staangle) * arc->radious;
          const double start_y =
              arc->basePoint.y + std::sin(arc->staangle) * arc->radious;
          const double end_x =
              arc->basePoint.x + std::cos(arc->endangle) * arc->radious;
          const double end_y =
              arc->basePoint.y + std::sin(arc->endangle) * arc->radious;
          if (!started) {
            path_data << "M " << FormatNumber(start_x) << ' '
                      << FormatNumber(start_y) << ' ';
            started = true;
          }
          path_data << "A " << FormatNumber(arc->radious) << ' '
                    << FormatNumber(arc->radious) << " 0 "
                    << (NormalizeArcDelta(arc->staangle, arc->endangle) > kPi
                            ? 1
                            : 0)
                    << ' ' << (arc->isccw != 0 ? 1 : 0) << ' '
                    << FormatNumber(end_x) << ' ' << FormatNumber(end_y) << ' ';
          UpdateBounds(arc->basePoint.x - arc->radious,
                       arc->basePoint.y - arc->radious);
          UpdateBounds(arc->basePoint.x + arc->radious,
                       arc->basePoint.y + arc->radious);
          continue;
        }

        if (const auto *ellipse =
                dynamic_cast<const DRW_Ellipse *>(edge.get())) {
          const double major_radius =
              std::hypot(ellipse->secPoint.x, ellipse->secPoint.y);
          const double minor_radius = std::abs(major_radius * ellipse->ratio);
          const double rotation =
              std::atan2(ellipse->secPoint.y, ellipse->secPoint.x) * 180.0 /
              kPi;
          const double start_x =
              ellipse->basePoint.x + std::cos(ellipse->staparam) * major_radius;
          const double start_y =
              ellipse->basePoint.y + std::sin(ellipse->staparam) * minor_radius;
          const double end_x =
              ellipse->basePoint.x + std::cos(ellipse->endparam) * major_radius;
          const double end_y =
              ellipse->basePoint.y + std::sin(ellipse->endparam) * minor_radius;
          if (!started) {
            path_data << "M " << FormatNumber(start_x) << ' '
                      << FormatNumber(start_y) << ' ';
            started = true;
          }
          path_data << "A " << FormatNumber(major_radius) << ' '
                    << FormatNumber(minor_radius) << ' '
                    << FormatNumber(rotation) << " 0 "
                    << (ellipse->isccw != 0 ? 1 : 0) << ' '
                    << FormatNumber(end_x) << ' ' << FormatNumber(end_y) << ' ';
          UpdateBounds(ellipse->basePoint.x - major_radius,
                       ellipse->basePoint.y - minor_radius);
          UpdateBounds(ellipse->basePoint.x + major_radius,
                       ellipse->basePoint.y + minor_radius);
          continue;
        }

        if (const auto *spline = dynamic_cast<const DRW_Spline *>(edge.get())) {
          const std::vector<DRW_Coord> points = SampleSplineCurve(*spline);
          if (points.size() < 2) {
            skipped_entities_["HATCH_SPLINE"] += 1;
            continue;
          }
          if (!started) {
            path_data << "M " << FormatNumber(points.front().x) << ' '
                      << FormatNumber(points.front().y) << ' ';
            started = true;
          }
          UpdateBounds(points.front().x, points.front().y);
          for (size_t index = 1; index < points.size(); ++index) {
            path_data << "L " << FormatNumber(points[index].x) << ' '
                      << FormatNumber(points[index].y) << ' ';
            UpdateBounds(points[index].x, points[index].y);
          }
          continue;
        }

        skipped_entities_["HATCH_EDGE"] += 1;
      }

      if (!started) {
        continue;
      }

      path_data << 'Z';
      CurrentBody() << "<path d=\"" << path_data.str()
                    << "\" stroke=\"#1d1d1d\" stroke-width=\"1\" ";
      if (data->solid != 0) {
        CurrentBody() << "fill=\"#d9d9d9\" fill-opacity=\"0.35\"";
      } else {
        CurrentBody() << "fill=\"none\"";
      }
      CurrentBody() << " />\n";
      emitted_any = true;
    }

    if (!emitted_any) {
      skipped_entities_["HATCH"] += 1;
    }
  }

  void addViewport(const DRW_Viewport &) override {}
  void addImage(const DRW_Image *) override {}
  void linkImage(const DRW_ImageDef *) override {}
  void addComment(const char *) override {}
  void addPlotSettings(const DRW_PlotSettings *) override {}
  void writeHeader(DRW_Header &) override {}
  void writeBlocks() override {}
  void writeBlockRecords() override {}
  void writeEntities() override {}
  void writeLTypes() override {}
  void writeLayers() override {}
  void writeTextstyles() override {}
  void writeVports() override {}
  void writeDimstyles() override {}
  void writeObjects() override {}
  void writeAppId() override {}

  int skipped_item_count() const {
    int count = 0;
    for (const auto &[_, value] : skipped_entities_) {
      count += value;
    }
    count += skipped_3dface_limit_count_;
    return count;
  }

  bool exceeded_3dface_limit() const { return skipped_3dface_limit_count_ > 0; }

  int total_3dface_count() const {
    return imported_3dface_count_ + skipped_3dface_limit_count_;
  }

  std::vector<std::string> BuildNotes() const {
    std::vector<std::string> notes;
    for (const auto &[entity_name, count] : skipped_entities_) {
      notes.push_back("Skipped " + std::to_string(count) + " " + entity_name +
                      (count == 1 ? " entity" : " entities"));
    }
    if (skipped_3dface_limit_count_ > 0) {
      notes.push_back(
          "Skipped " + std::to_string(skipped_3dface_limit_count_) +
          " 3DFACE entities after reaching the import cap of " +
          std::to_string(kMaxImported3dFaces) +
          " to avoid freezing the canvas with dense 3D mesh geometry.");
    }
    if (vectorized_text_count_ > 0) {
      notes.push_back("Rendered " + std::to_string(vectorized_text_count_) +
                      " TEXT entit" +
                      std::string(vectorized_text_count_ == 1 ? "y" : "ies") +
                      " as vector glyph outlines.");
    }
    if (vectorized_mtext_count_ > 0) {
      notes.push_back("Rendered " + std::to_string(vectorized_mtext_count_) +
                      " MTEXT entit" +
                      std::string(vectorized_mtext_count_ == 1 ? "y" : "ies") +
                      " as vector glyph outlines.");
    }
    if (vector_glyph_font_.available() &&
        (vectorized_text_count_ > 0 || vectorized_mtext_count_ > 0)) {
      notes.push_back("DXF vector text used font: " +
                      vector_glyph_font_.font_path());
    }
    if (!vector_glyph_font_.available() &&
        (approximated_text_count_ > 0 || approximated_mtext_count_ > 0)) {
      notes.push_back(
          "No TrueType font was found for DXF text vectorization; set "
          "IM2D_DXF_TEXT_FONT to point at a .ttf file to replace placeholder "
          "text geometry.");
    }
    if (approximated_text_count_ > 0) {
      notes.push_back("Approximated " +
                      std::to_string(approximated_text_count_) + " TEXT entit" +
                      std::string(approximated_text_count_ == 1 ? "y" : "ies") +
                      " as placeholder guide geometry.");
    }
    if (approximated_mtext_count_ > 0) {
      notes.push_back(
          "Approximated " + std::to_string(approximated_mtext_count_) +
          " MTEXT entit" +
          std::string(approximated_mtext_count_ == 1 ? "y" : "ies") +
          " as placeholder guide geometry.");
    }
    return notes;
  }

  std::string BuildSvg() const {
    if (!model_.has_geometry) {
      return {};
    }

    const double margin = 12.0;
    const double min_x = model_.min_x - margin;
    const double min_y = model_.min_y - margin;
    const double width =
        std::max(1.0, (model_.max_x - model_.min_x) + margin * 2.0);
    const double height =
        std::max(1.0, (model_.max_y - model_.min_y) + margin * 2.0);

    std::ostringstream svg;
    svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\""
        << FormatNumber(min_x) << ' ' << FormatNumber(min_y) << ' '
        << FormatNumber(width) << ' ' << FormatNumber(height) << "\">\n"
        << model_.body.str() << "</svg>\n";
    return svg.str();
  }

private:
  std::ostringstream &CurrentBody() { return CurrentFragment().body; }

  SvgFragment &CurrentFragment() {
    if (current_block_name_.empty()) {
      return model_;
    }
    return blocks_[current_block_name_].fragment;
  }

  void UpdateBounds(double x, double y) {
    SvgFragment &fragment = CurrentFragment();
    fragment.has_geometry = true;
    fragment.min_x = std::min(fragment.min_x, x);
    fragment.min_y = std::min(fragment.min_y, y);
    fragment.max_x = std::max(fragment.max_x, x);
    fragment.max_y = std::max(fragment.max_y, y);
  }

  BulgeArcInfo BuildBulgeArc(double start_x, double start_y, double bulge,
                             double end_x, double end_y) {
    const double delta_x = end_x - start_x;
    const double delta_y = end_y - start_y;
    const double chord = std::hypot(delta_x, delta_y);
    const double theta = 4.0 * std::atan(bulge);
    const double radius =
        chord * (1.0 + bulge * bulge) / (4.0 * std::abs(bulge));
    const double midpoint_x = (start_x + end_x) * 0.5;
    const double midpoint_y = (start_y + end_y) * 0.5;
    const double offset =
        std::sqrt(std::max(radius * radius - (chord * chord) * 0.25, 0.0));
    const double normal_x = -delta_y / chord;
    const double normal_y = delta_x / chord;
    const double direction = bulge > 0.0 ? 1.0 : -1.0;

    BulgeArcInfo info;
    info.radius = radius;
    info.center_x = midpoint_x + normal_x * offset * direction;
    info.center_y = midpoint_y + normal_y * offset * direction;
    info.large_arc = std::abs(theta) > kPi;
    info.sweep_positive = bulge > 0.0;
    return info;
  }

  void AppendBulgeSegment(double start_x, double start_y, double bulge,
                          double end_x, double end_y) {
    UpdateBounds(start_x, start_y);
    UpdateBounds(end_x, end_y);
    if (std::abs(bulge) <= kEpsilon) {
      CurrentBody() << "L " << FormatNumber(end_x) << ' ' << FormatNumber(end_y)
                    << ' ';
      return;
    }

    const BulgeArcInfo arc =
        BuildBulgeArc(start_x, start_y, bulge, end_x, end_y);
    UpdateBounds(arc.center_x - arc.radius, arc.center_y - arc.radius);
    UpdateBounds(arc.center_x + arc.radius, arc.center_y + arc.radius);
    CurrentBody() << "A " << FormatNumber(arc.radius) << ' '
                  << FormatNumber(arc.radius) << " 0 "
                  << (arc.large_arc ? 1 : 0) << ' '
                  << (arc.sweep_positive ? 1 : 0) << ' ' << FormatNumber(end_x)
                  << ' ' << FormatNumber(end_y) << ' ';
  }

  void AppendLwPolylineEntity(const DRW_LWPolyline &data,
                              const std::string &stroke_color) {
    const bool closed = (data.flags & kPolylineFlagClosed) != 0;
    bool has_bulge = false;
    for (const std::shared_ptr<DRW_Vertex2D> &vertex : data.vertlist) {
      if (std::abs(vertex->bulge) > kEpsilon) {
        has_bulge = true;
        break;
      }
    }

    if (!has_bulge) {
      CurrentBody() << (closed ? "<polygon points=\"" : "<polyline points=\"");
      for (const std::shared_ptr<DRW_Vertex2D> &vertex : data.vertlist) {
        UpdateBounds(vertex->x, vertex->y);
        CurrentBody() << FormatNumber(vertex->x) << ','
                      << FormatNumber(vertex->y) << ' ';
      }
      CurrentBody() << "\" stroke=\"" << stroke_color
                    << "\" stroke-width=\"1\" fill=\"none\" />\n";
      return;
    }

    CurrentBody() << "<path d=\"M " << FormatNumber(data.vertlist.front()->x)
                  << ' ' << FormatNumber(data.vertlist.front()->y) << ' ';
    for (size_t index = 0; index < data.vertlist.size(); ++index) {
      if (!closed && index + 1 >= data.vertlist.size()) {
        break;
      }
      const std::shared_ptr<DRW_Vertex2D> &start = data.vertlist[index];
      const std::shared_ptr<DRW_Vertex2D> &end =
          data.vertlist[(index + 1) % data.vertlist.size()];
      AppendBulgeSegment(start->x, start->y, start->bulge, end->x, end->y);
    }
    if (closed) {
      CurrentBody() << 'Z';
    }
    CurrentBody() << "\" stroke=\"" << stroke_color
                  << "\" stroke-width=\"1\" fill=\"none\" />\n";
  }

  void AppendPolylineEntity(const DRW_Polyline &data,
                            const std::string &stroke_color) {
    if ((data.flags & kPolylineFlagPolyfaceMesh) != 0) {
      AppendPolyfaceMeshEntity(data, stroke_color);
      return;
    }

    const bool closed = (data.flags & kPolylineFlagClosed) != 0;
    bool has_bulge = false;
    for (const std::shared_ptr<DRW_Vertex> &vertex : data.vertlist) {
      if (std::abs(vertex->bulge) > kEpsilon) {
        has_bulge = true;
        break;
      }
    }

    if (!has_bulge) {
      CurrentBody() << (closed ? "<polygon points=\"" : "<polyline points=\"");
      for (const std::shared_ptr<DRW_Vertex> &vertex : data.vertlist) {
        UpdateBounds(vertex->basePoint.x, vertex->basePoint.y);
        CurrentBody() << FormatNumber(vertex->basePoint.x) << ','
                      << FormatNumber(vertex->basePoint.y) << ' ';
      }
      CurrentBody() << "\" stroke=\"" << stroke_color
                    << "\" stroke-width=\"1\" fill=\"none\" />\n";
      return;
    }

    CurrentBody() << "<path d=\"M "
                  << FormatNumber(data.vertlist.front()->basePoint.x) << ' '
                  << FormatNumber(data.vertlist.front()->basePoint.y) << ' ';
    for (size_t index = 0; index < data.vertlist.size(); ++index) {
      if (!closed && index + 1 >= data.vertlist.size()) {
        break;
      }
      const std::shared_ptr<DRW_Vertex> &start = data.vertlist[index];
      const std::shared_ptr<DRW_Vertex> &end =
          data.vertlist[(index + 1) % data.vertlist.size()];
      AppendBulgeSegment(start->basePoint.x, start->basePoint.y, start->bulge,
                         end->basePoint.x, end->basePoint.y);
    }
    if (closed) {
      CurrentBody() << 'Z';
    }
    CurrentBody() << "\" stroke=\"" << stroke_color
                  << "\" stroke-width=\"1\" fill=\"none\" />\n";
  }

  void AppendPolyfaceMeshEntity(const DRW_Polyline &data,
                                const std::string &stroke_color) {
    const size_t declared_vertex_count =
        data.vertexcount > 0 ? static_cast<size_t>(data.vertexcount) : 0;
    const size_t clamped_vertex_count =
        std::min(declared_vertex_count, data.vertlist.size());
    if (clamped_vertex_count == 0 ||
        data.vertlist.size() <= clamped_vertex_count) {
      skipped_entities_["POLYFACE_MESH"] += 1;
      return;
    }

    const double stroke_width = StrokeWidthForEntity(data);
    bool emitted_any = false;
    for (size_t face_index = clamped_vertex_count;
         face_index < data.vertlist.size(); ++face_index) {
      const std::shared_ptr<DRW_Vertex> &face = data.vertlist[face_index];
      std::array<int, 4> indices = {face->vindex1, face->vindex2, face->vindex3,
                                    face->vindex4};

      std::vector<const DRW_Vertex *> face_vertices;
      face_vertices.reserve(4);
      for (int raw_index : indices) {
        if (raw_index == 0) {
          continue;
        }

        const int vertex_index = std::abs(raw_index) - 1;
        if (vertex_index < 0 ||
            static_cast<size_t>(vertex_index) >= clamped_vertex_count) {
          continue;
        }
        face_vertices.push_back(
            data.vertlist[static_cast<size_t>(vertex_index)].get());
      }

      if (face_vertices.size() < 2) {
        continue;
      }

      CurrentBody() << (face_vertices.size() >= 3 ? "<polygon points=\""
                                                  : "<polyline points=\"");
      for (const DRW_Vertex *vertex : face_vertices) {
        UpdateBounds(vertex->basePoint.x, vertex->basePoint.y);
        CurrentBody() << FormatNumber(vertex->basePoint.x) << ','
                      << FormatNumber(vertex->basePoint.y) << ' ';
      }
      CurrentBody() << "\" stroke=\"" << stroke_color << "\" stroke-width=\""
                    << FormatNumber(stroke_width) << "\" fill=\"none\" />\n";
      emitted_any = true;
    }

    if (!emitted_any) {
      skipped_entities_["POLYFACE_MESH"] += 1;
    }
  }

  void AppendQuadEntity(const DRW_Coord &point1, const DRW_Coord &point2,
                        const DRW_Coord &point3, const DRW_Coord &point4,
                        const std::string &stroke_color, bool filled) {
    const bool collapse_fourth = std::abs(point3.x - point4.x) < kEpsilon &&
                                 std::abs(point3.y - point4.y) < kEpsilon;
    CurrentBody() << "<polygon points=\"";
    for (const DRW_Coord &point : {point1, point2, point3}) {
      UpdateBounds(point.x, point.y);
      CurrentBody() << FormatNumber(point.x) << ',' << FormatNumber(point.y)
                    << ' ';
    }
    if (!collapse_fourth) {
      UpdateBounds(point4.x, point4.y);
      CurrentBody() << FormatNumber(point4.x) << ',' << FormatNumber(point4.y)
                    << ' ';
    }
    CurrentBody() << "\" stroke=\"" << stroke_color << "\" stroke-width=\"1\" ";
    if (filled) {
      CurrentBody() << "fill=\"" << stroke_color << "\" fill-opacity=\"0.35\"";
    } else {
      CurrentBody() << "fill=\"none\"";
    }
    CurrentBody() << " />\n";
  }

  bool TryAppendVectorText(const DRW_Text &data, bool multiline,
                           double line_spacing_factor) {
    const std::string text = NormalizeDxfText(data.text);
    if (text.empty()) {
      skipped_entities_[multiline ? "MTEXT" : "TEXT"] += 1;
      return true;
    }

    if (!vector_glyph_font_.available()) {
      return false;
    }

    const std::string stroke_color = StrokeColorForEntity(data);
    const double widthscale =
        std::max<double>(std::abs(multiline ? 1.0 : data.widthscale), 0.1);
    const bool emitted = vector_glyph_font_.AppendTextSvg(
        text, data.basePoint.x, data.basePoint.y, std::max(data.height, 1.0),
        widthscale, data.angle, HorizontalAlignmentForText(data, multiline),
        VerticalAlignmentForText(data, multiline), line_spacing_factor,
        stroke_color, kDxfFilledTextColor, CurrentBody(),
        [this](double x, double y) { UpdateBounds(x, y); });
    if (!emitted) {
      return false;
    }

    if (multiline) {
      vectorized_mtext_count_ += 1;
    } else {
      vectorized_text_count_ += 1;
    }
    return true;
  }

  void AppendTextPlaceholder(const DRW_Text &data, bool multiline) {
    const std::string text = NormalizeDxfText(data.text);
    if (text.empty()) {
      skipped_entities_[multiline ? "MTEXT" : "TEXT"] += 1;
      return;
    }

    const std::vector<std::string> lines = SplitTextLines(text);
    const double height = std::max(data.height, 1.0);
    const double width_factor =
        multiline ? 1.0 : std::max(std::abs(data.widthscale), 0.5);
    size_t longest_line = 1;
    for (const std::string &line : lines) {
      longest_line = std::max(longest_line, line.size());
    }
    const double width = std::max(height * width_factor, 1.0) *
                         static_cast<double>(longest_line) * 0.55;
    const double line_height = height * (multiline ? 1.2 : 1.0);
    const double total_height =
        height + line_height *
                     static_cast<double>(std::max<size_t>(lines.size(), 1) - 1);
    const HorizontalTextAlignment horizontal_alignment =
        HorizontalAlignmentForText(data, multiline);
    const VerticalTextAlignment vertical_alignment =
        VerticalAlignmentForText(data, multiline);

    double x = data.basePoint.x;
    switch (horizontal_alignment) {
    case HorizontalTextAlignment::Center:
      x -= width * 0.5;
      break;
    case HorizontalTextAlignment::Right:
      x -= width;
      break;
    case HorizontalTextAlignment::Left:
      break;
    }

    double y = data.basePoint.y - height;
    switch (vertical_alignment) {
    case VerticalTextAlignment::Top:
      y = data.basePoint.y;
      break;
    case VerticalTextAlignment::Middle:
      y = data.basePoint.y - total_height * 0.5;
      break;
    case VerticalTextAlignment::Bottom:
      y = data.basePoint.y - total_height;
      break;
    case VerticalTextAlignment::Baseline:
      y = data.basePoint.y - height;
      break;
    }

    const double mid_y = y + total_height * 0.5;
    const double guide_half_height = std::max(height * 0.04, 0.2);
    const std::string stroke_color = StrokeColorForEntity(data);
    UpdateBounds(x, y);
    UpdateBounds(x + width, y + total_height);
    CurrentBody() << "<path d=\"M " << FormatNumber(x) << ' ' << FormatNumber(y)
                  << " L " << FormatNumber(x + width) << ' ' << FormatNumber(y)
                  << " L " << FormatNumber(x + width) << ' '
                  << FormatNumber(y + total_height) << " L " << FormatNumber(x)
                  << ' ' << FormatNumber(y + total_height) << " Z\" stroke=\""
                  << stroke_color
                  << "\" stroke-width=\"0.75\" stroke-dasharray=\"3 2\" fill=\""
                  << SvgHexColor(kDxfTextPlaceholderColor)
                  << "\" fill-opacity=\"0\" />\n";
    CurrentBody() << "<path d=\"M " << FormatNumber(x) << ' '
                  << FormatNumber(mid_y - guide_half_height) << " L "
                  << FormatNumber(x + width) << ' '
                  << FormatNumber(mid_y - guide_half_height) << " L "
                  << FormatNumber(x + width) << ' '
                  << FormatNumber(mid_y + guide_half_height) << " L "
                  << FormatNumber(x) << ' '
                  << FormatNumber(mid_y + guide_half_height) << " Z\" stroke=\""
                  << stroke_color << "\" stroke-width=\"0.5\" fill=\""
                  << SvgHexColor(kDxfTextPlaceholderColor)
                  << "\" fill-opacity=\"0\" />\n";
    if (multiline) {
      approximated_mtext_count_ += 1;
    } else {
      approximated_text_count_ += 1;
    }
  }

  DRW_Coord TransformInsertPoint(const SvgBlock &block,
                                 const DRW_Insert &insert, int col, int row,
                                 double x, double y) {
    const double local_x = (x - block.base_point.x) * insert.xscale;
    const double local_y = (y - block.base_point.y) * insert.yscale;
    const double cosine = std::cos(insert.angle);
    const double sine = std::sin(insert.angle);
    const double rotated_x = local_x * cosine - local_y * sine;
    const double rotated_y = local_x * sine + local_y * cosine;
    return DRW_Coord(insert.basePoint.x + rotated_x + col * insert.colspace,
                     insert.basePoint.y + rotated_y + row * insert.rowspace,
                     0.0);
  }

  void AppendBlockInstance(const SvgBlock &block, const DRW_Insert &insert,
                           int col, int row) {
    const double tx = insert.basePoint.x + col * insert.colspace;
    const double ty = insert.basePoint.y + row * insert.rowspace;
    CurrentBody() << "<g transform=\"translate(" << FormatNumber(tx) << ' '
                  << FormatNumber(ty) << ") rotate("
                  << FormatNumber(insert.angle * 180.0 / kPi) << ") scale("
                  << FormatNumber(insert.xscale) << ' '
                  << FormatNumber(insert.yscale) << ") translate("
                  << FormatNumber(-block.base_point.x) << ' '
                  << FormatNumber(-block.base_point.y) << ")\">\n"
                  << block.fragment.body.str() << "</g>\n";

    const std::array<DRW_Coord, 4> corners = {
        DRW_Coord(block.fragment.min_x, block.fragment.min_y, 0.0),
        DRW_Coord(block.fragment.max_x, block.fragment.min_y, 0.0),
        DRW_Coord(block.fragment.min_x, block.fragment.max_y, 0.0),
        DRW_Coord(block.fragment.max_x, block.fragment.max_y, 0.0)};
    for (const DRW_Coord &corner : corners) {
      const DRW_Coord transformed =
          TransformInsertPoint(block, insert, col, row, corner.x, corner.y);
      UpdateBounds(transformed.x, transformed.y);
    }
  }

  SvgFragment model_;
  std::unordered_map<std::string, SvgBlock> blocks_;
  std::unordered_map<std::string, int> skipped_entities_;
  int imported_3dface_count_ = 0;
  int skipped_3dface_limit_count_ = 0;
  int vectorized_text_count_ = 0;
  int vectorized_mtext_count_ = 0;
  int approximated_text_count_ = 0;
  int approximated_mtext_count_ = 0;
  VectorGlyphFont vector_glyph_font_;
  std::string current_block_name_;
};

} // namespace

ImportResult ImportDxfFile(CanvasState &state,
                           const std::filesystem::path &file_path) {
  DxfToSvgAdapter adapter;
  dxfRW reader(file_path.string().c_str());
  if (!reader.read(&adapter, false)) {
    log::GetLogger()->error("Failed to parse DXF with libdxfrw: {}",
                            file_path.string());
    return {.success = false,
            .message =
                "Failed to parse DXF with libdxfrw: " + file_path.string()};
  }

  if (adapter.exceeded_3dface_limit()) {
    ImportResult result;
    result.success = false;
    result.skipped_items_count = adapter.skipped_item_count();
    result.notes = adapter.BuildNotes();
    result.warnings_count = static_cast<int>(result.notes.size());
    result.message =
        "DXF import rejected: file contains " +
        std::to_string(adapter.total_3dface_count()) +
        " 3DFACE entities, which indicates dense 3D mesh geometry that is "
        "not suitable for the 2D canvas importer.";
    log::GetLogger()->warn("Rejected mesh-heavy DXF import: {}",
                           file_path.string());
    return result;
  }

  const std::string svg_data = adapter.BuildSvg();
  if (svg_data.empty()) {
    ImportResult result;
    result.success = false;
    result.message = "DXF file did not produce importable SVG geometry.";
    result.skipped_items_count = adapter.skipped_item_count();
    result.notes = adapter.BuildNotes();
    result.warnings_count = static_cast<int>(result.notes.size());
    log::GetLogger()->warn("DXF import produced no geometry: {}",
                           file_path.string());
    return result;
  }

  ImportSvgOptions svg_options;
  svg_options.mark_text_placeholders = true;
  svg_options.text_placeholder_color = kDxfTextPlaceholderColor;
  svg_options.text_filled_glyph_color = kDxfFilledTextColor;

  ImportResult result =
      ImportSvgData(state, svg_data, file_path.filename().string(),
                    file_path.string(), svg_options);
  result.skipped_items_count += adapter.skipped_item_count();
  std::vector<std::string> dxf_notes = adapter.BuildNotes();
  result.notes.insert(result.notes.end(), dxf_notes.begin(), dxf_notes.end());
  result.warnings_count = static_cast<int>(result.notes.size());
  if (result.success) {
    for (ImportedArtwork &artwork : state.imported_artwork) {
      if (artwork.id == result.artwork_id) {
        artwork.source_format = "DXF";
        artwork.source_path = file_path.string();
        break;
      }
    }
    FlipImportedArtworkVertical(state, result.artwork_id);
    result.message =
        "Imported DXF sample via libdxfrw -> SVG and flipped vertically.";
    if (result.warnings_count > 0) {
      log::GetLogger()->warn("DXF import completed with {} warning(s): {}",
                             result.warnings_count, file_path.string());
    } else {
      log::GetLogger()->info("Imported DXF sample: {}", file_path.string());
    }
  }
  return result;
}

} // namespace im2d::importer
