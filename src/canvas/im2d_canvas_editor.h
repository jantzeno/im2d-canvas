#pragma once

#include "im2d_canvas_types.h"

#include <imgui.h>

#include <string>

namespace im2d::detail {

class CanvasEditor {
public:
  void OpenWeld(int artwork_id);
  void OpenSplit(int artwork_id);
  void Draw(CanvasState &state);

  static bool HasArtworkPreview(const CanvasState &state, int artwork_id);
  static void ClearPreviewStateForArtwork(CanvasState &state, int artwork_id);

private:
  enum class ImportedArtworkTool {
    None,
    Weld,
    Split,
  };

  static std::string ImportedArtworkLabel(const ImportedArtwork &artwork);
  static const char *AutoCutDirectionLabel(AutoCutPreviewAxisMode axis_mode);
  static bool AutoCutPreviewMatchesToolSettings(const CanvasState &state,
                                                int artwork_id);

  void ResetImportedArtworkToolPopup();
  const char *WindowTitle() const;
  void DrawWeldImportedArtworkPopup(CanvasState &state,
                                    const ImportedArtwork &artwork);
  void DrawSplitImportedArtworkPopup(CanvasState &state,
                                     const ImportedArtwork &artwork);

  ImportedArtworkTool imported_artwork_tool_popup_ = ImportedArtworkTool::None;
  int imported_artwork_tool_popup_artwork_id_ = 0;
  bool imported_artwork_tool_window_open_ = false;
  bool position_tool_window_on_open_ = false;
  ImVec2 tool_window_open_position_ = ImVec2(0.0f, 0.0f);
};

} // namespace im2d::detail
