#pragma once

#include <cmath>
#include <imgui.h>

namespace im2d {

inline ImVec2 Midpoint(const ImVec2 &a, const ImVec2 &b) {
  return ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
}

inline float CrossProduct(const ImVec2 &a, const ImVec2 &b) {
  return a.x * b.y - a.y * b.x;
}

inline float DotProduct(const ImVec2 &a, const ImVec2 &b) {
  return a.x * b.x + a.y * b.y;
}

inline float VectorLengthSquared(const ImVec2 &vector) {
  return DotProduct(vector, vector);
}

inline ImVec2 SubtractPoints(const ImVec2 &left, const ImVec2 &right) {
  return ImVec2(left.x - right.x, left.y - right.y);
}

inline bool NormalizeVector(const ImVec2 &vector, ImVec2 *normalized) {
  if (normalized == nullptr) {
    return false;
  }
  const float length_squared = VectorLengthSquared(vector);
  if (length_squared <= 0.000001f) {
    return false;
  }
  const float inverse_length = 1.0f / std::sqrt(length_squared);
  *normalized = ImVec2(vector.x * inverse_length, vector.y * inverse_length);
  return true;
}

} // namespace im2d
