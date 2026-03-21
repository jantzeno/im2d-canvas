#pragma once

#include "../canvas/im2d_canvas.h"

#include <functional>
#include <string>

namespace demo {

struct DemoConfig {
  std::string app_title;
  std::string canvas_window_title = "Canvas";
  ImVec4 clear_color = ImVec4(0.07f, 0.08f, 0.09f, 1.0f);
  std::function<void(im2d::CanvasState &state)> initialize_state;
  std::function<void(im2d::CanvasState &state)> draw_inspector;
};

int RunDemoApp(const DemoConfig &config);

} // namespace demo