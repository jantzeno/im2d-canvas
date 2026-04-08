#pragma once

#include <iomanip>
#include <sstream>
#include <string>

namespace im2d {

template <typename T>
std::string FormatNumber(T value, int precision, bool trim_trailing_zeros) {
  std::ostringstream stream;
  stream << std::fixed << std::setprecision(precision) << value;
  if (!trim_trailing_zeros) {
    return stream.str();
  }
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

} // namespace im2d
