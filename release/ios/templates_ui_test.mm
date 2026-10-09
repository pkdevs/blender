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
    NSCAssert(entries.count == 6, @"All validated templates are available");
    NSCAssert([picker.collectionView numberOfItemsInSection:0] == 6, @"Native chooser lists each template");
    NSCAssert(picker.navigationItem.rightBarButtonItem == nil,
              @"Templates open directly, without a separate Create button");
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
    NSDictionary *character = entries.lastObject;
    NSCAssert([character[@"id"] isEqual:@"character-animation"] &&
              [character[@"attribution"][@"license"] isEqual:@"CC-BY-4.0"],
              @"The character catalog includes its asset license");
    NSIndexPath *characterIndex = [NSIndexPath indexPathForItem:5 inSection:0];
    [picker.collectionView scrollToItemAtIndexPath:characterIndex
        atScrollPosition:UICollectionViewScrollPositionCenteredVertically animated:NO];
    [picker.collectionView layoutIfNeeded];
    UICollectionViewCell *characterCell = [picker.collectionView cellForItemAtIndexPath:characterIndex];
    UIButton *credit = [characterCell valueForKey:@"credit"];
    NSCAssert(!credit.hidden && [credit.currentTitle containsString:@"Blender Studio"] &&
              [credit.currentTitle containsString:@"CC-BY-4.0"], @"Credit is visible in the chooser");
    [UIView setAnimationsEnabled:NO];
    [credit sendActionsForControlEvents:UIControlEventTouchUpInside];
    [navigation.view layoutIfNeeded];
    UIViewController *details = navigation.topViewController;
    [details loadViewIfNeeded];
    NSCAssert([details.title isEqual:@"Asset Credits"], @"Credits open without creating a document");
    UITextView *creditText = details.view.subviews.firstObject;
    NSCAssert([creditText isKindOfClass:UITextView.class] &&
              [creditText.text containsString:character[@"attribution"][@"changes"]],
              @"The credit view identifies modifications");
    for (NSString *key in @[@"sourceURL", @"licenseURL"]) {
      NSRange range = [creditText.text rangeOfString:character[@"attribution"][key]];
      NSCAssert(range.location != NSNotFound &&
                [creditText.attributedText attribute:NSLinkAttributeName atIndex:range.location effectiveRange:nil],
                @"Source and license are actionable links");
    }
    [navigation popViewControllerAnimated:NO];
    [navigation.view layoutIfNeeded];
    [UIView setAnimationsEnabled:YES];
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
    NSObject *cancelled = [[[NSClassFromString(@"GHOSTIOSTemplatePicker") alloc] init] autorelease];
    __block NSUInteger cancellations = 0;
    cancelled.completion = ^(NSString *identifier) {
      NSCAssert(identifier == nil, @"Cancel does not create a document");
      cancellations++;
    };
    [cancelled finish:nil];
    [cancelled finish:nil];
    NSCAssert(cancellations == 1, @"Cancellation completes only once");
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
    __block NSUInteger completions = 0;
    picker.completion = ^(NSString *identifier) {
      completions++;
      NSCAssert([identifier isEqual:@"sculpt"], @"Create uses the selected stable template ID");
    };
    NSIndexPath *index = [NSIndexPath indexPathForItem:1 inSection:0];
    NSCAssert(completions == 0, @"Browsing templates does not create a document");
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
      [picker.collectionView selectItemAtIndexPath:index animated:NO scrollPosition:UICollectionViewScrollPositionNone];
      [picker.collectionView.delegate collectionView:picker.collectionView didSelectItemAtIndexPath:index];
      [picker.collectionView.delegate collectionView:picker.collectionView didSelectItemAtIndexPath:index];
      dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        NSCAssert(completions == 1, @"Tapping a template creates exactly one document, even after repeated taps");
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
