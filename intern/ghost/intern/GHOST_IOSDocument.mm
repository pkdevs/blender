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
bool WM_apple_ios_document_open(bContext *C, const char *filepath, bool template_document);
bool WM_apple_ios_document_write(bContext *C, const char *filepath, bool preview);
std::string WM_apple_ios_document_template_write(bContext *C, const char *filepath, const char *preview,
                                               std::string &attribution);
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
  const bool template_document = self.templateDocument;
  if (engine_wait([path, template_document](blender::bContext *C) {
        return blender::WM_apple_ios_document_open(C, path.c_str(), template_document);
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

static NSURL *personalTemplatesURL()
{
  NSURL *support = [NSFileManager.defaultManager URLsForDirectory:NSApplicationSupportDirectory
                                                       inDomains:NSUserDomainMask].firstObject;
  return [support URLByAppendingPathComponent:@"BlenderTemplates" isDirectory:YES];
}

static BOOL templateVersionSupported(id minimum)
{
  if (![minimum isKindOfClass:NSArray.class] || [minimum count] != 3) { return NO; }
  for (id component in minimum) {
    if (![component isKindOfClass:NSNumber.class] || [component integerValue] < 0) { return NO; }
  }
  NSArray *current = [[NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"]
      componentsSeparatedByString:@"."];
  for (NSUInteger i = 0; i < 3; i++) {
    const NSInteger available = i < current.count ? [current[i] integerValue] : 0;
    const NSInteger required = [minimum[i] integerValue];
    if (required != available) { return required < available; }
  }
  return YES;
}

NSArray<NSDictionary *> *GHOST_ios_document_templates()
{
  NSURL *assets = [NSBundle.mainBundle.resourceURL URLByAppendingPathComponent:@"Assets"];
  NSArray *versions = [[NSFileManager.defaultManager contentsOfDirectoryAtPath:assets.path error:nil]
      sortedArrayUsingSelector:@selector(localizedStandardCompare:)];
  NSMutableArray *folders = [NSMutableArray array];
  for (NSString *version in versions.reverseObjectEnumerator) {
    [folders addObject:[assets URLByAppendingPathComponent:
        [version stringByAppendingPathComponent:@"datafiles/templates"]]];
  }
  NSURL *personal = personalTemplatesURL();
  for (NSString *name in [[NSFileManager.defaultManager contentsOfDirectoryAtPath:personal.path error:nil]
                            sortedArrayUsingSelector:@selector(localizedStandardCompare:)]) {
    [folders addObject:[personal URLByAppendingPathComponent:name]];
  }
  NSMutableArray *templates = [NSMutableArray array];
  NSMutableSet *identifiers = [NSMutableSet set];
  for (NSURL *folder in folders) {
    NSData *data = [NSData dataWithContentsOfURL:[folder URLByAppendingPathComponent:@"manifest.json"]];
    NSDictionary *manifest = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    if (![manifest isKindOfClass:NSDictionary.class] ||
        ![manifest[@"version"] isKindOfClass:NSNumber.class] || [manifest[@"version"] integerValue] != 1 ||
        ![manifest[@"templates"] isKindOfClass:NSArray.class]) { continue; }
    for (NSDictionary *item in manifest[@"templates"]) {
      if (![item isKindOfClass:NSDictionary.class]) { continue; }
      NSString *identifier = item[@"id"], *file = item[@"file"], *preview = item[@"preview"];
      if (![identifier isKindOfClass:NSString.class] || !identifier.length ||
          [identifiers containsObject:identifier] || ![item[@"title"] isKindOfClass:NSString.class] ||
          ![item[@"title"] length] || !templateVersionSupported(item[@"minimumBlender"]) ||
          ![item[@"presentation"] isKindOfClass:NSDictionary.class] ||
          ![item[@"presentation"][@"timeline"] isKindOfClass:NSNumber.class] ||
          ![file isKindOfClass:NSString.class] || ![file isEqual:file.lastPathComponent] ||
          ![file.pathExtension isEqual:@"blend"] || ![preview isKindOfClass:NSString.class] ||
          ![preview isEqual:preview.lastPathComponent] || ![preview.pathExtension isEqual:@"png"]) { continue; }
      NSURL *seed = [folder URLByAppendingPathComponent:file];
      NSURL *image = [folder URLByAppendingPathComponent:preview];
      if (![seed checkResourceIsReachableAndReturnError:nil] ||
          ![image checkResourceIsReachableAndReturnError:nil]) { continue; }
      NSMutableDictionary *entry = [[item mutableCopy] autorelease];
      NSDictionary *credit = item[@"attribution"];
      BOOL validCredit = [credit isKindOfClass:NSDictionary.class];
      if (validCredit) {
        for (NSString *key in @[@"title", @"author", @"license", @"licenseURL", @"sourceURL", @"changes"]) {
          if (![credit[key] isKindOfClass:NSString.class] || ![credit[key] length]) {
            validCredit = NO;
            break;
          }
        }
      }
      if (!validCredit) { [entry removeObjectForKey:@"attribution"]; }
      entry[@"url"] = seed;
      entry[@"previewURL"] = image;
      entry[@"personal"] = @([folder.URLByDeletingLastPathComponent isEqual:personal]);
      [templates addObject:entry];
      [identifiers addObject:identifier];
    }
  }
  return templates;
}

static BOOL writeTemplateManifest(NSDictionary *entry, NSURL *folder)
{
  NSData *data = [NSJSONSerialization dataWithJSONObject:@{@"version": @1, @"templates": @[entry]}
                                               options:NSJSONWritingPrettyPrinted error:nil];
  return [data writeToURL:[folder URLByAppendingPathComponent:@"manifest.json"]
                 options:NSDataWritingAtomic error:nil];
}

void GHOST_ios_template_save(NSString *title, NSDictionary *presentation, void (^completion)(NSString *error))
{
  NSString *identifier = [@"user." stringByAppendingString:NSUUID.UUID.UUIDString];
  NSURL *folder = [[personalTemplatesURL() URLByAppendingPathComponent:identifier] retain];
  NSDictionary *entry = [@{@"id": identifier, @"title": title, @"file": @"scene.blend",
      @"preview": @"preview.png", @"minimumBlender": @[@5, @1, @0],
      @"presentation": @{@"timeline": @([presentation[@"timeline"] boolValue])}} retain];
  void (^done)(NSString *) = [completion copy];
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    struct Result { std::mutex mutex; std::string error = "Could not save the template."; std::string attribution; };
    auto result = std::make_shared<Result>();
    const std::string path = [[folder URLByAppendingPathComponent:@"scene.blend"] path].UTF8String;
    const std::string preview = [[folder URLByAppendingPathComponent:@"preview.png"] path].UTF8String;
    BOOL success = [NSFileManager.defaultManager createDirectoryAtURL:folder
        withIntermediateDirectories:YES attributes:nil error:nil];
    if (success) {
      success = engine_wait([path, preview, result](blender::bContext *C) {
        std::string credit;
        std::string error = blender::WM_apple_ios_document_template_write(C, path.c_str(), preview.c_str(), credit);
        std::lock_guard lock(result->mutex);
        result->error = error;
        result->attribution = credit;
        return error.empty();
      });
    }
    if (success) {
      NSMutableDictionary *metadata = [[entry mutableCopy] autorelease];
      std::string credit;
      { std::lock_guard lock(result->mutex); credit = result->attribution; }
      NSData *data = [NSData dataWithBytes:credit.data() length:credit.size()];
      NSDictionary *attribution = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
      if ([attribution isKindOfClass:NSDictionary.class] && attribution.count) {
        metadata[@"attribution"] = attribution;
      }
      success = writeTemplateManifest(metadata, folder);
    }
    std::string error;
    { std::lock_guard lock(result->mutex); error = result->error; }
    NSString *message = success ? nil : error.empty() ? @"Could not save the template." :
        [NSString stringWithUTF8String:error.c_str()];
    if (!success) { [NSFileManager.defaultManager removeItemAtURL:folder error:nil]; }
    dispatch_async(dispatch_get_main_queue(), ^{
      done(message);
      [done release];
      [entry release];
      [folder release];
    });
  });
}

BOOL GHOST_ios_template_manage(NSString *identifier, NSString *action, NSString *title)
{
  for (NSDictionary *entry in GHOST_ios_document_templates()) {
    if (![entry[@"id"] isEqual:identifier]) { continue; }
    NSURL *folder = [entry[@"url"] URLByDeletingLastPathComponent];
    if ([action isEqual:@"delete"]) {
      return [entry[@"personal"] boolValue] && [NSFileManager.defaultManager removeItemAtURL:folder error:nil];
    }
    NSMutableDictionary *metadata = [[entry mutableCopy] autorelease];
    [metadata removeObjectsForKeys:@[@"url", @"previewURL", @"personal"]];
    if ([action isEqual:@"rename"]) {
      if (![entry[@"personal"] boolValue] || !title.length) { return NO; }
      metadata[@"title"] = title;
      return writeTemplateManifest(metadata, folder);
    }
    if ([action isEqual:@"duplicate"]) {
      NSString *newID = [@"user." stringByAppendingString:NSUUID.UUID.UUIDString];
      NSURL *destination = [personalTemplatesURL() URLByAppendingPathComponent:newID];
      NSFileManager *files = NSFileManager.defaultManager;
      metadata[@"id"] = newID;
      metadata[@"title"] = [entry[@"title"] stringByAppendingString:@" Copy"];
      BOOL success = [files createDirectoryAtURL:destination withIntermediateDirectories:YES attributes:nil error:nil] &&
          [files copyItemAtURL:entry[@"url"] toURL:[destination URLByAppendingPathComponent:entry[@"file"]] error:nil] &&
          [files copyItemAtURL:entry[@"previewURL"] toURL:[destination URLByAppendingPathComponent:entry[@"preview"]] error:nil] &&
          writeTemplateManifest(metadata, destination);
      if (!success) { [files removeItemAtURL:destination error:nil]; }
      return success;
    }
  }
  return NO;
}

void GHOST_ios_document_create(NSString *identifier, void (^completion)(NSURL *url))
{
  NSDictionary *selected = nil;
  for (NSDictionary *item in GHOST_ios_document_templates()) {
    if ([item[@"id"] isEqual:identifier.length ? identifier : @"general"]) { selected = item; break; }
  }
  if (!selected) { completion(nil); return; }
  NSURL *seed = [selected[@"url"] retain];
  NSString *folder = [[NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString] copy];
  NSURL *url = [[NSURL fileURLWithPath:[folder stringByAppendingPathComponent:@"Untitled.blend"]] retain];
  void (^done)(NSURL *) = [[completion copy] autorelease];
  [done retain];
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    NSFileManager *files = NSFileManager.defaultManager;
    const BOOL success = [files createDirectoryAtPath:folder withIntermediateDirectories:YES attributes:nil error:nil] &&
                         [files copyItemAtURL:seed toURL:url error:nil];
    if (!success) { [files removeItemAtPath:folder error:nil]; }
    dispatch_async(dispatch_get_main_queue(), ^{
      done(success ? url : nil);
      [seed release];
      [folder release];
      [url release];
      [done release];
    });
  });
}
