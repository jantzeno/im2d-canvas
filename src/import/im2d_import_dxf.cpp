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
#include <unordered_set>
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
constexpr unsigned int kDxfHoleTextColor = 0x00ff03ffu;

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

std::string LowercaseCopy(std::string_view value) {
  std::string lowered(value);
  std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return lowered;
}

bool HasSupportedFontExtension(const std::filesystem::path &path) {
  static constexpr std::array<std::string_view, 3> kExtensions = {
      ".ttf", ".otf", ".ttc"};
  const std::string extension = LowercaseCopy(path.extension().string());
  return std::find(kExtensions.begin(), kExtensions.end(), extension) !=
         kExtensions.end();
}

bool IsPreferredVectorFont(const std::filesystem::path &path) {
  static constexpr std::array<std::string_view, 8> kPreferredFonts = {
      "dejavusans.ttf",
      "liberationsans-regular.ttf",
      "notosans-regular.ttf",
      "droidsans.ttf",
      "carlito-regular.ttf",
      "cantarell-regular.otf",
      "arial.ttf",
      "helvetica.ttf"};
  const std::string filename = LowercaseCopy(path.filename().string());
  return std::find(kPreferredFonts.begin(), kPreferredFonts.end(), filename) !=
         kPreferredFonts.end();
}

std::filesystem::path FindSystemVectorFont(const std::filesystem::path &root) {
  std::error_code error;
  if (!std::filesystem::exists(root, error) ||
      !std::filesystem::is_directory(root, error)) {
    return {};
  }

  std::filesystem::path first_supported_font;
  std::filesystem::recursive_directory_iterator iterator(
      root, std::filesystem::directory_options::skip_permission_denied, error);
  const std::filesystem::recursive_directory_iterator end;
  while (!error && iterator != end) {
    const std::filesystem::directory_entry &entry = *iterator;
    const std::filesystem::path candidate = entry.path();

    bool is_regular_file = entry.is_regular_file(error);
    if (error) {
      error.clear();
      iterator.increment(error);
      continue;
    }
    if (!is_regular_file || !HasSupportedFontExtension(candidate)) {
      iterator.increment(error);
      continue;
    }

    if (first_supported_font.empty()) {
      first_supported_font = candidate;
    }
    if (IsPreferredVectorFont(candidate)) {
      return candidate;
    }

    iterator.increment(error);
  }

  return first_supported_font;
}

std::string BasenameLower(std::string_view value) {
  return LowercaseCopy(
      std::filesystem::path(std::string(value)).filename().string());
}

std::string EscapeXmlAttribute(std::string_view value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (const char character : value) {
    switch (character) {
    case '&':
      escaped += "&amp;";
      break;
    case '"':
      escaped += "&quot;";
      break;
    case '\'':
      escaped += "&apos;";
      break;
    case '<':
      escaped += "&lt;";
      break;
    case '>':
      escaped += "&gt;";
      break;
    default:
      escaped.push_back(character);
      break;
    }
  }
  return escaped;
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
      "/usr/share/fonts/TTF/DejaVuSans.ttf",
      "/usr/share/fonts/google-noto/NotoSans-Regular.ttf",
      "/usr/share/fonts/google-droid-sans-fonts/DroidSans.ttf",
      "/usr/share/fonts/google-carlito-fonts/Carlito-Regular.ttf",
      "/usr/share/fonts/abattis-cantarell-fonts/Cantarell-Regular.otf"};

  std::error_code error;
  for (const char *path : kFontCandidates) {
    const std::filesystem::path candidate(path);
    if (std::filesystem::exists(candidate, error)) {
      return candidate;
    }
  }

  std::vector<std::filesystem::path> font_roots = {"/usr/share/fonts",
                                                   "/usr/local/share/fonts"};
  if (const char *home = std::getenv("HOME");
      home != nullptr && home[0] != '\0') {
    font_roots.emplace_back(std::filesystem::path(home) / ".local/share/fonts");
    font_roots.emplace_back(std::filesystem::path(home) / ".fonts");
  }

  for (const std::filesystem::path &root : font_roots) {
    const std::filesystem::path candidate = FindSystemVectorFont(root);
    if (!candidate.empty()) {
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
    struct Contour {
      std::ostringstream path;
      std::vector<std::pair<double, double>> samples;

      double SignedArea() const {
        if (samples.size() < 3) {
          return 0.0;
        }

        double twice_area = 0.0;
        for (size_t index = 0; index < samples.size(); ++index) {
          const auto &[x1, y1] = samples[index];
          const auto &[x2, y2] = samples[(index + 1) % samples.size()];
          twice_area += x1 * y2 - x2 * y1;
        }
        return twice_area * 0.5;
      }
    };

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
    double current_x = 0.0;
    double current_y = 0.0;
    bool contour_open = false;
    bool has_geometry = false;
    std::vector<Contour> contours;
    size_t current_contour_index = 0;

    std::pair<double, double> TransformPoint(double x_units,
                                             double y_units) const;
    Contour *CurrentContour() {
      if (!contour_open || current_contour_index >= contours.size()) {
        return nullptr;
      }
      return &contours[current_contour_index];
    }

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
    contours[current_contour_index].path << "Z ";
    contour_open = false;
  }
}

int VectorGlyphFont::MoveTo(const FT_Vector *to, void *user) {
  auto *builder = static_cast<OutlineSvgBuilder *>(user);
  builder->CloseContour();
  const auto [x, y] = builder->TransformPoint(static_cast<double>(to->x),
                                              static_cast<double>(to->y));
  builder->contours.emplace_back();
  builder->current_contour_index = builder->contours.size() - 1;
  builder->contours.back().path << "M " << FormatNumber(x) << ' '
                                << FormatNumber(y) << ' ';
  builder->contours.back().samples.emplace_back(static_cast<double>(to->x),
                                                static_cast<double>(to->y));
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
  if (auto *contour = builder->CurrentContour(); contour != nullptr) {
    contour->path << "L " << FormatNumber(x) << ' ' << FormatNumber(y) << ' ';
    contour->samples.emplace_back(static_cast<double>(to->x),
                                  static_cast<double>(to->y));
  }
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
  if (auto *contour = builder->CurrentContour(); contour != nullptr) {
    contour->path << "C " << FormatNumber(x1) << ' ' << FormatNumber(y1) << ' '
                  << FormatNumber(x2) << ' ' << FormatNumber(y2) << ' '
                  << FormatNumber(x3) << ' ' << FormatNumber(y3) << ' ';
    constexpr int kCurveSamples = 8;
    for (int sample_index = 1; sample_index <= kCurveSamples; ++sample_index) {
      const double t = static_cast<double>(sample_index) /
                       static_cast<double>(kCurveSamples);
      const double mt = 1.0 - t;
      const double sample_x = mt * mt * builder->current_x +
                              2.0 * mt * t * control_x + t * t * end_x;
      const double sample_y = mt * mt * builder->current_y +
                              2.0 * mt * t * control_y + t * t * end_y;
      contour->samples.emplace_back(sample_x, sample_y);
    }
  }
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
  if (auto *contour = builder->CurrentContour(); contour != nullptr) {
    contour->path << "C " << FormatNumber(x1) << ' ' << FormatNumber(y1) << ' '
                  << FormatNumber(x2) << ' ' << FormatNumber(y2) << ' '
                  << FormatNumber(x3) << ' ' << FormatNumber(y3) << ' ';
    constexpr int kCurveSamples = 8;
    const double start_x = builder->current_x;
    const double start_y = builder->current_y;
    const double control1_x = static_cast<double>(control1->x);
    const double control1_y = static_cast<double>(control1->y);
    const double control2_x = static_cast<double>(control2->x);
    const double control2_y = static_cast<double>(control2->y);
    const double end_x = static_cast<double>(to->x);
    const double end_y = static_cast<double>(to->y);
    for (int sample_index = 1; sample_index <= kCurveSamples; ++sample_index) {
      const double t = static_cast<double>(sample_index) /
                       static_cast<double>(kCurveSamples);
      const double mt = 1.0 - t;
      const double sample_x = mt * mt * mt * start_x +
                              3.0 * mt * mt * t * control1_x +
                              3.0 * mt * t * t * control2_x + t * t * t * end_x;
      const double sample_y = mt * mt * mt * start_y +
                              3.0 * mt * mt * t * control1_y +
                              3.0 * mt * t * t * control2_y + t * t * t * end_y;
      contour->samples.emplace_back(sample_x, sample_y);
    }
  }
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
          int dominant_sign = 1;
          double dominant_area = 0.0;
          for (const OutlineSvgBuilder::Contour &contour : builder.contours) {
            const double area = contour.SignedArea();
            if (std::abs(area) > dominant_area) {
              dominant_area = std::abs(area);
              dominant_sign = area < 0.0 ? -1 : 1;
            }
          }

          for (const OutlineSvgBuilder::Contour &contour : builder.contours) {
            if (contour.samples.size() < 3) {
              continue;
            }
            const double area = contour.SignedArea();
            const int contour_sign = area < 0.0 ? -1 : 1;
            const unsigned int contour_marker =
                dominant_area > 0.0 && contour_sign != dominant_sign
                    ? kDxfHoleTextColor
                    : marker_color;
            body << "<path d=\"" << contour.path.str() << "\" stroke=\""
                 << stroke_color << "\" stroke-width=\"1\" fill=\""
                 << SvgHexColor(contour_marker) << "\" fill-opacity=\"0\" />\n";
            emitted_any = true;
          }
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
  struct PendingInsert {
    std::string token;
    DRW_Insert insert;
  };
  std::ostringstream body;
  std::vector<PendingInsert> pending_inserts;
};

struct SvgBlock {
  std::string name;
  DRW_Coord base_point;
  SvgFragment fragment;
  bool resolved = false;
  bool resolving = false;
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
  void addTextStyle(const DRW_Textstyle &data) override {
    text_styles_[data.name] = TextStyleInfo{data.font, data.bigFont};
  }
  void addAppId(const DRW_AppId &) override {}

  void addBlock(const DRW_Block &data) override {
    current_block_name_ = data.name;
    SvgBlock &block = blocks_[current_block_name_];
    block = SvgBlock{};
    block.name = data.name;
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
    AppendDeferredInsert(data);
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
    count += skipped_insert_missing_count_;
    count += skipped_insert_empty_count_;
    count += skipped_insert_recursive_count_;
    return count;
  }

  bool exceeded_3dface_limit() const { return skipped_3dface_limit_count_ > 0; }

  int total_3dface_count() const {
    return imported_3dface_count_ + skipped_3dface_limit_count_;
  }

  std::vector<std::string> BuildNotes() const {
    std::vector<std::string> notes;
    notes.insert(notes.end(), font_resolution_notes_.begin(),
                 font_resolution_notes_.end());
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
    if (skipped_insert_missing_count_ > 0) {
      notes.push_back(
          "Skipped " + std::to_string(skipped_insert_missing_count_) +
          " INSERT " +
          std::string(
              skipped_insert_missing_count_ == 1
                  ? "reference because its target block was missing."
                  : "references because their target blocks were missing."));
    }
    if (skipped_insert_empty_count_ > 0) {
      notes.push_back("Skipped " + std::to_string(skipped_insert_empty_count_) +
                      " INSERT " +
                      std::string(skipped_insert_empty_count_ == 1
                                      ? "reference because its target block "
                                        "resolved to no geometry."
                                      : "references because their target "
                                        "blocks resolved to no geometry."));
    }
    if (skipped_insert_recursive_count_ > 0) {
      notes.push_back(
          "Skipped " + std::to_string(skipped_insert_recursive_count_) +
          " INSERT " +
          std::string(skipped_insert_recursive_count_ == 1
                          ? "reference to avoid recursive block expansion."
                          : "references to avoid recursive block expansion."));
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

  void FinalizeGeometry() {
    if (geometry_finalized_) {
      return;
    }

    std::vector<std::string> resolution_stack;
    for (auto &[block_name, _] : blocks_) {
      ResolveBlock(block_name, resolution_stack);
    }
    ResolveFragment(model_, resolution_stack);
    geometry_finalized_ = true;
  }

  std::string BuildSvg() {
    FinalizeGeometry();
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
  struct TextStyleInfo {
    std::string font;
    std::string big_font;
  };

  std::ostringstream &CurrentBody() { return CurrentFragment().body; }

  static void ResetFragmentBody(SvgFragment &fragment,
                                const std::string &body_text) {
    fragment.body.str("");
    fragment.body.clear();
    fragment.body << body_text;
  }

  static void UpdateFragmentBounds(SvgFragment &fragment, double x, double y) {
    fragment.has_geometry = true;
    fragment.min_x = std::min(fragment.min_x, x);
    fragment.min_y = std::min(fragment.min_y, y);
    fragment.max_x = std::max(fragment.max_x, x);
    fragment.max_y = std::max(fragment.max_y, y);
  }

  static void ReplaceFirst(std::string &text, const std::string &needle,
                           const std::string &replacement) {
    const size_t position = text.find(needle);
    if (position == std::string::npos) {
      text += replacement;
      return;
    }
    text.replace(position, needle.size(), replacement);
  }

  void AppendDeferredInsert(const DRW_Insert &data) {
    SvgFragment &fragment = CurrentFragment();
    const std::string token =
        "<!--IM2D_INSERT_" + std::to_string(next_insert_token_id_++) + "-->";
    fragment.body << token;
    fragment.pending_inserts.push_back(SvgFragment::PendingInsert{token, data});
  }

  void RecordFontResolution(const DRW_Text &data, bool vectorized) {
    const auto style_it = text_styles_.find(data.style);
    if (style_it == text_styles_.end()) {
      return;
    }

    const TextStyleInfo &style_info = style_it->second;
    if (style_info.font.empty()) {
      return;
    }

    std::ostringstream note;
    note << "DXF text style " << data.style << " requested " << style_info.font;
    if (!style_info.big_font.empty()) {
      note << " (bigfont " << style_info.big_font << ")";
    }

    if (vectorized && vector_glyph_font_.available()) {
      if (BasenameLower(style_info.font) ==
          BasenameLower(vector_glyph_font_.font_path())) {
        return;
      }
      note << "; used " << vector_glyph_font_.font_path() << " instead.";
    } else {
      note << "; no outline substitute font was available, so placeholder "
              "geometry was used.";
    }

    const std::string note_text = note.str();
    if (font_resolution_note_set_.insert(note_text).second) {
      font_resolution_notes_.push_back(note_text);
      log::GetLogger()->warn("{}", note_text);
    }
  }

  SvgFragment &CurrentFragment() {
    if (current_block_name_.empty()) {
      return model_;
    }
    return blocks_[current_block_name_].fragment;
  }

  void UpdateBounds(double x, double y) {
    SvgFragment &fragment = CurrentFragment();
    UpdateFragmentBounds(fragment, x, y);
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

    RecordFontResolution(data, true);

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
    RecordFontResolution(data, false);
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
    AppendResolvedBlockInstance(CurrentFragment(), block, insert, col, row);
  }

  void AppendResolvedBlockInstance(SvgFragment &target_fragment,
                                   const SvgBlock &block,
                                   const DRW_Insert &insert, int col, int row) {
    const double tx = insert.basePoint.x + col * insert.colspace;
    const double ty = insert.basePoint.y + row * insert.rowspace;
    const int generated_group_id = next_generated_group_id_++;
    const std::string dom_id =
        "im2d-dxf-group-" + std::to_string(generated_group_id);
    std::string group_label =
        block.name.empty() ? "Block Instance" : "Block " + block.name;
    if (std::max(insert.rowcount, 1) > 1 || std::max(insert.colcount, 1) > 1) {
      group_label +=
          " [" + std::to_string(row + 1) + "," + std::to_string(col + 1) + "]";
    }
    target_fragment.body << "<g id=\"" << dom_id << "\" data-im2d-label=\""
                         << EscapeXmlAttribute(group_label)
                         << "\" transform=\"translate(" << FormatNumber(tx)
                         << ' ' << FormatNumber(ty) << ") rotate("
                         << FormatNumber(insert.angle * 180.0 / kPi)
                         << ") scale(" << FormatNumber(insert.xscale) << ' '
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
      UpdateFragmentBounds(target_fragment, transformed.x, transformed.y);
    }
  }

  bool ResolveBlock(const std::string &block_name,
                    std::vector<std::string> &resolution_stack) {
    auto it = blocks_.find(block_name);
    if (it == blocks_.end()) {
      return false;
    }

    SvgBlock &block = it->second;
    if (block.resolved) {
      return block.fragment.has_geometry;
    }
    if (block.resolving) {
      skipped_insert_recursive_count_ += 1;
      return false;
    }

    block.resolving = true;
    resolution_stack.push_back(block_name);
    ResolveFragment(block.fragment, resolution_stack);
    resolution_stack.pop_back();
    block.resolving = false;
    block.resolved = true;
    return block.fragment.has_geometry;
  }

  bool ResolveFragment(SvgFragment &fragment,
                       std::vector<std::string> &resolution_stack) {
    if (fragment.pending_inserts.empty()) {
      return fragment.has_geometry;
    }

    std::string resolved_body = fragment.body.str();
    const std::vector<SvgFragment::PendingInsert> pending_inserts =
        fragment.pending_inserts;
    fragment.pending_inserts.clear();

    for (const SvgFragment::PendingInsert &pending_insert : pending_inserts) {
      std::ostringstream expansion;
      auto block_it = blocks_.find(pending_insert.insert.name);
      if (block_it == blocks_.end()) {
        skipped_insert_missing_count_ += 1;
        ReplaceFirst(resolved_body, pending_insert.token, std::string{});
        continue;
      }

      const bool recursive_reference =
          std::find(resolution_stack.begin(), resolution_stack.end(),
                    pending_insert.insert.name) != resolution_stack.end();
      if (recursive_reference) {
        skipped_insert_recursive_count_ += 1;
        ReplaceFirst(resolved_body, pending_insert.token, std::string{});
        continue;
      }

      if (!ResolveBlock(pending_insert.insert.name, resolution_stack) ||
          !block_it->second.fragment.has_geometry) {
        skipped_insert_empty_count_ += 1;
        ReplaceFirst(resolved_body, pending_insert.token, std::string{});
        continue;
      }

      SvgFragment expansion_fragment;
      for (int row = 0; row < std::max(pending_insert.insert.rowcount, 1);
           ++row) {
        for (int col = 0; col < std::max(pending_insert.insert.colcount, 1);
             ++col) {
          AppendResolvedBlockInstance(expansion_fragment, block_it->second,
                                      pending_insert.insert, col, row);
        }
      }
      ReplaceFirst(resolved_body, pending_insert.token,
                   expansion_fragment.body.str());
      if (expansion_fragment.has_geometry) {
        UpdateFragmentBounds(fragment, expansion_fragment.min_x,
                             expansion_fragment.min_y);
        UpdateFragmentBounds(fragment, expansion_fragment.max_x,
                             expansion_fragment.max_y);
      }
    }

    ResetFragmentBody(fragment, resolved_body);
    return fragment.has_geometry;
  }

  SvgFragment model_;
  std::unordered_map<std::string, SvgBlock> blocks_;
  std::unordered_map<std::string, int> skipped_entities_;
  std::unordered_map<std::string, TextStyleInfo> text_styles_;
  std::unordered_set<std::string> font_resolution_note_set_;
  std::vector<std::string> font_resolution_notes_;
  int imported_3dface_count_ = 0;
  int skipped_3dface_limit_count_ = 0;
  int vectorized_text_count_ = 0;
  int vectorized_mtext_count_ = 0;
  int approximated_text_count_ = 0;
  int approximated_mtext_count_ = 0;
  int skipped_insert_missing_count_ = 0;
  int skipped_insert_empty_count_ = 0;
  int skipped_insert_recursive_count_ = 0;
  int next_insert_token_id_ = 1;
  int next_generated_group_id_ = 1;
  bool geometry_finalized_ = false;
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

  adapter.FinalizeGeometry();

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
  svg_options.text_hole_glyph_color = kDxfHoleTextColor;

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
