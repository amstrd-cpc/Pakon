#pragma once

// Machine-readable description of one scan, written next to the .pakraw
// as <prefix>.scan.json. It carries everything a later processing stage
// (flat-field, Digital ICE from the IR plane, colour/"Pakon look") needs
// that is not in the pixels: mode and geometry of both windows, this
// unit's EEPROM values, the Corrections results including the per-pixel
// dark and open-gate references, the film on-times, and where the film
// starts and ends.
//
// Schema "pakon-scan/1". Calibration references are measured on the
// calibration geometry (pixels cal_start..Offset+width); the film window
// starts at Offset, so film pixel p corresponds to calibration pixel
// p + (film.start - calibration.start) (same resample/binning).

#include <string>

#include "pakon/scan/runner.hpp"

namespace pakon::scan {

std::string sidecar_json(const ScanResult& result);

} // namespace pakon::scan
