#pragma once

/** macOS menu-bar name + About panel (no-op on other platforms). */
#if defined(__APPLE__)
extern "C" void fw_macos_apply_branding(void);
#else
inline void fw_macos_apply_branding(void) {}
#endif
