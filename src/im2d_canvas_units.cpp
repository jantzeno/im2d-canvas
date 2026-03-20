#include "im2d_canvas_units.h"

namespace im2d {

const char *MeasurementUnitLabel(MeasurementUnit unit) {
  switch (unit) {
  case MeasurementUnit::Pixels:
    return "px";
  case MeasurementUnit::Millimeters:
    return "mm";
  case MeasurementUnit::Inches:
    return "in";
  }

  return "?";
}

float GetPixelsPerMillimeter(const PhysicalCalibration &calibration) {
  return calibration.enabled ? calibration.calibrated_pixels_per_mm
                             : calibration.default_pixels_per_mm;
}

float UnitsToMillimeters(float value, MeasurementUnit unit) {
  switch (unit) {
  case MeasurementUnit::Pixels:
    return value;
  case MeasurementUnit::Millimeters:
    return value;
  case MeasurementUnit::Inches:
    return value * 25.4f;
  }

  return value;
}

float MillimetersToUnits(float value, MeasurementUnit unit) {
  switch (unit) {
  case MeasurementUnit::Pixels:
    return value;
  case MeasurementUnit::Millimeters:
    return value;
  case MeasurementUnit::Inches:
    return value / 25.4f;
  }

  return value;
}

float UnitsToPixels(float value, MeasurementUnit unit,
                    const PhysicalCalibration &calibration) {
  switch (unit) {
  case MeasurementUnit::Pixels:
    return value;
  case MeasurementUnit::Millimeters:
    return value * GetPixelsPerMillimeter(calibration);
  case MeasurementUnit::Inches:
    return value * GetPixelsPerMillimeter(calibration) * 25.4f;
  }

  return value;
}

float PixelsToUnits(float value, MeasurementUnit unit,
                    const PhysicalCalibration &calibration) {
  switch (unit) {
  case MeasurementUnit::Pixels:
    return value;
  case MeasurementUnit::Millimeters:
    return value / GetPixelsPerMillimeter(calibration);
  case MeasurementUnit::Inches:
    return value / (GetPixelsPerMillimeter(calibration) * 25.4f);
  }

  return value;
}

bool ApplyCalibration(PhysicalCalibration &calibration) {
  return ApplyCalibration(calibration, calibration.reference_pixels,
                          calibration.measured_length,
                          calibration.measured_unit);
}

bool ApplyCalibration(PhysicalCalibration &calibration, float reference_pixels,
                      float measured_length, MeasurementUnit measured_unit) {
  const float measured_mm = UnitsToMillimeters(measured_length, measured_unit);
  if (reference_pixels <= 0.0f || measured_mm <= 0.0f) {
    return false;
  }

  calibration.reference_pixels = reference_pixels;
  calibration.measured_length = measured_length;
  calibration.measured_unit = measured_unit;
  calibration.calibrated_pixels_per_mm = reference_pixels / measured_mm;
  calibration.enabled = true;
  return true;
}

} // namespace im2d