#pragma once

#include "im2d_canvas_types.h"

namespace im2d {

const char *MeasurementUnitLabel(MeasurementUnit unit);
float GetPixelsPerMillimeter(const PhysicalCalibration &calibration);
float UnitsToMillimeters(float value, MeasurementUnit unit);
float MillimetersToUnits(float value, MeasurementUnit unit);
float UnitsToPixels(float value, MeasurementUnit unit,
                    const PhysicalCalibration &calibration);
float PixelsToUnits(float value, MeasurementUnit unit,
                    const PhysicalCalibration &calibration);
bool ApplyCalibration(PhysicalCalibration &calibration);
bool ApplyCalibration(PhysicalCalibration &calibration, float reference_pixels,
                      float measured_length, MeasurementUnit measured_unit);

} // namespace im2d