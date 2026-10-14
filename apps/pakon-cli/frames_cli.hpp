#pragma once

// The `frames` command: split a film window into its pictures
// (process/frames.hpp). Offline: reads <prefix>.film.pakraw and
// <prefix>.scan.json, never opens a device. Writes one linear 16-bit RGB
// TIFF per frame (the raw negative, cropped losslessly; width = film
// travel), an IR TIFF per frame in IR modes, and <prefix>.frames.json.
//
// Exit codes: 0 success, 1 runtime failure, 2 usage error.

#include <cstdio>
#include <string>
#include <vector>

namespace pakon::cli {

int run_frames_command(const std::vector<std::string>& args, std::FILE* out, std::FILE* err);
void print_frames_usage(std::FILE* out);

} // namespace pakon::cli
