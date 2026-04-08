#include "../../src/canvas/im2d_canvas_document.h"
#include "../../src/canvas/im2d_canvas_imported_artwork_ops.h"

#include <catch2/catch_test_macros.hpp>

namespace {

im2d::ImportedPath MakeRectanglePath(const int id, const float min_x,
                                     const float min_y, const float max_x,
                                     const float max_y, const bool clockwise,
                                     const bool mark_as_hole = false) {
  im2d::ImportedPath path;
  path.id = id;
  path.label = "Path " + std::to_string(id);
  path.closed = true;
  if (mark_as_hole) {
    path.flags |= static_cast<uint32_t>(im2d::ImportedPathFlagHoleContour);
  }

  std::vector<ImVec2> points;
  if (clockwise) {
    points = {
        ImVec2(min_x, min_y),
        ImVec2(min_x, max_y),
        ImVec2(max_x, max_y),
        ImVec2(max_x, min_y),
    };
  } else {
    points = {
        ImVec2(min_x, min_y),
        ImVec2(max_x, min_y),
        ImVec2(max_x, max_y),
        ImVec2(min_x, max_y),
    };
  }

  path.segments.reserve(points.size());
  for (size_t index = 0; index < points.size(); ++index) {
    im2d::ImportedPathSegment segment;
    segment.kind = im2d::ImportedPathSegmentKind::Line;
    segment.start = points[index];
    segment.end = points[(index + 1) % points.size()];
    path.segments.push_back(segment);
  }
  return path;
}

im2d::ImportedArtwork
MakeArtworkWithRoot(std::vector<im2d::ImportedPath> paths) {
  im2d::ImportedArtwork artwork;
  artwork.id = 1;
  artwork.name = "Test Artwork";
  artwork.root_group_id = 1;
  artwork.next_group_id = 2;
  artwork.next_path_id = static_cast<int>(paths.size()) + 1;

  im2d::ImportedGroup root;
  root.id = artwork.root_group_id;
  root.label = "Root";
  for (im2d::ImportedPath &path : paths) {
    path.parent_group_id = root.id;
    root.path_ids.push_back(path.id);
  }

  artwork.groups.push_back(root);
  artwork.paths = std::move(paths);
  im2d::RecomputeImportedArtworkBounds(artwork);
  im2d::RecomputeImportedHierarchyBounds(artwork);
  im2d::RefreshImportedArtworkPartMetadata(artwork);
  return artwork;
}

im2d::ImportedArtwork MakeArtworkWithNestedGroup() {
  im2d::ImportedArtwork artwork;
  artwork.id = 1;
  artwork.name = "Grouped Artwork";
  artwork.root_group_id = 1;
  artwork.next_group_id = 3;
  artwork.next_path_id = 3;

  im2d::ImportedGroup root;
  root.id = 1;
  root.label = "Root";
  root.child_group_ids.push_back(2);
  root.path_ids.push_back(2);

  im2d::ImportedGroup child;
  child.id = 2;
  child.parent_group_id = 1;
  child.label = "Child";
  child.path_ids.push_back(1);

  im2d::ImportedPath grouped_path =
      MakeRectanglePath(1, 0.0f, 0.0f, 5.0f, 5.0f, false);
  grouped_path.parent_group_id = child.id;
  im2d::ImportedPath root_path =
      MakeRectanglePath(2, 8.0f, 0.0f, 12.0f, 4.0f, false);
  root_path.parent_group_id = root.id;

  artwork.groups.push_back(root);
  artwork.groups.push_back(child);
  artwork.paths.push_back(grouped_path);
  artwork.paths.push_back(root_path);
  im2d::RecomputeImportedArtworkBounds(artwork);
  im2d::RecomputeImportedHierarchyBounds(artwork);
  im2d::RefreshImportedArtworkPartMetadata(artwork);
  return artwork;
}

im2d::ImportedArtwork MakeArtworkWithId(const int artwork_id,
                                        const float x_offset = 0.0f) {
  im2d::ImportedArtwork artwork = MakeArtworkWithRoot(
      {MakeRectanglePath(1, x_offset, 0.0f, x_offset + 8.0f, 8.0f, false),
       MakeRectanglePath(2, x_offset + 12.0f, 0.0f, x_offset + 18.0f, 6.0f,
                         false)});
  artwork.id = artwork_id;
  artwork.name = "Artwork " + std::to_string(artwork_id);
  artwork.part.part_id = artwork_id;
  artwork.part.source_artwork_id = artwork_id;
  return artwork;
}

im2d::CanvasState MakeCanvasState(im2d::ImportedArtwork artwork) {
  im2d::CanvasState state;
  state.imported_artwork.push_back(std::move(artwork));
  state.selected_imported_artwork_id = state.imported_artwork.front().id;
  state.selected_imported_debug = {
      im2d::ImportedDebugSelectionKind::Artwork,
      state.imported_artwork.front().id,
      0,
  };
  return state;
}

} // namespace

TEST_CASE("Imported artwork selection helpers expose grouping and extraction "
          "eligibility",
          "[canvas][imported]") {
  im2d::CanvasState state = MakeCanvasState(MakeArtworkWithNestedGroup());
  const im2d::ImportedArtwork &artwork = state.imported_artwork.front();

  REQUIRE(im2d::HasGroupableImportedRootSelection(state, artwork));
  REQUIRE_FALSE(im2d::HasGroupableImportedElementSelection(state, artwork));

  state.selected_imported_elements = {
      {im2d::ImportedElementKind::Path, 1},
      {im2d::ImportedElementKind::Path, 2},
  };
  REQUIRE(im2d::HasGroupableImportedElementSelection(state, artwork));
  REQUIRE_FALSE(im2d::HasGroupableImportedRootSelection(state, artwork));

  state.selected_imported_elements.clear();
  state.selected_imported_debug = {
      im2d::ImportedDebugSelectionKind::Group,
      artwork.id,
      2,
  };
  REQUIRE(im2d::HasExtractableImportedDebugSelection(state, artwork));
  REQUIRE(im2d::HasUngroupableImportedDebugSelection(state, artwork));

  state.selected_imported_debug = {
      im2d::ImportedDebugSelectionKind::Path,
      artwork.id,
      2,
  };
  REQUIRE(im2d::HasExtractableImportedDebugSelection(state, artwork));

  state.imported_artwork.push_back(MakeArtworkWithId(2, 40.0f));
  state.selected_imported_artwork_ids = {1, 2};
  state.selection_scope = im2d::ImportedArtworkSelectionScope::Canvas;
  REQUIRE(im2d::HasGroupableImportedArtworkSelection(state));
}

TEST_CASE("GroupSelectedImportedArtworkObjects merges selected artworks into "
          "one grouped artwork",
          "[canvas][imported]") {
  im2d::CanvasState state;
  state.imported_artwork.push_back(MakeArtworkWithId(1));
  state.imported_artwork.push_back(MakeArtworkWithId(2, 40.0f));
  state.next_imported_artwork_id = 3;
  state.next_imported_part_id = 3;
  state.selected_imported_artwork_id = 1;
  state.selected_imported_artwork_ids = {1, 2};
  state.selection_scope = im2d::ImportedArtworkSelectionScope::Canvas;
  state.selected_imported_debug = {
      im2d::ImportedDebugSelectionKind::Artwork,
      1,
      0,
  };

  const im2d::ImportedArtworkOperationResult result =
      im2d::GroupSelectedImportedArtworkObjects(state);

  REQUIRE(result.success);
  REQUIRE(result.selected_count == 2);
  REQUIRE(state.imported_artwork.size() == 1);
  REQUIRE(state.selected_imported_artwork_id == result.created_artwork_id);
  REQUIRE(state.selected_imported_artwork_ids ==
          std::vector<int>{result.created_artwork_id});

  const im2d::ImportedArtwork &grouped_artwork = state.imported_artwork.front();
  REQUIRE(grouped_artwork.groups.size() == 3);
  const im2d::ImportedGroup *root_group =
      im2d::FindImportedGroup(grouped_artwork, grouped_artwork.root_group_id);
  REQUIRE(root_group != nullptr);
  REQUIRE(root_group->child_group_ids.size() == 2);
  REQUIRE(grouped_artwork.part.contributing_source_artwork_ids.size() == 2);
}

TEST_CASE("UngroupSelectedImportedArtworkObjects restores grouped artworks to "
          "multiple selected objects",
          "[canvas][imported]") {
  im2d::CanvasState state;
  state.imported_artwork.push_back(MakeArtworkWithId(1));
  state.imported_artwork.push_back(MakeArtworkWithId(2, 40.0f));
  state.next_imported_artwork_id = 3;
  state.next_imported_part_id = 3;
  state.selected_imported_artwork_id = 1;
  state.selected_imported_artwork_ids = {1, 2};
  state.selection_scope = im2d::ImportedArtworkSelectionScope::Canvas;
  state.selected_imported_debug = {
      im2d::ImportedDebugSelectionKind::Artwork,
      1,
      0,
  };

  const im2d::ImportedArtworkOperationResult group_result =
      im2d::GroupSelectedImportedArtworkObjects(state);
  REQUIRE(group_result.success);
  REQUIRE(state.imported_artwork.size() == 1);
  REQUIRE(state.selected_imported_artwork_id ==
          group_result.created_artwork_id);

  const im2d::ImportedArtwork &grouped_artwork = state.imported_artwork.front();
  REQUIRE(im2d::HasUngroupableImportedArtworkSelection(state, grouped_artwork));

  const im2d::ImportedArtworkOperationResult ungroup_result =
      im2d::UngroupSelectedImportedArtworkObjects(state);

  REQUIRE(ungroup_result.success);
  REQUIRE(state.imported_artwork.size() == 2);
  REQUIRE(state.selected_imported_artwork_ids.size() == 2);
  REQUIRE(state.selected_imported_debug.kind ==
          im2d::ImportedDebugSelectionKind::Artwork);
}

TEST_CASE("UngroupSelectedImportedArtworkObjects unwraps grouped artwork root "
          "contents when the artwork is selected",
          "[canvas][imported]") {
  im2d::CanvasState state = MakeCanvasState(MakeArtworkWithId(1));
  state.selected_imported_debug = {
      im2d::ImportedDebugSelectionKind::Artwork,
      state.selected_imported_artwork_id,
      0,
  };

  const im2d::ImportedArtworkOperationResult group_result =
      im2d::GroupImportedArtworkRootContents(
          state, state.selected_imported_artwork_id);
  REQUIRE(group_result.success);
  REQUIRE(state.imported_artwork.size() == 1);

  state.selected_imported_debug = {
      im2d::ImportedDebugSelectionKind::Artwork,
      state.selected_imported_artwork_id,
      0,
  };
  REQUIRE(im2d::HasUngroupableImportedArtworkSelection(
      state, state.imported_artwork.front()));

  const im2d::ImportedArtworkOperationResult ungroup_result =
      im2d::UngroupSelectedImportedArtworkObjects(state);

  REQUIRE(ungroup_result.success);
  REQUIRE(state.imported_artwork.size() == 1);
  const im2d::ImportedArtwork &artwork = state.imported_artwork.front();
  const im2d::ImportedGroup *root_group =
      im2d::FindImportedGroup(artwork, artwork.root_group_id);
  REQUIRE(root_group != nullptr);
  REQUIRE(root_group->child_group_ids.empty());
  REQUIRE(root_group->path_ids.size() == 2);
  REQUIRE(state.selected_imported_debug.kind ==
          im2d::ImportedDebugSelectionKind::Artwork);
}

TEST_CASE("RepairImportedArtworkOrphanHoles resolves orphan contours by "
          "reclassifying them",
          "[canvas][imported]") {
  im2d::ImportedArtwork artwork = MakeArtworkWithRoot({
      MakeRectanglePath(1, 0.0f, 0.0f, 10.0f, 10.0f, false),
      MakeRectanglePath(2, 20.0f, 20.0f, 24.0f, 24.0f, true, true),
  });
  im2d::CanvasState state = MakeCanvasState(std::move(artwork));

  REQUIRE(state.imported_artwork.front().part.orphan_hole_count == 1);
  REQUIRE(state.imported_artwork.front().part.hole_contour_count == 1);

  const im2d::ImportedArtworkOperationResult result =
      im2d::RepairImportedArtworkOrphanHoles(state,
                                             state.imported_artwork.front().id);

  REQUIRE(result.success);
  REQUIRE(result.repaired_hole_count == 1);
  REQUIRE(result.orphan_hole_count == 0);
  REQUIRE(state.imported_artwork.front().part.orphan_hole_count == 0);
  REQUIRE(state.imported_artwork.front().part.outer_contour_count == 2);
  REQUIRE(state.imported_artwork.front().part.hole_contour_count == 0);
}

TEST_CASE("ApplyImportedArtworkSelectionScope collapses object scope to one "
          "artwork and removes non-path element selections",
          "[canvas][imported]") {
  im2d::CanvasState state;
  state.imported_artwork.push_back(MakeArtworkWithId(1));
  state.imported_artwork.push_back(MakeArtworkWithId(2, 40.0f));
  state.selected_imported_artwork_id = 2;
  state.selected_imported_artwork_ids = {1, 2};
  state.selected_imported_elements = {
      {im2d::ImportedElementKind::DxfText, 9},
      {im2d::ImportedElementKind::Path, 2},
  };
  state.selected_imported_debug = {
      im2d::ImportedDebugSelectionKind::DxfText,
      2,
      9,
  };

  im2d::ApplyImportedArtworkSelectionScope(
      state, im2d::ImportedArtworkSelectionScope::Object);

  REQUIRE(state.selection_scope == im2d::ImportedArtworkSelectionScope::Object);
  REQUIRE(state.selected_imported_artwork_id == 2);
  REQUIRE(state.selected_imported_artwork_ids == std::vector<int>{2});
  REQUIRE(state.selected_imported_elements.size() == 1);
  REQUIRE(state.selected_imported_elements.front().kind ==
          im2d::ImportedElementKind::Path);
  REQUIRE(state.selected_imported_debug.kind ==
          im2d::ImportedDebugSelectionKind::Path);
  REQUIRE(state.selected_imported_debug.item_id == 2);

  im2d::ApplyImportedArtworkSelectionScope(
      state, im2d::ImportedArtworkSelectionScope::Canvas);

  REQUIRE(state.selection_scope == im2d::ImportedArtworkSelectionScope::Canvas);
  REQUIRE(state.selected_imported_elements.empty());
  REQUIRE(state.selected_imported_artwork_ids == std::vector<int>{2});
}

TEST_CASE("ResolveImportedArtworkOperationTargets follows selection scope",
          "[canvas][imported]") {
  im2d::CanvasState state;
  state.imported_artwork.push_back(MakeArtworkWithId(1));
  state.imported_artwork.push_back(MakeArtworkWithId(2, 40.0f));
  state.selected_imported_artwork_id = 2;
  state.selected_imported_artwork_ids = {1, 2};
  state.selection_scope = im2d::ImportedArtworkSelectionScope::Canvas;

  REQUIRE(im2d::ResolveImportedArtworkOperationTargets(state) ==
          std::vector<int>({1, 2}));

  im2d::ApplyImportedArtworkSelectionScope(
      state, im2d::ImportedArtworkSelectionScope::Object);

  REQUIRE(im2d::ResolveImportedArtworkOperationTargets(state) ==
          std::vector<int>({2}));
}

TEST_CASE("ApplyImportedArtworkOperationToSelection aggregates partial batch "
          "results",
          "[canvas][imported]") {
  im2d::CanvasState state;
  state.imported_artwork.push_back(MakeArtworkWithId(1));
  state.imported_artwork.push_back(MakeArtworkWithId(2, 40.0f));
  state.selected_imported_artwork_id = 1;
  state.selected_imported_artwork_ids = {1, 2};
  state.selection_scope = im2d::ImportedArtworkSelectionScope::Canvas;

  const im2d::ImportedArtworkOperationResult result =
      im2d::ApplyImportedArtworkOperationToSelection(
          state, state.selected_imported_artwork_id, "Test Batch",
          [](im2d::CanvasState &, const int artwork_id) {
            im2d::ImportedArtworkOperationResult item_result;
            item_result.artwork_id = artwork_id;
            item_result.success = artwork_id == 1;
            item_result.message = artwork_id == 1 ? "ok" : "failed";
            return item_result;
          });

  REQUIRE_FALSE(result.success);
  REQUIRE(result.selected_count == 2);
  REQUIRE(result.message.find("completed for 1 of 2") != std::string::npos);
  REQUIRE(result.message.find("Artwork 2: failed") != std::string::npos);
}

TEST_CASE("SelectImportedPathsInWorldRect only selects enclosed paths",
          "[canvas][imported]") {
  im2d::CanvasState state = MakeCanvasState(MakeArtworkWithId(1));

  const im2d::ImportedArtworkOperationResult result =
      im2d::SelectImportedPathsInWorldRect(
          state, state.selected_imported_artwork_id, ImVec2(-1.0f, -1.0f),
          ImVec2(9.0f, 9.0f), im2d::ImportedArtworkEditMode::SelectRectangle);

  REQUIRE(result.success);
  REQUIRE(result.selected_count == 1);
  REQUIRE(state.selected_imported_elements.size() == 1);
  REQUIRE(state.selected_imported_elements.front().kind ==
          im2d::ImportedElementKind::Path);
  REQUIRE(state.selected_imported_debug.kind ==
          im2d::ImportedDebugSelectionKind::Path);
}
