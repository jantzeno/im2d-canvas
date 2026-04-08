#include "im2d_canvas_undo.h"

#include "im2d_canvas_document.h"

#include <algorithm>
#include <utility>

namespace im2d {

namespace {

CanvasUndoSnapshot CaptureSnapshot(const CanvasState &state) {
  CanvasUndoSnapshot snapshot;
  snapshot.imported_artwork = state.imported_artwork;
  snapshot.selected_imported_artwork_id = state.selected_imported_artwork_id;
  snapshot.selected_imported_artwork_ids = state.selected_imported_artwork_ids;
  snapshot.selected_imported_debug = state.selected_imported_debug;
  snapshot.selection_scope = state.selection_scope;
  snapshot.imported_artwork_edit_mode = state.imported_artwork_edit_mode;
  snapshot.selected_imported_elements = state.selected_imported_elements;
  snapshot.next_imported_artwork_id = state.next_imported_artwork_id;
  snapshot.next_imported_part_id = state.next_imported_part_id;
  return snapshot;
}

void TrimSnapshots(std::vector<CanvasUndoSnapshot> *snapshots,
                   const std::size_t max_snapshots) {
  if (snapshots == nullptr || snapshots->size() <= max_snapshots) {
    return;
  }

  const std::size_t erase_count = snapshots->size() - max_snapshots;
  snapshots->erase(snapshots->begin(), snapshots->begin() + erase_count);
}

bool ContainsArtworkId(const CanvasState &state, const int artwork_id) {
  if (artwork_id == 0) {
    return false;
  }
  return FindImportedArtwork(state, artwork_id) != nullptr;
}

bool IsValidDebugSelection(const CanvasState &state,
                           const ImportedDebugSelection &selection) {
  if (selection.kind == ImportedDebugSelectionKind::None ||
      selection.artwork_id == 0) {
    return false;
  }

  const ImportedArtwork *artwork =
      FindImportedArtwork(state, selection.artwork_id);
  if (artwork == nullptr) {
    return false;
  }

  switch (selection.kind) {
  case ImportedDebugSelectionKind::Artwork:
    return true;
  case ImportedDebugSelectionKind::Group:
    return selection.item_id != 0 &&
           FindImportedGroup(*artwork, selection.item_id) != nullptr;
  case ImportedDebugSelectionKind::Path:
    return selection.item_id != 0 &&
           FindImportedPath(*artwork, selection.item_id) != nullptr;
  case ImportedDebugSelectionKind::DxfText:
    return selection.item_id != 0 &&
           FindImportedDxfText(*artwork, selection.item_id) != nullptr;
  case ImportedDebugSelectionKind::None:
    break;
  }
  return false;
}

void SanitizeRestoredSelection(CanvasState &state) {
  std::erase_if(state.selected_imported_artwork_ids,
                [&state](const int artwork_id) {
                  return !ContainsArtworkId(state, artwork_id);
                });

  if (!ContainsArtworkId(state, state.selected_imported_artwork_id)) {
    state.selected_imported_artwork_id =
        state.selected_imported_artwork_ids.empty()
            ? 0
            : state.selected_imported_artwork_ids.front();
  }

  if (state.selected_imported_artwork_id == 0) {
    state.selected_imported_artwork_ids.clear();
    state.selected_imported_elements.clear();
    state.selected_imported_debug = {};
    return;
  }

  if (state.selected_imported_artwork_ids.empty()) {
    state.selected_imported_artwork_ids.push_back(
        state.selected_imported_artwork_id);
  }

  const ImportedArtwork *artwork =
      FindImportedArtwork(state, state.selected_imported_artwork_id);
  if (artwork == nullptr) {
    state.selected_imported_elements.clear();
    state.selected_imported_debug = {};
    return;
  }

  std::erase_if(
      state.selected_imported_elements,
      [artwork](const ImportedElementSelection &selection) {
        if (selection.kind == ImportedElementKind::Path) {
          return FindImportedPath(*artwork, selection.item_id) == nullptr;
        }
        return FindImportedDxfText(*artwork, selection.item_id) == nullptr;
      });

  if (!IsValidDebugSelection(state, state.selected_imported_debug)) {
    state.selected_imported_debug = {ImportedDebugSelectionKind::Artwork,
                                     state.selected_imported_artwork_id, 0};
  }
}

void RestoreSnapshot(CanvasState &state, const CanvasUndoSnapshot &snapshot) {
  state.imported_artwork = snapshot.imported_artwork;
  state.selected_imported_artwork_id = snapshot.selected_imported_artwork_id;
  state.selected_imported_artwork_ids = snapshot.selected_imported_artwork_ids;
  state.selected_imported_debug = snapshot.selected_imported_debug;
  state.selection_scope = snapshot.selection_scope;
  state.imported_artwork_edit_mode = snapshot.imported_artwork_edit_mode;
  state.selected_imported_elements = snapshot.selected_imported_elements;
  state.next_imported_artwork_id = snapshot.next_imported_artwork_id;
  state.next_imported_part_id = snapshot.next_imported_part_id;
  state.imported_artwork_separation_preview = {};
  state.imported_artwork_auto_cut_preview = {};
  state.last_imported_operation_issue_artwork_id = 0;
  state.last_imported_operation_issue_elements.clear();
  state.highlight_last_imported_operation_issue_elements = false;
  SanitizeRestoredSelection(state);
}

void RestoreFromStack(CanvasState &state,
                      std::vector<CanvasUndoSnapshot> *source_stack,
                      std::vector<CanvasUndoSnapshot> *destination_stack,
                      const char *message) {
  if (source_stack == nullptr || source_stack->empty() ||
      destination_stack == nullptr) {
    return;
  }

  destination_stack->push_back(CaptureSnapshot(state));
  TrimSnapshots(destination_stack, state.undo_history.max_snapshots);

  const CanvasUndoSnapshot snapshot = std::move(source_stack->back());
  source_stack->pop_back();
  RestoreSnapshot(state, snapshot);
  state.last_imported_artwork_operation = {
      .success = true,
      .message = message,
  };
}

} // namespace

void PushUndoSnapshot(CanvasState &state, std::string_view /*reason*/) {
  if (state.undo_history.transaction_depth > 0) {
    return;
  }

  state.undo_history.undo_stack.push_back(CaptureSnapshot(state));
  TrimSnapshots(&state.undo_history.undo_stack,
                state.undo_history.max_snapshots);
  state.undo_history.redo_stack.clear();
}

bool CanUndo(const CanvasState &state) {
  return !state.undo_history.undo_stack.empty();
}

bool CanRedo(const CanvasState &state) {
  return !state.undo_history.redo_stack.empty();
}

bool Undo(CanvasState &state) {
  if (!CanUndo(state)) {
    return false;
  }

  RestoreFromStack(state, &state.undo_history.undo_stack,
                   &state.undo_history.redo_stack, "Undo completed.");
  return true;
}

bool Redo(CanvasState &state) {
  if (!CanRedo(state)) {
    return false;
  }

  RestoreFromStack(state, &state.undo_history.redo_stack,
                   &state.undo_history.undo_stack, "Redo completed.");
  return true;
}

ScopedUndoTransaction::ScopedUndoTransaction(CanvasState &state,
                                             const bool record_snapshot,
                                             std::string_view reason)
    : state_(&state) {
  if (record_snapshot && state_->undo_history.transaction_depth == 0) {
    PushUndoSnapshot(*state_, reason);
  }
  state_->undo_history.transaction_depth += 1;
}

ScopedUndoTransaction::ScopedUndoTransaction(CanvasState &state,
                                             std::string_view reason)
    : ScopedUndoTransaction(state, true, reason) {}

ScopedUndoTransaction::ScopedUndoTransaction(
    ScopedUndoTransaction &&other) noexcept
    : state_(other.state_) {
  other.state_ = nullptr;
}

ScopedUndoTransaction &
ScopedUndoTransaction::operator=(ScopedUndoTransaction &&other) noexcept {
  if (this == &other) {
    return *this;
  }

  if (state_ != nullptr) {
    state_->undo_history.transaction_depth =
        std::max(state_->undo_history.transaction_depth - 1, 0);
  }
  state_ = other.state_;
  other.state_ = nullptr;
  return *this;
}

ScopedUndoTransaction::~ScopedUndoTransaction() {
  if (state_ == nullptr) {
    return;
  }

  state_->undo_history.transaction_depth =
      std::max(state_->undo_history.transaction_depth - 1, 0);
}

} // namespace im2d