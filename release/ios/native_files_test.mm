/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* Production document-picker implementation is inserted above this harness. */
#include <cassert>

static int calls;
static std::vector<std::string> picked;
static std::string pickerError;
static UIWindow *testWindow;

static void complete(uint64_t, const char *const *paths, int count, const char *error)
{
  calls++;
  picked.clear();
  for (int i = 0; i < count; i++)
    picked.emplace_back(paths[i]);
  pickerError = error ?: "";
}

@interface PickerHarness : GHOSTIOSDocumentPicker
@end
@implementation PickerHarness
- (void)present:(UIViewController *)controller
{
  self.controller = controller;
}
@end

@interface DismissablePicker : UIDocumentPickerViewController
@end
@implementation DismissablePicker
- (void)dismissViewControllerAnimated:(BOOL)animated completion:(void (^)(void))completion
{
  if (completion)
    completion();
}
@end

static PickerHarness *makePicker(GHOST_IOSDocumentMode mode)
{
  PickerHarness *picker = [[[PickerHarness alloc] init] autorelease];
  picker.request = 42;
  picker.mode = mode;
  picker.title = @"Save As";
  picker.initialPath = @"/folder/Model.blend";
  picker.types = @[ UTTypeData ];
  picker.multiple = YES;
  picker.scopedURLs = [NSMutableArray array];
  picker.completion = complete;
  return picker;
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

static void checkRouting(void (^finished)())
{
  GHOST_ios_document_access_restore();
  assert([documentFilename(@"  Model  ", @"blend") isEqual:@"Model.blend"]);
  assert([documentFilename(@"Model.BLEND", @"blend") isEqual:@"Model.BLEND"]);
  assert([documentFilename(@"v1.2", @"blend") isEqual:@"v1.2.blend"]);
  for (NSString *invalid in @[ @"", @"  ", @".", @"..", @"../file", @"file/name", @"a:b" ]) {
    assert(documentFilename(invalid, @"blend") == nil);
  }
  NSArray *types = documentContentTypes(@"public.image", @"*.png;*.jpeg;*.png");
  assert(types.count == 2 && [types[0] conformsToType:UTTypeImage]);
  assert([documentContentTypes(@"public.audio", @"")[0] conformsToType:UTTypeAudio]);
  assert([documentContentTypes(@"", @"*")[0] isEqual:UTTypeData]);
  assert(documentPathIsInside(@"/folder/child/file.blend", @"/folder"));
  assert(!documentPathIsInside(@"/folder-other/file.blend", @"/folder"));

  PickerHarness *picker = makePicker(GHOST_IOSDocumentMode::Open);
  [picker start];
  assert([picker.controller isKindOfClass:UIDocumentPickerViewController.class]);
  UIDocumentPickerViewController *controller = (id)picker.controller;
  assert(controller.allowsMultipleSelection);
  assert([controller.directoryURL.path isEqual:@"/folder"]);
  assert(calls == 0);
  [picker documentPickerWasCancelled:controller];
  [picker presentationControllerDidDismiss:nil];
  assert(calls == 1 && picked.empty() && pickerError.empty());

  NSString *root = [NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
  [NSFileManager.defaultManager createDirectoryAtPath:root
                          withIntermediateDirectories:YES
                                           attributes:nil
                                                error:nil];
  NSURL *first = [NSURL fileURLWithPath:[root stringByAppendingPathComponent:@"First.blend"]];
  NSURL *second = [NSURL fileURLWithPath:[root stringByAppendingPathComponent:@"Second.blend"]];
  [@"fixture" writeToURL:first atomically:YES encoding:NSUTF8StringEncoding error:nil];
  [@"fixture" writeToURL:second atomically:YES encoding:NSUTF8StringEncoding error:nil];
  picker = makePicker(GHOST_IOSDocumentMode::Open);
  [picker documentPicker:nil didPickDocumentsAtURLs:@[ first, second ]];
  assert(calls == 2 && picked.size() == 2 && picked[0] == first.path.UTF8String);
  assert(pickerError.empty());
  picker = makePicker(GHOST_IOSDocumentMode::Open);
  [picker documentPicker:nil
      didPickDocumentsAtURLs:@[ first, [NSURL fileURLWithPath:@"/other/file.blend"] ]];
  assert(calls == 3 && picked.empty() && !pickerError.empty());

  picker = makePicker(GHOST_IOSDocumentMode::Save);
  [picker start];
  assert([picker.controller isKindOfClass:UIAlertController.class]);
  UIAlertController *name = (id)picker.controller;
  assert([name.textFields.firstObject.text isEqual:@"Model.blend"]);
  assert(name.actions.count == 2 && [name.actions.lastObject.title isEqual:@"Choose Location"]);
  name.textFields.firstObject.text = @"../bad";
  [picker filenameChanged:name.textFields.firstObject];
  assert(!name.actions.lastObject.enabled);
  name.textFields.firstObject.text = @"New";
  [picker filenameChanged:name.textFields.firstObject];
  assert(name.actions.lastObject.enabled);
  assert(calls == 3);  // Merely opening Save must not write or commit anything.
  picker.filename = @"New.blend";
  [picker showPicker];
  controller = (id)picker.controller;
  assert([controller isKindOfClass:UIDocumentPickerViewController.class]);
  assert(!controller.allowsMultipleSelection);
  DismissablePicker *dismissable = [[[DismissablePicker alloc]
      initForOpeningContentTypes:@[ UTTypeFolder ]
                          asCopy:NO] autorelease];
  [picker documentPicker:dismissable didPickDocumentsAtURLs:@[ [NSURL fileURLWithPath:root] ]];
  waitUntil(
      ^BOOL {
        return calls == 4;
      },
      ^{
        assert(calls == 4 && picked.size() == 1);
        assert(picked[0] == [root stringByAppendingPathComponent:@"New.blend"].UTF8String);
        assert(![NSFileManager.defaultManager
            fileExistsAtPath:[root stringByAppendingPathComponent:@"New.blend"]]);

        PickerHarness *replacement = makePicker(GHOST_IOSDocumentMode::Save);
        replacement.filename = @"First.blend";
        [replacement documentPicker:dismissable
             didPickDocumentsAtURLs:@[ [NSURL fileURLWithPath:root] ]];
        waitUntil(
            ^BOOL {
              return replacement.controller != nil;
            },
            ^{
              UIAlertController *replace = (id)replacement.controller;
              assert([replace isKindOfClass:UIAlertController.class]);
              assert([replace.actions.lastObject.title isEqual:@"Replace"] && calls == 4);
              [replacement finish:nil error:nil];
              assert(calls == 5 && picked.empty());
              assert([[NSString stringWithContentsOfURL:first
                                               encoding:NSUTF8StringEncoding
                                                  error:nil]
                  isEqual:@"fixture"]);  // Cancelled replacement leaves the original intact.

              PickerHarness *directory = makePicker(GHOST_IOSDocumentMode::Directory);
              [directory start];
              assert(![(UIDocumentPickerViewController *)
                           directory.controller allowsMultipleSelection]);
              [directory documentPicker:nil
                  didPickDocumentsAtURLs:@[ [NSURL fileURLWithPath:root] ]];
              assert(calls == 6 && picked[0] == root.UTF8String);

              char error[1024] = {};
              int executions = 0;
              assert(GHOST_ios_document_can_write_path(first.path.UTF8String));
              assert(GHOST_ios_document_coordinate(
                  first.path.UTF8String,
                  false,
                  [](void *context, const char *path) {
                    ++*static_cast<int *>(context);
                    assert([[NSString stringWithContentsOfFile:[NSString stringWithUTF8String:path]
                                                      encoding:NSUTF8StringEncoding
                                                         error:nil] isEqual:@"fixture"]);
                  },
                  &executions,
                  error,
                  sizeof(error)));
              assert(executions == 1);
              assert(GHOST_ios_document_coordinate(
                  first.path.UTF8String,
                  true,
                  [](void *context, const char *path) {
                    ++*static_cast<int *>(context);
                    assert([@"saved" writeToFile:[NSString stringWithUTF8String:path]
                                      atomically:YES
                                        encoding:NSUTF8StringEncoding
                                           error:nil]);
                  },
                  &executions,
                  error,
                  sizeof(error)));
              assert(executions == 2);
              [NSFileManager.defaultManager removeItemAtPath:root error:nil];
              finished();
            });
      });
}

@interface FilesTestDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation FilesTestDelegate
@end
@interface FilesTestScene : UIResponder <UIWindowSceneDelegate>
@end
@implementation FilesTestScene
- (void)scene:(UIScene *)scene
    willConnectToSession:(UISceneSession *)session
                 options:(UISceneConnectionOptions *)options
{
  testWindow = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
  testWindow.rootViewController = [[[UIViewController alloc] init] autorelease];
  testWindow.rootViewController.view.backgroundColor = UIColor.systemBackgroundColor;
  [testWindow makeKeyAndVisible];
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_main_queue(), ^{
    checkRouting(^{
      GHOST_WindowIOS window;
      window.rootWindow = testWindow;
      assert(GHOST_ios_document_picker_begin(&window,
                                             100,
                                             GHOST_IOSDocumentMode::Open,
                                             "Open Blender File",
                                             "",
                                             "public.data",
                                             "*.blend",
                                             false,
                                             complete));
      dispatch_after(
          dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
            assert([testWindow.rootViewController.presentedViewController
                isKindOfClass:UIDocumentPickerViewController.class]);
            UIGraphicsImageRenderer *renderer = [[[UIGraphicsImageRenderer alloc]
                initWithSize:testWindow.bounds.size] autorelease];
            UIImage *image = [renderer
                imageWithActions:^(UIGraphicsImageRendererContext *context) {
                  [testWindow drawViewHierarchyInRect:testWindow.bounds afterScreenUpdates:YES];
                }];
            NSString *documents = NSSearchPathForDirectoriesInDomains(
                                      NSDocumentDirectory, NSUserDomainMask, YES)
                                      .firstObject;
            [UIImagePNGRepresentation(image)
                writeToFile:[documents stringByAppendingPathComponent:@"native-files-picker.png"]
                 atomically:YES];
            GHOST_ios_document_picker_cancel(100);
            assert(calls == 6 && document_pickers.count == 0);
            GHOST_ios_document_access_shutdown();
            fprintf(stderr,
                    "PASS: native document picker, save destination, overwrite cancellation, "
                    "coordination\n");
            exit(0);
          });
    });
  });
}
@end
int main(int argc, char **argv)
{
  @autoreleasepool {
    return UIApplicationMain(argc, argv, nil, NSStringFromClass(FilesTestDelegate.class));
  }
}
