#pragma once

#include "im2d_canvas_types.h"

#include <imgui.h>

#include <string_view>

namespace im2d {

struct CanvasNotificationBannerLayout {
  ImVec2 min = ImVec2(0.0f, 0.0f);
  ImVec2 min_size = ImVec2(0.0f, 0.0f);
};

struct CanvasNotificationBannerStyle {
  ImU32 background = 0;
  ImU32 border = 0;
  ImU32 title = 0;
  ImU32 summary = 0;
  float border_radius = 6.0f;
  float border_thickness = 1.5f;
  ImVec2 text_padding = ImVec2(10.0f, 10.0f);
  float summary_offset_y = 24.0f;
  ImVec2 dismiss_button_size = ImVec2(18.0f, 18.0f);
  ImVec2 dismiss_button_margin = ImVec2(8.0f, 8.0f);
};

CanvasNotificationBannerStyle
CanvasNotificationBannerStyleFromTheme(const CanvasTheme &theme);

CanvasNotificationBannerLayout ResolveCanvasNotificationBannerLayout(
    const CanvasNotificationBannerLayout &layout,
    const CanvasNotificationBannerStyle &style, const char *title,
    std::string_view summary, CanvasNotificationDismissMode dismiss_mode);

CanvasNotificationId
ShowCanvasNotification(CanvasState &state, std::string_view title,
                       std::string_view summary,
                       CanvasNotificationDismissMode dismiss_mode);

void DismissCanvasNotification(CanvasState &state, CanvasNotificationId id);

bool CanvasNotificationBannerContainsPoint(
    const CanvasNotificationBannerLayout &layout,
    const CanvasNotificationBannerStyle &style, const char *title,
    std::string_view summary, CanvasNotificationDismissMode dismiss_mode,
    const ImVec2 &point);

bool CanvasNotificationBannerCloseButtonContainsPoint(
    const CanvasNotificationBannerLayout &layout,
    const CanvasNotificationBannerStyle &style, const char *title,
    std::string_view summary, CanvasNotificationDismissMode dismiss_mode,
    const ImVec2 &point);

void DrawCanvasNotificationBanner(ImDrawList *draw_list,
                                  const CanvasNotificationBannerLayout &layout,
                                  const CanvasNotificationBannerStyle &style,
                                  const char *title, std::string_view summary,
                                  CanvasNotificationDismissMode dismiss_mode);

} // namespace im2d