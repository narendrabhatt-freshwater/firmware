#pragma once

#include "app.hpp"

#include <string>

/** Persist / restore control_gui layout and connection preferences. */
namespace fw::settings
{

/** Absolute path to the INI under the user config directory. */
std::string Path();

bool Load(App &app);
bool Save(const App &app);

} // namespace fw::settings
