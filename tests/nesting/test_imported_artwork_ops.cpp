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
