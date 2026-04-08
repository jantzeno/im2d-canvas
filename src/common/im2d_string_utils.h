#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace im2d {

inline std::string LowercaseCopy(std::string_view value) {
  std::string lowered(value);
  std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return lowered;
}

inline std::string TrimCopy(std::string_view value) {
  size_t begin = 0;
  size_t end = value.size();
  while (begin < end &&
         std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
    begin += 1;
  }
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
    end -= 1;
  }
  return std::string(value.substr(begin, end - begin));
}

} // namespace im2d
