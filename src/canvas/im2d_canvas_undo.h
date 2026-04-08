#pragma once

#include "im2d_canvas_types.h"

#include <string_view>

namespace im2d {

void PushUndoSnapshot(CanvasState &state,
                      std::string_view reason = std::string_view());
bool CanUndo(const CanvasState &state);
bool CanRedo(const CanvasState &state);
bool Undo(CanvasState &state);
bool Redo(CanvasState &state);

class ScopedUndoTransaction {
public:
  explicit ScopedUndoTransaction(CanvasState &state, bool record_snapshot,
                                 std::string_view reason = std::string_view());
  explicit ScopedUndoTransaction(CanvasState &state,
                                 std::string_view reason = std::string_view());
  ScopedUndoTransaction(const ScopedUndoTransaction &) = delete;
  ScopedUndoTransaction &operator=(const ScopedUndoTransaction &) = delete;
  ScopedUndoTransaction(ScopedUndoTransaction &&other) noexcept;
  ScopedUndoTransaction &operator=(ScopedUndoTransaction &&other) noexcept;
  ~ScopedUndoTransaction();

private:
  CanvasState *state_ = nullptr;
};

} // namespace im2d