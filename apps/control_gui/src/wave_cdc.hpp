#pragma once

#include "log_buffer.hpp"

#include <cstdint>
#include <functional>
#include <string>

/**
 * Load a raw int16 LE mono file into a Channel Card wave slot over USB CDC.
 * File must be even length, 2..32768 bytes (no WAV header).
 * Optional progress callback: 0..1 as bytes are written.
 */
bool WaveCdc_LoadRaw(const std::string &cdc_path,
                     int slot,
                     const std::string &file_path,
                     LogBuffer &log,
                     const std::function<void(float)> &on_progress = {});

/** Best-effort native file picker; returns empty string if cancelled. */
std::string WaveCdc_PickRawFile();

/** Pick a folder; returns empty if cancelled. */
std::string WaveCdc_PickFolder();
