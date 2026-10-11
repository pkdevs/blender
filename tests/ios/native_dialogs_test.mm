/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* Production native-dialog implementation is inserted above this harness. */
#include <cassert>
#include <thread>

static GHOST_WindowIOS ghost;
static int calls, choice;
static uint64_t selected;
static UIWindow *testWindow;
static void complete(uint64_t, int action, uint64_t toggles)
{
  assert(NSThread.isMainThread);
  assert(!testWindow.rootViewController.presentedViewController);
  calls++;
  choice = action;
  selected = toggles;
}
static void waitUntil(BOOL (^ready)(), void (^next)(), int attempts = 500)
{
  if (ready()) {
    next();
    return;
  }
  assert(attempts > 0);
  dispatch_after(
      dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_MSEC * 10), dispatch_get_main_queue(), ^{
        waitUntil(ready, next, attempts - 1);
      });
}
static void capture(NSString *name)
{
  UIGraphicsImageRenderer *renderer = [[[UIGraphicsImageRenderer alloc]
      initWithSize:testWindow.bounds.size] autorelease];
  UIImage *image = [renderer imageWithActions:^(UIGraphicsImageRendererContext *) {
    [testWindow drawViewHierarchyInRect:testWindow.bounds afterScreenUpdates:YES];
  }];
  [UIImagePNGRepresentation(image)
      writeToFile:[[NSHomeDirectory() stringByAppendingPathComponent:@"Documents"]
                      stringByAppendingPathComponent:name]
       atomically:YES];
}
/* Stands in for the SwiftUI options sheet
 * (`source/blender/windowmanager/apple_ios/swift/DialogSheet.swift`), which this harness does not
 * build: it keeps what the dialog passes and chooses like the sheet's buttons. */
@interface WMIOSSwiftDialogSheet : UIViewController
@property(nonatomic, copy) NSArray<NSString *> *toggles;
@property(nonatomic, copy) NSArray<NSNumber *> *selected;
@property(nonatomic, copy) NSArray<NSString *> *actions;
@property(nonatomic, copy) NSArray<NSNumber *> *roles;
@property(nonatomic) NSInteger preferred;
@property(nonatomic, copy) void (^choose)(NSInteger action, uint64_t toggles);
@end
@implementation WMIOSSwiftDialogSheet
- (instancetype)initWithTitle:(NSString *)title
                      message:(NSString *)message
                      toggles:(NSArray<NSString *> *)toggles
                     selected:(NSArray<NSNumber *> *)selected
                      actions:(NSArray<NSString *> *)actions
                        roles:(NSArray<NSNumber *> *)roles
                    preferred:(NSInteger)preferred
                       choose:(void (^)(NSInteger action, uint64_t toggles))choose
{
  if ((self = [super initWithNibName:nil bundle:nil])) {
    self.title = title;
    self.toggles = toggles;
    self.selected = selected;
    self.actions = actions;
    self.roles = roles;
    self.preferred = preferred;
    self.choose = choose;
  }
  return self;
}
- (void)dealloc
{
  [_toggles release];
  [_selected release];
  [_actions release];
  [_roles release];
  [_choose release];
  [super dealloc];
}
@end

static void testSheet()
{
  wmIOSDialog model;
  model.title = "Save changes before closing?";
  model.message = "Untitled.blend\nModified images and asset catalogs";
  model.actions = {
      {"Cancel", wmIOSDialogRole::Cancel}, {"Don't Save", wmIOSDialogRole::Destructive}, {"Save"}};
  model.toggles = {{"Save 2 modified images", true}, {"Save modified asset catalogs", false}};
  model.preferred_action = 2;
  assert(wm_apple_ios_dialog_begin(&ghost, 2, model, complete));
  waitUntil(
      ^BOOL {
        return !testWindow.rootViewController.presentedViewController.isBeingPresented &&
               [testWindow.rootViewController.presentedViewController
                   isKindOfClass:WMIOSSwiftDialogSheet.class];
      },
      ^{
        WMIOSSwiftDialogSheet *sheet = (id)testWindow.rootViewController.presentedViewController;
        assert([sheet.title isEqual:@"Save changes before closing?"]);
        assert(([sheet.toggles
            isEqual:@[ @"Save 2 modified images", @"Save modified asset catalogs" ]]));
        assert(([sheet.selected isEqual:@[ @YES, @NO ]]));
        assert(([sheet.actions isEqual:@[ @"Cancel", @"Don't Save", @"Save" ]]));
        assert(([sheet.roles isEqual:@[ @1, @2, @0 ]]) && sheet.preferred == 2);
        /* The sheet's options were flipped: only the second is chosen. */
        sheet.choose(2, 2);
        waitUntil(
            ^BOOL {
              return calls == 2;
            },
            ^{
              assert(choice == 2 && selected == 2);
              wmIOSDialog report;
              report.title = "Error";
              report.message = "Unable to save this file.";
              report.actions = {{"OK"}};
              wm_apple_ios_dialog_begin(&ghost, 3, report, nullptr);
              wm_apple_ios_dialog_begin(&ghost, 4, report, nullptr);
              waitUntil(
                  ^BOOL {
                    return dialog_queue.count > 0 &&
                           testWindow.rootViewController.presentedViewController != nil;
                  },
                  ^{
                    assert(dialog_queue.count == 1);
                    wm_apple_ios_dialog_cancel(3);
                    waitUntil(
                        ^BOOL {
                          return dialog_queue.count == 0;
                        },
                        ^{
                          assert(calls == 2);
                          ghost.input_alive->store(false);
                          wm_apple_ios_dialog_begin(&ghost, 5, report, complete);
                          waitUntil(
                              ^BOOL {
                                return calls == 3;
                              },
                              ^{
                                assert(choice == -1 && dialog_queue.count == 0);
                                printf(
                                    "PASS: native alerts, destructive styles, options, "
                                    "dismissal, deduplication and lifetime\n");
                                fflush(stdout);
                                exit(0);
                              });
                        });
                  });
            });
      });
}
@interface DialogTestScene : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, retain) UIWindow *window;
@end
@implementation DialogTestScene
- (void)scene:(UIScene *)scene
    willConnectToSession:(UISceneSession *)session
                 options:(UISceneConnectionOptions *)options
{
  self.window = [[[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene] autorelease];
  self.window.rootViewController = [[[UIViewController alloc] init] autorelease];
  self.window.rootViewController.view.backgroundColor = UIColor.systemBackgroundColor;
  [self.window makeKeyAndVisible];
  testWindow = self.window;
  ghost.rootWindow = self.window;
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_main_queue(), ^{
    wmIOSDialog model;
    model.title = "Delete selected objects?";
    model.actions = {{"Cancel", wmIOSDialogRole::Cancel},
                     {"Delete", wmIOSDialogRole::Destructive}};
    model.preferred_action = 0;
    std::thread engine([model] { assert(wm_apple_ios_dialog_begin(&ghost, 1, model, complete)); });
    engine.join();
    waitUntil(
        ^BOOL {
          return !testWindow.rootViewController.presentedViewController.isBeingPresented &&
                 [testWindow.rootViewController.presentedViewController
                     isKindOfClass:UIAlertController.class];
        },
        ^{
          UIAlertController *alert = (id)testWindow.rootViewController.presentedViewController;
          assert([alert.title isEqual:@"Delete selected objects?"]);
          assert(alert.actions.count == 2);
          assert(alert.actions[0].style == UIAlertActionStyleCancel);
          assert(alert.actions[1].style == UIAlertActionStyleDestructive);
          assert(alert.preferredAction == alert.actions[0]);
          capture(@"native-delete-warning.png");
          WMIOSDialogRequest *request = dialog_queue.firstObject;
          [request finish:0];
          [request finish:1];  // A double callback must not execute the destructive action.
          waitUntil(
              ^BOOL {
                return calls == 1;
              },
              ^{
                assert(choice == 0);
                testSheet();
              });
        });
  });
}
@end
@interface DialogTestApp : UIResponder <UIApplicationDelegate>
@end
@implementation DialogTestApp
@end
int main(int argc, char **argv)
{
  @autoreleasepool {
    return UIApplicationMain(argc, argv, nil, @"DialogTestApp");
  }
}
