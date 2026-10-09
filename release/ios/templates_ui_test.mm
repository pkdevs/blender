/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#import <UIKit/UIKit.h>
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
std::string WM_apple_ios_document_template_write(bContext *, const char *, const char *) { return "test"; }
}

@interface NSObject (TemplateTests)
@property(nonatomic, copy) void (^completion)(NSString *);
- (void)createSelected:(id)sender;
- (void)finish:(NSString *)identifier;
- (void)updateSearchResultsForSearchController:(UISearchController *)searchController;
@end

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
  UICollectionViewController *picker = [[[NSClassFromString(@"GHOSTIOSTemplatePicker") alloc] init] autorelease];
  UINavigationController *navigation = [[[UINavigationController alloc] initWithRootViewController:picker] autorelease];
  const BOOL sheet = [arguments containsObject:@"--sheet"];
  self.window.rootViewController = sheet ? [[[UIViewController alloc] init] autorelease] : navigation;
  [self.window makeKeyAndVisible];
  if ([arguments containsObject:@"--large-text"]) {
    navigation.traitOverrides.preferredContentSizeCategory = UIContentSizeCategoryAccessibilityExtraExtraExtraLarge;
  }
  if (sheet) {
    navigation.modalPresentationStyle = UIModalPresentationFormSheet;
    navigation.preferredContentSize = picker.preferredContentSize;
    [self.window.rootViewController presentViewController:navigation animated:NO completion:nil];
  }
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_main_queue(), ^{
    NSArray *entries = GHOST_ios_document_templates();
    NSCAssert(entries.count == 5, @"All validated templates are available");
    NSCAssert([picker.collectionView numberOfItemsInSection:0] == 5, @"Native chooser lists each template");
    UISearchController *search = picker.navigationItem.searchController;
    NSCAssert(search != nil, @"The chooser uses the system search controller");
    search.searchBar.text = @"sculpt";
    [picker updateSearchResultsForSearchController:search];
    NSCAssert([picker.collectionView numberOfItemsInSection:0] == 1, @"Search filters template names");
    search.searchBar.text = @"No matching template";
    [picker updateSearchResultsForSearchController:search];
    NSCAssert(picker.collectionView.numberOfSections == 0 && picker.contentUnavailableConfiguration &&
              !picker.navigationItem.rightBarButtonItem.enabled, @"Empty searches use the native unavailable state");
    search.searchBar.text = @"";
    [picker updateSearchResultsForSearchController:search];
    NSCAssert(GHOST_ios_template_manage(@"general", @"duplicate", nil), @"Bundled templates can be duplicated");
    NSArray *all = GHOST_ios_document_templates();
    NSCAssert(all.count == 6 && [all.lastObject[@"personal"] boolValue], @"Copies appear under My Templates");
    NSString *copyID = all.lastObject[@"id"];
    NSCAssert(GHOST_ios_template_manage(copyID, @"rename", @"Custom Scene"), @"Personal templates can be renamed");
    NSCAssert([GHOST_ios_document_templates().lastObject[@"title"] isEqual:@"Custom Scene"], @"Names persist");
    NSCAssert(!GHOST_ios_template_manage(@"general", @"delete", nil), @"Bundled templates are read-only");
    NSCAssert(GHOST_ios_template_manage(copyID, @"delete", nil) && GHOST_ios_document_templates().count == 5,
              @"Deleting a personal template preserves bundled templates");
    NSObject *cancelled = [[[NSClassFromString(@"GHOSTIOSTemplatePicker") alloc] init] autorelease];
    __block NSUInteger cancellations = 0;
    cancelled.completion = ^(NSString *identifier) {
      NSCAssert(identifier == nil, @"Cancel does not create a document");
      cancellations++;
    };
    [cancelled finish:nil];
    [cancelled finish:nil];
    NSCAssert(cancellations == 1, @"Cancellation completes only once");
    __block NSUInteger completions = 0;
    picker.completion = ^(NSString *identifier) {
      completions++;
      NSCAssert([identifier isEqual:@"sculpt"], @"Create uses the selected stable template ID");
    };
    NSIndexPath *index = [NSIndexPath indexPathForItem:1 inSection:0];
    [picker.collectionView selectItemAtIndexPath:index animated:NO scrollPosition:UICollectionViewScrollPositionNone];
    [picker.collectionView.delegate collectionView:picker.collectionView didSelectItemAtIndexPath:index];
    NSCAssert(completions == 0, @"Selecting a card does not create a document");
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
      [picker createSelected:nil];
      [picker createSelected:nil];
      dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        NSCAssert(completions == 1, @"Create completes exactly once, even after repeated taps");
        NSLog(@"PASS: native templates");
        exit(0);
      });
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
