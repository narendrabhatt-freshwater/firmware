#ifndef VOICEBD_ENVELOPE_CONTROLS_H
#define VOICEBD_ENVELOPE_CONTROLS_H

#include <filesystem>
#include <string>

namespace envelope_controls {

struct settings {
    double attack_ms = 5000.0;
    double sustain = 1.0;
    double release_ms = 5.0;
    double gain = 1.0;
    bool enabled = true;
};

enum class action { apply, status, help };
struct command {
    action kind;
    settings value;
};

// Parse a complete colon command without changing the currently applied settings.
command parse(std::string const& line, settings const& current);
std::string describe(settings const& value);
std::string program(settings const& value);
void compile(std::filesystem::path const& compiler,
             std::filesystem::path const& source,
             std::filesystem::path const& bytecode);

} // namespace envelope_controls
#endif
