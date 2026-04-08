#include "im2d_canvas_notification.h"

namespace im2d {

namespace {

CanvasNotificationId NextCanvasNotificationId() {
  static CanvasNotificationId next_id = 0;
  next_id = static_cast<CanvasNotificationId>(next_id + 1);
  if (next_id == 0) {
    next_id = 1;
  }
  return next_id;
}

ImU32 ThemeColorToU32(const ImVec4 &color) {
  return ImGui::ColorConvertFloat4ToU32(color);
}

ImVec2 BannerMax(const CanvasNotificationBannerLayout &layout) {
  return ImVec2(layout.min.x + layout.min_size.x,
                layout.min.y + layout.min_size.y);
}

bool PointInRect(const ImVec2 &point, const ImVec2 &min, const ImVec2 &max) {
  return point.x >= min.x && point.x <= max.x && point.y >= min.y &&
         point.y <= max.y;
}

ImVec2 DismissButtonMin(const CanvasNotificationBannerLayout &layout,
                        const CanvasNotificationBannerStyle &style) {
  return ImVec2(layout.min.x + layout.min_size.x -
                    style.dismiss_button_margin.x - style.dismiss_button_size.x,
                layout.min.y + style.dismiss_button_margin.y);
}

ImVec2 DismissButtonMax(const CanvasNotificationBannerLayout &layout,
                        const CanvasNotificationBannerStyle &style) {
  const ImVec2 min = DismissButtonMin(layout, style);
  return ImVec2(min.x + style.dismiss_button_size.x,
                min.y + style.dismiss_button_size.y);
}

CanvasNotificationBannerLayout
MeasuredBannerLayout(const CanvasNotificationBannerLayout &layout,
                     const CanvasNotificationBannerStyle &style,
                     const char *title, std::string_view summary,
                     CanvasNotificationDismissMode dismiss_mode) {
  const ImVec2 title_size = ImGui::CalcTextSize(title);
  const ImVec2 summary_size = ImGui::CalcTextSize(
      summary.data(), summary.data() + summary.size(), false, -1.0f);

  float width =
      std::max(title_size.x, summary_size.x) + style.text_padding.x * 2.0f;
  float bottom =
      std::max(style.text_padding.y + title_size.y,
               style.text_padding.y + style.summary_offset_y + summary_size.y);

  if (dismiss_mode == CanvasNotificationDismissMode::UserClosable) {
    width = std::max(width, style.text_padding.x + title_size.x +
                                style.dismiss_button_margin.x +
                                style.dismiss_button_size.x +
                                style.dismiss_button_margin.x);
    bottom = std::max(bottom, style.dismiss_button_margin.y +
                                  style.dismiss_button_size.y);
  }

  CanvasNotificationBannerLayout resolved = layout;
  resolved.min_size.x = std::max(layout.min_size.x, width);
  resolved.min_size.y =
      std::max(layout.min_size.y, bottom + style.text_padding.y);
  return resolved;
}

} // namespace

CanvasNotificationBannerStyle
CanvasNotificationBannerStyleFromTheme(const CanvasTheme &theme) {
  return {
      ThemeColorToU32(theme.preview_banner_background),
      ThemeColorToU32(theme.preview_banner_border),
      ThemeColorToU32(theme.preview_banner_title),
      ThemeColorToU32(theme.preview_banner_summary),
  };
}

CanvasNotificationId
ShowCanvasNotification(CanvasState &state, std::string_view title,
                       std::string_view summary,
                       CanvasNotificationDismissMode dismiss_mode) {
  const CanvasNotificationId id = NextCanvasNotificationId();
  state.canvas_notification = {
      .active = true,
      .id = id,
      .dismiss_mode = dismiss_mode,
      .title = std::string(title),
      .summary = std::string(summary),
  };
  return id;
}

CanvasNotificationBannerLayout ResolveCanvasNotificationBannerLayout(
    const CanvasNotificationBannerLayout &layout,
    const CanvasNotificationBannerStyle &style, const char *title,
    std::string_view summary, CanvasNotificationDismissMode dismiss_mode) {
  return MeasuredBannerLayout(layout, style, title, summary, dismiss_mode);
}

void DismissCanvasNotification(CanvasState &state, CanvasNotificationId id) {
  if (!state.canvas_notification.active || state.canvas_notification.id != id) {
    return;
  }
  state.canvas_notification = {};
}

bool CanvasNotificationBannerContainsPoint(
    const CanvasNotificationBannerLayout &layout,
    const CanvasNotificationBannerStyle &style, const char *title,
    std::string_view summary, CanvasNotificationDismissMode dismiss_mode,
    const ImVec2 &point) {
  const CanvasNotificationBannerLayout resolved =
      MeasuredBannerLayout(layout, style, title, summary, dismiss_mode);
  return PointInRect(point, resolved.min, BannerMax(resolved));
}

bool CanvasNotificationBannerCloseButtonContainsPoint(
    const CanvasNotificationBannerLayout &layout,
    const CanvasNotificationBannerStyle &style, const char *title,
    std::string_view summary, CanvasNotificationDismissMode dismiss_mode,
    const ImVec2 &point) {
  if (dismiss_mode != CanvasNotificationDismissMode::UserClosable) {
    return false;
  }
  const CanvasNotificationBannerLayout resolved =
      MeasuredBannerLayout(layout, style, title, summary, dismiss_mode);
  return PointInRect(point, DismissButtonMin(resolved, style),
                     DismissButtonMax(resolved, style));
}

void DrawCanvasNotificationBanner(ImDrawList *draw_list,
                                  const CanvasNotificationBannerLayout &layout,
                                  const CanvasNotificationBannerStyle &style,
                                  const char *title, std::string_view summary,
                                  CanvasNotificationDismissMode dismiss_mode) {
  const CanvasNotificationBannerLayout resolved =
      MeasuredBannerLayout(layout, style, title, summary, dismiss_mode);
  const ImVec2 max = BannerMax(resolved);
  draw_list->AddRectFilled(resolved.min, max, style.background,
                           style.border_radius);
  draw_list->AddRect(resolved.min, max, style.border, style.border_radius, 0,
                     style.border_thickness);

  const ImVec2 title_pos(resolved.min.x + style.text_padding.x,
                         resolved.min.y + style.text_padding.y);
  draw_list->AddText(title_pos, style.title, title);

  const ImVec2 summary_pos(title_pos.x, title_pos.y + style.summary_offset_y);
  draw_list->AddText(summary_pos, style.summary, summary.data(),
                     summary.data() + summary.size());

  if (dismiss_mode != CanvasNotificationDismissMode::UserClosable) {
    return;
  }

  const bool close_hovered = CanvasNotificationBannerCloseButtonContainsPoint(
      resolved, style, title, summary, dismiss_mode, ImGui::GetIO().MousePos);
  const ImVec2 close_min = DismissButtonMin(resolved, style);
  const ImVec2 close_max = DismissButtonMax(resolved, style);
  if (close_hovered) {
    draw_list->AddRect(close_min, close_max, style.border, 4.0f, 0, 1.0f);
  }

  constexpr char kDismissLabel[] = "X";
  const ImVec2 label_size = ImGui::CalcTextSize(kDismissLabel);
  const ImVec2 label_pos(
      close_min.x + (style.dismiss_button_size.x - label_size.x) * 0.5f,
      close_min.y + (style.dismiss_button_size.y - label_size.y) * 0.5f - 1.0f);
  draw_list->AddText(label_pos, close_hovered ? style.title : style.summary,
                     kDismissLabel);
}

} // namespace im2d