#pragma once

/** Product identity for the control GUI — keep branding here, not scattered. */
namespace fw::product
{

constexpr const char *kName = "CMI";
constexpr const char *kTitle = "CMI Control";
constexpr const char *kTagline =
    "Desktop control for Channel and Effect cards";
constexpr const char *kCopyright = "Copyright © CMI";
constexpr const char *kBundleId = "com.cmi.control";

#ifndef CMI_VERSION
#define CMI_VERSION "0.0.0"
#endif
#ifndef CMI_BUILD_TYPE
#define CMI_BUILD_TYPE "Unknown"
#endif

constexpr const char *kVersion = CMI_VERSION;
constexpr const char *kBuildType = CMI_BUILD_TYPE;

} // namespace fw::product
