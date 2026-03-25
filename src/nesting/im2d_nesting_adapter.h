#pragma once

#include "../canvas/im2d_canvas_types.h"
#include "im2d_nesting_types.h"

#include <optional>
#include <vector>

namespace im2d::nesting {

std::optional<Sheet> ConvertWorkingAreaToSheet(const WorkingArea &working_area,
                                               const std::string &sheet_id = {},
                                               int quantity = 1);
std::vector<Part> ConvertImportedArtworkToParts(
    const ImportedArtwork &artwork,
    const std::vector<double> &allowed_rotations_degrees =
        std::vector<double>(),
    int quantity = 1);

} // namespace im2d::nesting