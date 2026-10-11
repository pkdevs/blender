/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "GHOST_IOSDocumentPicker.hh"
#include "GHOST_IOSRuntime.hh"
#include "GHOST_WindowIOS.hh"

#include <cstdio>
#include <vector>

static NSMutableDictionary<NSString *, NSURL *> *document_access;
static NSMutableDictionary<NSNumber *, id> *document_pickers;
static NSString *const bookmark_key = @"BlenderDocumentBookmarks";
static char document_queue_key;

static dispatch_queue_t documentQueue()
{
  static dispatch_queue_t queue;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    queue = dispatch_queue_create("org.blender.document.access", DISPATCH_QUEUE_SERIAL);
    dispatch_queue_set_specific(queue, &document_queue_key, &document_queue_key, nullptr);
  });
  return queue;
}

static BOOL documentPathIsInside(NSString *path, NSString *directory)
{
  path = path.stringByResolvingSymlinksInPath;
  directory = directory.stringByResolvingSymlinksInPath;
  return [path isEqualToString:directory] ||
         [path hasPrefix:[directory stringByAppendingString:@"/"]];
}

extern "C" void GHOST_ios_document_access_restore()
{
  if (!dispatch_get_specific(&document_queue_key)) {
    dispatch_sync(documentQueue(), ^{
      GHOST_ios_document_access_restore();
    });
    return;
  }
  if (document_access) {
    return;
  }
  document_access = [[NSMutableDictionary alloc] init];
  NSDictionary *bookmarks = [NSUserDefaults.standardUserDefaults dictionaryForKey:bookmark_key];
  for (NSData *data in bookmarks.allValues) {
    if (![data isKindOfClass:NSData.class]) {
      continue;
    }
    BOOL stale = NO;
    NSURL *url = [NSURL URLByResolvingBookmarkData:data
                                           options:0
                                     relativeToURL:nil
                               bookmarkDataIsStale:&stale
                                             error:nil];
    if (url && [url startAccessingSecurityScopedResource]) {
      document_access[url.path] = url;
    }
  }
}

static void documentRememberAccess(NSArray<NSURL *> *urls)
{
  if (!dispatch_get_specific(&document_queue_key)) {
    NSArray *owned = [urls copy];
    dispatch_async(documentQueue(), ^{
      @autoreleasepool {
        documentRememberAccess(owned);
        [owned release];
      }
    });
    return;
  }
  GHOST_ios_document_access_restore();
  NSMutableDictionary *bookmarks = [[[NSUserDefaults.standardUserDefaults
      dictionaryForKey:bookmark_key] mutableCopy] autorelease];
  if (!bookmarks) {
    bookmarks = [NSMutableDictionary dictionary];
  }
  for (NSURL *url in urls) {
    NSURL *previous = document_access[url.path];
    if (previous) {
      [previous stopAccessingSecurityScopedResource];
    }
    document_access[url.path] = url;
    NSData *data = [url bookmarkDataWithOptions:NSURLBookmarkCreationMinimalBookmark
                 includingResourceValuesForKeys:nil
                                  relativeToURL:nil
                                          error:nil];
    if (data) {
      bookmarks[url.path] = data;
    }
  }
  [NSUserDefaults.standardUserDefaults setObject:bookmarks forKey:bookmark_key];
}

extern "C" bool GHOST_ios_document_can_write_path(const char *path)
{
  if (!dispatch_get_specific(&document_queue_key)) {
    __block bool result;
    dispatch_sync(documentQueue(), ^{
      result = GHOST_ios_document_can_write_path(path);
    });
    return result;
  }
  GHOST_ios_document_access_restore();
  NSString *value = [NSString stringWithUTF8String:path];
  if (documentPathIsInside(value, NSHomeDirectory())) {
    return true;
  }
  /* Blender saves through a sibling temporary file and may create .blend1 backups.
   * A grant for only the opened file is insufficient; ask for its folder once. */
  for (NSURL *url in document_access.allValues) {
    NSNumber *directory = nil;
    [url getResourceValue:&directory forKey:NSURLIsDirectoryKey error:nil];
    if (directory.boolValue && documentPathIsInside(value, url.path)) {
      return true;
    }
  }
  return false;
}

static NSArray<UTType *> *documentContentTypes(NSString *types, NSString *glob)
{
  NSMutableArray<UTType *> *result = [NSMutableArray array];
  for (NSString *pattern in [glob componentsSeparatedByString:@";"]) {
    NSString *extension = [pattern hasPrefix:@"*."] ? [pattern substringFromIndex:2] : @"";
    if (extension.length &&
        [extension
            rangeOfCharacterFromSet:[NSCharacterSet characterSetWithCharactersInString:@"*?[]/"]]
                .location == NSNotFound)
    {
      UTType *type = [UTType typeWithFilenameExtension:extension];
      if (type && ![result containsObject:type]) {
        [result addObject:type];
      }
    }
  }
  if (result.count == 0) {
    for (NSString *identifier in [types componentsSeparatedByString:@";"]) {
      UTType *type = identifier.length ? [UTType typeWithIdentifier:identifier] : nil;
      if (type && ![result containsObject:type]) {
        [result addObject:type];
      }
    }
  }
  return result.count ? result : @[ UTTypeData ];
}

static NSString *documentFilename(NSString *value, NSString *extension)
{
  value = [value stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
  if (!value.length || [value isEqualToString:@"."] || [value isEqualToString:@".."] ||
      [value rangeOfCharacterFromSet:[NSCharacterSet characterSetWithCharactersInString:@"/:"]]
              .location != NSNotFound ||
      [value rangeOfCharacterFromSet:NSCharacterSet.controlCharacterSet].location != NSNotFound)
  {
    return nil;
  }
  if (extension.length &&
      ![value.lowercaseString hasSuffix:[@"." stringByAppendingString:extension.lowercaseString]])
  {
    value = [value stringByAppendingPathExtension:extension];
  }
  return value;
}

@interface GHOSTIOSDocumentPicker
    : NSObject <UIDocumentPickerDelegate, UIAdaptivePresentationControllerDelegate>
@property(nonatomic) uint64_t request;
@property(nonatomic) GHOST_IOSDocumentMode mode;
@property(nonatomic) GHOST_IOSDocumentCompletion completion;
@property(nonatomic, retain) UIViewController *presenter;
@property(nonatomic, retain) UIViewController *controller;
@property(nonatomic, copy) NSString *title;
@property(nonatomic, copy) NSString *initialPath;
@property(nonatomic, copy) NSString *filename;
@property(nonatomic, copy) NSArray<UTType *> *types;
@property(nonatomic, retain) NSMutableArray<NSURL *> *scopedURLs;
@property(nonatomic) BOOL multiple;
@property(nonatomic) BOOL finished;
- (void)start;
- (void)finish:(NSArray<NSURL *> *)urls error:(NSString *)error;
@end

@implementation GHOSTIOSDocumentPicker
- (void)dealloc
{
  for (NSURL *url in _scopedURLs) {
    [url stopAccessingSecurityScopedResource];
  }
  [_scopedURLs release];
  [_presenter release];
  [_controller release];
  [_title release];
  [_initialPath release];
  [_filename release];
  [_types release];
  [super dealloc];
}

- (void)finish:(NSArray<NSURL *> *)urls error:(NSString *)error
{
  if (self.finished) {
    return;
  }
  [[self retain] autorelease];
  self.finished = YES;
  if (urls.count && !error) {
    documentRememberAccess(self.scopedURLs);
    [self.scopedURLs removeAllObjects]; /* Access is now owned by document_access. */
  }
  else {
    for (NSURL *url in self.scopedURLs) {
      [url stopAccessingSecurityScopedResource];
    }
    [self.scopedURLs removeAllObjects];
  }
  std::vector<const char *> paths;
  for (NSURL *url in urls) {
    paths.push_back(url.path.UTF8String);
  }
  if (self.completion) {
    self.completion(self.request, paths.data(), int(paths.size()), error.UTF8String);
  }
  self.controller = nil;
  [document_pickers removeObjectForKey:@(self.request)];
}

- (void)present:(UIViewController *)controller
{
  if (self.finished) {
    return;
  }
  self.controller = controller;
  controller.modalPresentationStyle = UIModalPresentationFormSheet;
  [self.presenter presentViewController:controller animated:YES completion:nil];
  controller.presentationController.delegate = self;
}

- (void)showPicker
{
  NSArray *types = self.mode == GHOST_IOSDocumentMode::Open ? self.types : @[ UTTypeFolder ];
  UIDocumentPickerViewController *picker = [[[UIDocumentPickerViewController alloc]
      initForOpeningContentTypes:types
                          asCopy:NO] autorelease];
  picker.delegate = self;
  picker.title = self.title;
  picker.allowsMultipleSelection = self.mode == GHOST_IOSDocumentMode::Open && self.multiple;
  picker.shouldShowFileExtensions = YES;
  NSString *directory = self.mode == GHOST_IOSDocumentMode::Directory ?
                            self.initialPath :
                            self.initialPath.stringByDeletingLastPathComponent;
  if (directory.length == 0) {
    directory = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES)
                    .firstObject;
  }
  picker.directoryURL = [NSURL fileURLWithPath:directory isDirectory:YES];
  [self present:picker];
}

- (void)filenameChanged:(UITextField *)field
{
  UIAlertController *alert = (UIAlertController *)self.controller;
  alert.actions.lastObject.enabled = documentFilename(field.text,
                                                      self.initialPath.pathExtension) != nil;
}

- (void)start
{
  if (self.mode != GHOST_IOSDocumentMode::Save) {
    [self showPicker];
    return;
  }
  UIAlertController *alert = [UIAlertController
      alertControllerWithTitle:self.title
                       message:nil
                preferredStyle:UIAlertControllerStyleAlert];
  NSString *initial = self.initialPath.lastPathComponent;
  if (!initial.length) {
    initial = @"Untitled";
  }
  [alert addTextFieldWithConfigurationHandler:^(UITextField *field) {
    field.text = initial;
    field.clearButtonMode = UITextFieldViewModeWhileEditing;
    field.autocorrectionType = UITextAutocorrectionTypeNo;
    field.accessibilityLabel = @"Filename";
    [field addTarget:self
                  action:@selector(filenameChanged:)
        forControlEvents:UIControlEventEditingChanged];
  }];
  [alert addAction:[UIAlertAction actionWithTitle:@"Cancel"
                                            style:UIAlertActionStyleCancel
                                          handler:^(UIAlertAction *) {
                                            [self finish:nil error:nil];
                                          }]];
  __block UIAlertController *nameAlert = alert;
  [alert addAction:[UIAlertAction actionWithTitle:@"Choose Location"
                                            style:UIAlertActionStyleDefault
                                          handler:^(UIAlertAction *) {
                                            self.filename = documentFilename(
                                                nameAlert.textFields.firstObject.text,
                                                initial.pathExtension);
                                            [nameAlert dismissViewControllerAnimated:YES
                                                                          completion:^{
                                                                            if (self.filename) {
                                                                              [self showPicker];
                                                                            }
                                                                            else {
                                                                              [self start];
                                                                            }
                                                                          }];
                                          }]];
  [self present:alert];
  [self filenameChanged:alert.textFields.firstObject];
}

- (void)documentPicker:(UIDocumentPickerViewController *)picker
    didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls
{
  if (self.finished || urls.count == 0) {
    [self finish:nil error:nil];
    return;
  }
  for (NSURL *url in urls) {
    if ([url startAccessingSecurityScopedResource]) {
      [self.scopedURLs addObject:url];
    }
  }
  if (self.mode == GHOST_IOSDocumentMode::Save) {
    NSURL *destination = [urls.firstObject URLByAppendingPathComponent:self.filename];
    [picker
        dismissViewControllerAnimated:YES
                           completion:^{
                             /* Transfer ownership without retaining UIKit objects in the file
                              * queue's block, so the final release always happens on main. */
                             void *picker_token = [self retain];
                             dispatch_async(documentQueue(), ^{
                               const BOOL exists = [NSFileManager.defaultManager
                                   fileExistsAtPath:destination.path];
                               dispatch_async(dispatch_get_main_queue(), ^{
                                 GHOSTIOSDocumentPicker *owner = (GHOSTIOSDocumentPicker *)
                                     picker_token;
                                 if (owner.finished) {
                                   [owner release];
                                   return;
                                 }
                                 if (exists) {
                                   UIAlertController *alert = [UIAlertController
                                       alertControllerWithTitle:@"Replace File?"
                                                        message:destination.lastPathComponent
                                                 preferredStyle:UIAlertControllerStyleAlert];
                                   [alert addAction:[UIAlertAction
                                                        actionWithTitle:@"Cancel"
                                                                  style:UIAlertActionStyleCancel
                                                                handler:^(UIAlertAction *) {
                                                                  [owner finish:nil error:nil];
                                                                }]];
                                   [alert
                                       addAction:[UIAlertAction
                                                     actionWithTitle:@"Replace"
                                                               style:UIAlertActionStyleDestructive
                                                             handler:^(UIAlertAction *) {
                                                               [owner finish:@[ destination ]
                                                                       error:nil];
                                                             }]];
                                   [owner present:alert];
                                 }
                                 else {
                                   [owner finish:@[ destination ] error:nil];
                                 }
                                 [owner release];
                               });
                             });
                           }];
    return;
  }
  NSString *parent = urls.firstObject.URLByDeletingLastPathComponent.path;
  for (NSURL *url in urls) {
    if (![url.URLByDeletingLastPathComponent.path isEqualToString:parent]) {
      [self finish:nil error:@"Select files from a single folder for this Blender operation."];
      return;
    }
  }
  [self finish:urls error:nil];
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)controller
{
  [self finish:nil error:nil];
}

- (void)presentationControllerDidDismiss:(UIPresentationController *)controller
{
  [self finish:nil error:nil];
}
@end

extern "C" bool GHOST_ios_document_picker_begin(void *window,
                                                uint64_t request,
                                                GHOST_IOSDocumentMode mode,
                                                const char *title,
                                                const char *path,
                                                const char *types,
                                                const char *glob,
                                                bool multiple,
                                                GHOST_IOSDocumentCompletion completion)
{
  if (!window) {
    return false;
  }
  UIWindow *native_window = static_cast<GHOST_WindowIOS *>(window)->rootWindow;
  auto alive = static_cast<GHOST_WindowIOS *>(window)->input_alive;
  NSString *owned_title = [NSString stringWithUTF8String:title];
  NSString *owned_path = [NSString stringWithUTF8String:path];
  NSArray *owned_types = documentContentTypes([NSString stringWithUTF8String:types],
                                              [NSString stringWithUTF8String:glob]);
  dispatch_async(dispatch_get_main_queue(), ^{
    UIViewController *presenter = alive->load() ? native_window.rootViewController : nil;
    if (!presenter) {
      completion(request, nullptr, 0, nullptr);
      return;
    }
    while (presenter.presentedViewController) {
      presenter = presenter.presentedViewController;
    }
    GHOSTIOSDocumentPicker *picker = [[[GHOSTIOSDocumentPicker alloc] init] autorelease];
    picker.request = request;
    picker.mode = mode;
    picker.completion = completion;
    picker.presenter = presenter;
    picker.title = owned_title;
    picker.initialPath = owned_path;
    picker.types = owned_types;
    picker.multiple = multiple;
    picker.scopedURLs = [NSMutableArray array];
    if (!document_pickers) {
      document_pickers = [[NSMutableDictionary alloc] init];
    }
    document_pickers[@(request)] = picker;
    [picker start];
  });
  return true;
}

extern "C" void GHOST_ios_document_picker_cancel(uint64_t request)
{
  if (!NSThread.isMainThread) {
    dispatch_async(dispatch_get_main_queue(), ^{
      GHOST_ios_document_picker_cancel(request);
    });
    return;
  }
  GHOSTIOSDocumentPicker *picker = document_pickers[@(request)];
  if (picker) {
    [[picker retain] autorelease];
    picker.completion = nullptr;
    [picker.controller dismissViewControllerAnimated:NO completion:nil];
    [picker finish:nil error:nil];
  }
}

extern "C" bool GHOST_ios_document_coordinate(const char *path,
                                              bool writing,
                                              void (*execute)(void *, const char *),
                                              void *context,
                                              char *error,
                                              size_t error_size)
{
  NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
  NSFileCoordinator *coordinator = [[[NSFileCoordinator alloc] initWithFilePresenter:nil]
      autorelease];
  __block BOOL called = NO;
  NSError *failure = nil;
  void (^access)(NSURL *) = ^(NSURL *coordinatedURL) {
    called = YES;
    execute(context, coordinatedURL.path.UTF8String);
  };
  if (writing) {
    [coordinator coordinateWritingItemAtURL:url
                                    options:NSFileCoordinatorWritingForReplacing
                                      error:&failure
                                 byAccessor:access];
  }
  else {
    [coordinator coordinateReadingItemAtURL:url options:0 error:&failure byAccessor:access];
  }
  if (!called) {
    snprintf(
        error, error_size, "%s", failure.localizedDescription.UTF8String ?: "File access failed");
  }
  return called;
}

extern "C" void GHOST_ios_document_access_shutdown()
{
  GHOST_ios_main_sync(^{
    for (NSNumber *request in [[document_pickers.allKeys copy] autorelease]) {
      GHOST_ios_document_picker_cancel(request.unsignedLongLongValue);
    }
    [document_pickers release];
    document_pickers = nil;
  });
  dispatch_sync(documentQueue(), ^{
    for (NSURL *url in document_access.allValues) {
      [url stopAccessingSecurityScopedResource];
    }
    [document_access release];
    document_access = nil;
  });
}
