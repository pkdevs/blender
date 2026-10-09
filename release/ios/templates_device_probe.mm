/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* Test-only adapter for exercising the real document lifecycle on an iPad. */
#import <UIKit/UIKit.h>
#include <dlfcn.h>
#include <cstring>

@interface NSObject (TemplatesDeviceTest)
- (void)newDocumentFrom:(UIViewController *)presenter;
- (void)openDocumentAtURL:(NSURL *)url;
- (void)recordCreatedTemplateAtURL:(NSURL *)url;
- (NSDictionary *)toolbarModel;
@end

static NSString *operationResult;

extern "C" char *blender_device_probe_templates(const char *json)
{
  NSDictionary *request = [NSJSONSerialization JSONObjectWithData:
      [[NSString stringWithUTF8String:json] dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil];
  __block char *result;
  dispatch_sync(dispatch_get_main_queue(), ^{
    auto catalog = (NSArray *(*)())dlsym(RTLD_DEFAULT, "_Z28GHOST_ios_document_templatesv");
    auto launcher = (UIDocumentViewController *(*)())dlsym(RTLD_DEFAULT, "_Z29GHOST_ios_launcher_controllerv");
    auto create = (void (*)(NSString *, void (^)(NSURL *)))dlsym(
        RTLD_DEFAULT, "_Z25GHOST_ios_document_createP8NSStringU13block_pointerFvP5NSURLE");
    auto save = (void (*)(NSString *, NSDictionary *, void (^)(NSString *)))dlsym(
        RTLD_DEFAULT, "_Z23GHOST_ios_template_saveP8NSStringP12NSDictionaryU13block_pointerFvS0_E");
    NSCAssert(catalog && launcher && create && save, @"Production document entry points are available");
    UIDocumentViewController *documents = launcher();
    UIViewController *viewport = nil;
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
      if (![scene isKindOfClass:UIWindowScene.class]) { continue; }
      for (UIWindow *window in ((UIWindowScene *)scene).windows) {
        UIViewController *candidate = window.rootViewController;
        if ([candidate isKindOfClass:UINavigationController.class]) {
          candidate = ((UINavigationController *)candidate).topViewController;
        }
        if ([candidate respondsToSelector:NSSelectorFromString(@"setToolbarExpanded:animated:")]) {
          viewport = candidate;
        }
      }
    }
    NSString *action = request[@"action"];
    if ([action isEqual:@"bootstrap"]) {
      operationResult = @"pending";
      create(@"general", ^(NSURL *source) {
        NSURL *directory = [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory
            inDomains:NSUserDomainMask].firstObject;
        NSURL *destination = [directory URLByAppendingPathComponent:
            [NSString stringWithFormat:@"Template Test %@.blend", NSUUID.UUID.UUIDString]];
        BOOL copied = source && [NSFileManager.defaultManager moveItemAtURL:source toURL:destination error:nil];
        if (source) { [NSFileManager.defaultManager removeItemAtURL:source.URLByDeletingLastPathComponent error:nil]; }
        if (copied) {
          [documents setValue:@"general" forKey:@"_creatingTemplate"];
          [documents recordCreatedTemplateAtURL:destination];
          [documents openDocumentAtURL:destination];
        }
        operationResult = copied ? @"ok" : @"copy failed";
      });
    }
    if ([action isEqual:@"new"]) { [documents newDocumentFrom:viewport]; }
    UIViewController *presented = viewport.presentedViewController;
    UICollectionViewController *picker = [presented isKindOfClass:UINavigationController.class] ?
        (id)((UINavigationController *)presented).topViewController : nil;
    const BOOL choosing = [picker isKindOfClass:NSClassFromString(@"GHOSTIOSTemplatePicker")];
    if (choosing && [action isEqual:@"choose"]) {
      NSArray *sections = [picker valueForKey:@"_sections"];
      for (NSUInteger s = 0; s < sections.count; s++) {
        for (NSUInteger i = 0; i < [sections[s] count]; i++) {
          if ([sections[s][i][@"id"] isEqual:request[@"id"]]) {
            NSIndexPath *index = [NSIndexPath indexPathForItem:i inSection:s];
            [picker.collectionView selectItemAtIndexPath:index animated:NO
                scrollPosition:UICollectionViewScrollPositionCenteredVertically];
            [picker.collectionView.delegate collectionView:picker.collectionView didSelectItemAtIndexPath:index];
          }
        }
      }
    }
    if ([action isEqual:@"save"]) {
      operationResult = @"pending";
      save(@"Device Test Template", @{@"timeline": @NO}, ^(NSString *error) { operationResult = error ?: @"ok"; });
    }
    NSMutableArray *entries = [NSMutableArray array];
    for (NSDictionary *entry in catalog()) {
      [entries addObject:@{@"id": entry[@"id"], @"title": entry[@"title"],
          @"path": [entry[@"url"] path], @"personal": entry[@"personal"]}];
    }
    id timeline = [viewport valueForKey:@"_timeline"];
    NSDictionary *state = @{@"entries": entries, @"choosing": @(choosing),
        @"toolbar": [viewport toolbarModel] ?: @{},
        @"document": documents.document.fileURL.path ?: @"",
        @"opening": [documents valueForKey:@"_opening"] ?: @NO,
        @"creating": [documents valueForKey:@"_creating"] ?: @NO,
        @"timeline": [timeline valueForKey:@"visible"] ?: @NO,
        @"result": operationResult ?: @""};
    result = strdup([[NSString alloc] initWithData:[NSJSONSerialization dataWithJSONObject:state options:0 error:nil]
        encoding:NSUTF8StringEncoding].UTF8String);
  });
  return result;
}
