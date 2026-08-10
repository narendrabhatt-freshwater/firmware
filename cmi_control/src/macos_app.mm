/**
 * macOS menu-bar branding for CMI Control.
 * Retargets GLFW's About item to a filled standard About panel and renames
 * the application menu from the process binary name to CMI.
 */

#import <AppKit/AppKit.h>

#include "product.hpp"

@interface CMIAboutTarget : NSObject
- (void)showAbout:(id)sender;
@end

@implementation CMIAboutTarget
- (void)showAbout:(id)sender
{
  (void)sender;
  NSMutableDictionary *opts = [NSMutableDictionary dictionary];
  opts[NSAboutPanelOptionApplicationName] = @"CMI Control";
  opts[NSAboutPanelOptionApplicationVersion] =
      [NSString stringWithUTF8String:fw::product::kVersion];
  opts[NSAboutPanelOptionVersion] =
      [NSString stringWithFormat:@"%s build", fw::product::kBuildType];
  opts[@"Copyright"] = [NSString stringWithUTF8String:fw::product::kCopyright];
  opts[@"Credits"] = [[NSAttributedString alloc]
      initWithString:[NSString stringWithUTF8String:fw::product::kTagline]];
  [NSApp orderFrontStandardAboutPanelWithOptions:opts];
}
@end

static CMIAboutTarget *g_about_target = nil;

extern "C" void fw_macos_apply_branding(void)
{
  @autoreleasepool {
    [[NSProcessInfo processInfo]
        setProcessName:[NSString stringWithUTF8String:fw::product::kName]];

    NSMenu *main = [NSApp mainMenu];
    if (!main || main.numberOfItems < 1) {
      return;
    }
    NSMenu *app_menu = [[main itemAtIndex:0] submenu];
    if (!app_menu) {
      return;
    }
    [app_menu setTitle:@"CMI"];

    if (!g_about_target) {
      g_about_target = [[CMIAboutTarget alloc] init];
    }
    for (NSMenuItem *item in app_menu.itemArray) {
      if ([item.title hasPrefix:@"About"]) {
        item.title = @"About CMI Control";
        item.target = g_about_target;
        item.action = @selector(showAbout:);
      } else if ([item.title hasPrefix:@"Hide "] &&
                 ![item.title isEqualToString:@"Hide Others"]) {
        item.title = @"Hide CMI";
      } else if ([item.title hasPrefix:@"Quit "]) {
        item.title = @"Quit CMI";
      }
    }
  }
}
