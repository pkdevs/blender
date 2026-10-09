/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#import "GHOST_IOSDocument.hh"

#include "GHOST_IOSRuntime.hh"
#import "GHOST_IOSLauncher.hh"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>

namespace blender {
/* Implemented in wm_files.cc. */
bool WM_apple_ios_document_open(bContext *C, const char *filepath);
bool WM_apple_ios_document_write(bContext *C, const char *filepath, bool preview);
bool WM_apple_ios_document_new(bContext *C, const char *app_template, const char *filepath);
}  // namespace blender

/**
 * Run `work` on the engine and wait for it. Documents read and write on UIKit's background queues,
 * inside file coordination, and only finish once Blender has.
 */
static bool engine_wait(std::function<bool(blender::bContext *)> work)
{
  struct Wait {
    std::mutex mutex;
    std::condition_variable condition;
    bool done = false, success = false;
  };
  auto wait = std::make_shared<Wait>();
  GHOST_ios_engine_perform([work = std::move(work), wait](blender::bContext *C) {
    const bool success = C && work(C);
    {
      std::lock_guard lock(wait->mutex);
      wait->done = true;
      wait->success = success;
    }
    wait->condition.notify_all();
  });
  std::unique_lock lock(wait->mutex);
  /* Large scenes take a while; never hold the document's queue forever. */
  if (!wait->condition.wait_for(lock, std::chrono::minutes(5), [&] { return wait->done; })) {
    return false;
  }
  return wait->success;
}

@implementation GHOSTIOSBlendDocument {
  BOOL _blenderModified;
  BOOL _accessing;
  /** Saving was asked for, rather than UIKit's periodic saving while the document is edited. */
  int _saving;
}

- (void)saveChangesWithCompletionHandler:(void (^)(BOOL success))completionHandler
{
  if (!self.hasUnsavedChanges) {
    if (completionHandler) {
      completionHandler(YES);
    }
    return;
  }
  _saving++;
  [super autosaveWithCompletionHandler:^(BOOL success) {
    _saving--;
    if (completionHandler) {
      completionHandler(success);
    }
  }];
}

- (void)autosaveWithCompletionHandler:(void (^)(BOOL success))completionHandler
{
  /* UIKit saves edited documents every so often. Blender would stop to write the file and render
   * its preview, mid-edit: leave the work for when the document closes or the app leaves the
   * foreground. Blender's own auto-save still keeps the work safe meanwhile. */
  if (_saving == 0 && !(self.documentState & UIDocumentStateClosed)) {
    if (completionHandler) {
      completionHandler(YES);
    }
    return;
  }
  [super autosaveWithCompletionHandler:completionHandler];
}

- (void)openWithCompletionHandler:(void (^)(BOOL success))completionHandler
{
  /* Blender opens the file by its path, outside of the document's coordinated read. */
  _accessing = [self.fileURL startAccessingSecurityScopedResource];
  [super openWithCompletionHandler:completionHandler];
}

- (void)closeWithCompletionHandler:(void (^)(BOOL success))completionHandler
{
  /* Closing saves, through autosave. */
  _saving++;
  NSURL *url = self.fileURL;
  const BOOL accessing = _accessing;
  _accessing = NO;
  [super closeWithCompletionHandler:^(BOOL success) {
    _saving--;
    if (accessing) {
      [url stopAccessingSecurityScopedResource];
    }
    if (completionHandler) {
      completionHandler(success);
    }
  }];
}

- (BOOL)readFromURL:(NSURL *)url error:(NSError **)outError
{
  /* Blender loads the file now, so the document opens on its scene. */
  const std::string path = url.path.UTF8String ?: "";
  if (engine_wait([path](blender::bContext *C) {
        return blender::WM_apple_ios_document_open(C, path.c_str());
      }))
  {
    return YES;
  }
  if (outError) {
    *outError = [NSError errorWithDomain:NSCocoaErrorDomain code:NSFileReadCorruptFileError
                                userInfo:nil];
  }
  return NO;
}

- (id)contentsForType:(NSString *)typeName error:(NSError **)outError
{
  /* Blender writes the file itself, see below. */
  return [NSData data];
}

- (BOOL)writeContents:(id)contents
                   toURL:(NSURL *)url
        forSaveOperation:(UIDocumentSaveOperation)saveOperation
     originalContentsURL:(NSURL *)originalContentsURL
                   error:(NSError **)outError
{
  /* Blender writes the file where UIKit asks, and UIKit moves it into place, so saving needs
   * access to nothing but the document. */
  const std::string path = url.path.UTF8String ?: "";
  /* Rendering the preview needs Blender's window on screen, and the app in front. */
  const bool preview = !GHOST_ios_app_in_background() && GHOST_ios_document_window_visible();
  if (engine_wait([path, preview](blender::bContext *C) {
        return blender::WM_apple_ios_document_write(C, path.c_str(), preview);
      }))
  {
    return YES;
  }
  if (outError) {
    *outError = [NSError errorWithDomain:NSCocoaErrorDomain code:NSFileWriteUnknownError
                                userInfo:nil];
  }
  return NO;
}

- (BOOL)hasUnsavedChanges
{
  return _blenderModified || [super hasUnsavedChanges];
}

- (void)setBlenderModified:(BOOL)modified
{
  if (modified && !_blenderModified) {
    /* Schedules saving, as any edit of a document does. */
    [self updateChangeCount:UIDocumentChangeDone];
  }
  _blenderModified = modified;
}

@end

void GHOST_ios_document_create(NSString *app_template, void (^completion)(NSURL *url))
{
  NSString *folder = [NSTemporaryDirectory()
      stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
  [NSFileManager.defaultManager createDirectoryAtPath:folder withIntermediateDirectories:YES
                                           attributes:nil error:nil];
  NSURL *url = [NSURL fileURLWithPath:[folder stringByAppendingPathComponent:@"Untitled.blend"]];
  const std::string path = url.path.UTF8String;
  const std::string name = app_template.UTF8String ?: "";
  void (^done)(NSURL *) = [[completion copy] autorelease];
  [url retain];
  [done retain];
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    const bool success = engine_wait([path, name](blender::bContext *C) {
      return blender::WM_apple_ios_document_new(C, name.c_str(), path.c_str());
    });
    dispatch_async(dispatch_get_main_queue(), ^{
      done(success ? url : nil);
      [url release];
      [done release];
    });
  });
}
