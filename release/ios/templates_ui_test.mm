/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#import <UIKit/UIKit.h>
#import <objc/runtime.h>
#include "GHOST_IOSRuntime.hh"
#import "GHOST_IOSDocument.hh"
#import "GHOST_IOSLauncher.hh"

static int engine_calls = 0;
void GHOST_ios_engine_perform(std::function<void(blender::bContext *)>) { engine_calls++; }
bool GHOST_ios_app_in_background() { return false; }
void GHOST_ios_document_window_set_visible(bool, bool) {}
bool GHOST_ios_document_window_visible() { return false; }
UIView *GHOST_ios_document_window_snapshot() { return nil; }
namespace blender {
bool WM_apple_ios_document_open(bContext *, const char *, bool) { return false; }
bool WM_apple_ios_document_write(bContext *, const char *, bool) { return false; }
std::string WM_apple_ios_document_template_write(bContext *, const char *, const char *, std::string &) { return "test"; }
}

@interface NSObject (TemplateTests)
@property(nonatomic, copy) void (^completion)(NSString *);
- (void)finish:(NSString *)identifier;
- (void)updateSearchResultsForSearchController:(UISearchController *)searchController;
- (void)documentBrowser:(UIDocumentBrowserViewController *)browser
    didRequestDocumentCreationWithHandler:(void (^)(NSURL *, UIDocumentBrowserImportMode))handler;
@end

static NSUInteger template_requests;
static void chooseTemplateSpy(id, SEL, void (^completion)(NSString *))
{
  template_requests++;
  completion(nil);
}

static void testCopies(NSArray *entries, NSUInteger index, void (^done)(void))
{
  if (index == entries.count) { done(); return; }
  NSDictionary *entry = entries[index];
  NSData *original = [NSData dataWithContentsOfURL:entry[@"url"]];
  GHOST_ios_document_create(entry[@"id"], ^(NSURL *url) {
    NSCAssert(NSThread.isMainThread && url, @"Creation completes on UIKit with a new file");
    NSCAssert(![url isEqual:entry[@"url"]] && [[NSData dataWithContentsOfURL:url] isEqual:original],
              @"A document is an independent byte-identical seed copy");
    NSCAssert(engine_calls == 0, @"Creating a template must never replace Blender's live scene");
    [NSFileManager.defaultManager removeItemAtURL:url.URLByDeletingLastPathComponent error:nil];
    testCopies(entries, index + 1, done);
  });
}

@interface TemplatesTestScene : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, retain) UIWindow *window;
@end
@implementation TemplatesTestScene
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options
{
  self.window = [[[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene] autorelease];
  NSArray *arguments = NSProcessInfo.processInfo.arguments;
  self.window.overrideUserInterfaceStyle = [arguments containsObject:@"--light"] ?
      UIUserInterfaceStyleLight : UIUserInterfaceStyleDark;
  /* The chooser itself is SwiftUI, covered by `test_swiftui_panels.py`; this checks the catalog
   * and the launcher's flows around it. */
  self.window.rootViewController = [[[UIViewController alloc] init] autorelease];
  [self.window makeKeyAndVisible];
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_main_queue(), ^{
    NSArray *entries = GHOST_ios_document_templates();
    NSCAssert(entries.count == 6, @"All validated templates are available");
    NSDictionary *character = entries.lastObject;
    NSCAssert([character[@"id"] isEqual:@"character-animation"] &&
              [character[@"attribution"][@"license"] isEqual:@"CC-BY-4.0"],
              @"The character catalog includes its asset license");
    NSCAssert(GHOST_ios_template_manage(@"character-animation", @"duplicate", nil),
              @"Attributed templates can be duplicated");
    NSDictionary *characterCopy = GHOST_ios_document_templates().lastObject;
    NSCAssert([characterCopy[@"attribution"] isEqual:character[@"attribution"]],
              @"Duplication retains the complete attribution");
    NSCAssert(GHOST_ios_template_manage(characterCopy[@"id"], @"delete", nil), @"Remove test copy");
    NSCAssert(GHOST_ios_template_manage(@"general", @"duplicate", nil), @"Bundled templates can be duplicated");
    NSArray *all = GHOST_ios_document_templates();
    NSCAssert(all.count == 7 && [all.lastObject[@"personal"] boolValue], @"Copies appear under My Templates");
    NSString *copyID = all.lastObject[@"id"];
    NSCAssert(GHOST_ios_template_manage(copyID, @"rename", @"Custom Scene"), @"Personal templates can be renamed");
    NSCAssert([GHOST_ios_document_templates().lastObject[@"title"] isEqual:@"Custom Scene"], @"Names persist");
    NSCAssert(!GHOST_ios_template_manage(@"general", @"delete", nil), @"Bundled templates are read-only");
    NSCAssert(GHOST_ios_template_manage(copyID, @"delete", nil) && GHOST_ios_document_templates().count == 6,
              @"Deleting a personal template preserves bundled templates");
    Class launcherClass = NSClassFromString(@"GHOSTIOSLauncherController");
    Class spyClass = objc_allocateClassPair(launcherClass, "TemplatesLauncherSpy", 0);
    SEL choose = NSSelectorFromString(@"chooseTemplate:");
    class_addMethod(spyClass, choose, (IMP)chooseTemplateSpy,
                    method_getTypeEncoding(class_getInstanceMethod(launcherClass, choose)));
    objc_registerClassPair(spyClass);
    UIDocumentViewController *launcher = [[[spyClass alloc] initWithDocument:nil] autorelease];
    __block BOOL cancelledImport = NO;
    [launcher documentBrowser:nil didRequestDocumentCreationWithHandler:^(NSURL *url, UIDocumentBrowserImportMode mode) {
      cancelledImport = url == nil && mode == UIDocumentBrowserImportModeNone;
    }];
    NSCAssert(template_requests == 1 && cancelledImport,
              @"The browser's default New action opens the chooser and cancellation creates nothing");
    GHOST_ios_document_create(@"../sculpt", ^(NSURL *url) {
      NSCAssert(url == nil, @"Arbitrary paths cannot be used as templates");
    });
    testCopies(entries, 0, ^{
      NSString *path = [NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject
          stringByAppendingPathComponent:@"templates.png"];
      UIGraphicsImageRenderer *renderer = [[[UIGraphicsImageRenderer alloc] initWithBounds:self.window.bounds] autorelease];
      UIImage *image = [renderer imageWithActions:^(UIGraphicsImageRendererContext *) {
        [self.window drawViewHierarchyInRect:self.window.bounds afterScreenUpdates:YES];
      }];
      [UIImagePNGRepresentation(image) writeToFile:path atomically:YES];
      NSLog(@"PASS: native templates");
      exit(0);
    });
  });
}
@end

@interface TemplatesTestApp : UIResponder <UIApplicationDelegate>
@end
@implementation TemplatesTestApp
@end
int main(int argc, char **argv)
{
  @autoreleasepool { return UIApplicationMain(argc, argv, nil, @"TemplatesTestApp"); }
}
