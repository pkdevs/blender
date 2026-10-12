/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#import "wm_apple_ios_file_drop.hh"
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

/** Feel that something could not be done; visionOS has no haptics. */
static void notify_error()
{
#if !defined(APPLE_TARGET_VISIONOS)
  [[[[UINotificationFeedbackGenerator alloc] init] autorelease]
      notificationOccurred:UINotificationFeedbackTypeError];
#endif
}

/* Image formats Blender reads directly. Others, such as HEIC from Photos, are converted to PNG. */
static NSArray<UTType *> *blenderReadableImageTypes()
{
  static NSArray<UTType *> *types = nil;
  if (!types) {
    NSMutableArray *list = [NSMutableArray arrayWithArray:@[
      UTTypePNG,
      UTTypeJPEG,
      UTTypeTIFF,
      UTTypeBMP,
      UTTypeWebP,
    ]];
    for (NSString *identifier in @[
           @"com.ilm.openexr-image",
           @"public.radiance",
           @"com.truevision.tga-image",
           @"public.jpeg-2000"
         ])
    {
      if (UTType *type = [UTType typeWithIdentifier:identifier]) {
        [list addObject:type];
      }
    }
    types = [list copy];
  }
  return types;
}

/** A new folder per drop, so files of the same name never replace each other and a dropped
 * file's path stays valid for as long as the .blend refers to it. */
static NSURL *dropFolder()
{
  NSFileManager *files = NSFileManager.defaultManager;
  NSURL *support = [files URLForDirectory:NSApplicationSupportDirectory
                                 inDomain:NSUserDomainMask
                        appropriateForURL:nil
                                   create:YES
                                    error:nil];
  if (!support) {
    return nil;
  }
  NSDateFormatter *format = [[[NSDateFormatter alloc] init] autorelease];
  format.dateFormat = @"yyyy-MM-dd HH.mm.ss";
  NSString *name = [NSString stringWithFormat:@"%@ %@",
                                              [format stringFromDate:NSDate.date],
                                              [NSUUID.UUID.UUIDString substringToIndex:8]];
  NSURL *folder = [[support URLByAppendingPathComponent:@"Dropped Files"
                                            isDirectory:YES] URLByAppendingPathComponent:name
                                                                             isDirectory:YES];
  if (![files createDirectoryAtURL:folder
          withIntermediateDirectories:YES
                           attributes:nil
                                error:nil])
  {
    return nil;
  }
  return folder;
}

/** Name a copied file after the original, adding the type's extension when it has none, as
 * Blender's drop boxes and importers recognize files by extension. */
static NSString *fileName(NSString *name, UTType *type, NSUInteger index)
{
  name = name.lastPathComponent;
  if (!name.length || [name hasPrefix:@"."]) {
    name = [NSString stringWithFormat:@"Dropped %lu", (unsigned long)index + 1];
  }
  if (!name.pathExtension.length && type.preferredFilenameExtension) {
    name = [name stringByAppendingPathExtension:type.preferredFilenameExtension];
  }
  return name;
}

@implementation WMIOSFileDrop
{
  UIDropInteraction *_interaction;
}

- (instancetype)init
{
  if ((self = [super init])) {
    _interaction = [[UIDropInteraction alloc] initWithDelegate:self];
  }
  return self;
}

- (void)dealloc
{
  [_interaction release];
  [_dropFiles release];
  [super dealloc];
}

- (UIDropInteraction *)interaction
{
  return _interaction;
}

- (BOOL)dropInteraction:(UIDropInteraction *)interaction
       canHandleSession:(id<UIDropSession>)session
{
  /* Drags that start in Blender (assets) are not files from another app. */
  return session.localDragSession == nil && [session hasItemsConformingToTypeIdentifiers:@[
           UTTypeData.identifier,
           UTTypeImage.identifier
         ]];
}

- (UIDropProposal *)dropInteraction:(UIDropInteraction *)interaction
                   sessionDidUpdate:(id<UIDropSession>)session
{
  return [[[UIDropProposal alloc] initWithDropOperation:UIDropOperationCopy] autorelease];
}

- (void)dropInteraction:(UIDropInteraction *)interaction performDrop:(id<UIDropSession>)session
{
  NSURL *folder = dropFolder();
  if (!folder) {
    return;
  }
  const CGPoint point = [session locationInView:interaction.view];
  NSArray<UIDragItem *> *items = session.items;
  NSMutableArray *results = [NSMutableArray arrayWithCapacity:items.count];
  for (NSUInteger i = 0; i < items.count; i++) {
    [results addObject:NSNull.null];
  }
  dispatch_group_t group = dispatch_group_create();
  for (NSUInteger index = 0; index < items.count; index++) {
    [self copyItem:items[index].itemProvider
             index:index
              into:folder
             group:group
          finished:^(NSString *path) {
            if (path) {
              @synchronized(results) {
                results[index] = path;
              }
            }
          }];
  }
  void (^drop_files)(NSArray<NSString *> *, CGPoint) = [[_dropFiles copy] autorelease];
  dispatch_group_notify(group, dispatch_get_main_queue(), ^{
    NSMutableArray<NSString *> *paths = [NSMutableArray array];
    for (id path in results) {
      if (path != NSNull.null) {
        [paths addObject:path];
      }
    }
    if (paths.count && drop_files) {
      drop_files(paths, point);
    }
    else {
      notify_error();
    }
  });
  dispatch_release(group);
}

/** Copy one dragged item into `folder`, then call `finished` with its path, or nil when the item
 * offers nothing Blender could open. */
- (void)copyItem:(NSItemProvider *)provider
           index:(NSUInteger)index
            into:(NSURL *)folder
           group:(dispatch_group_t)group
        finished:(void (^)(NSString *path))finished
{
  finished = [[finished copy] autorelease];
  NSString *suggested = provider.suggestedName;

  /* Images: take a format Blender reads if the item offers one, otherwise convert to PNG. */
  if ([provider hasItemConformingToTypeIdentifier:UTTypeImage.identifier]) {
    for (UTType *readable in blenderReadableImageTypes()) {
      for (NSString *identifier in provider.registeredTypeIdentifiers) {
        UTType *type = [UTType typeWithIdentifier:identifier];
        if (type && [type conformsToType:readable]) {
          [self copyFile:provider
                    type:type
               suggested:suggested
                   index:index
                    into:folder
                   group:group
                finished:finished];
          return;
        }
      }
    }
    if ([provider canLoadObjectOfClass:UIImage.class]) {
      dispatch_group_enter(group);
      [provider
          loadObjectOfClass:UIImage.class
          completionHandler:^(id<NSItemProviderReading> object, NSError *) {
            NSData *png = [object isKindOfClass:UIImage.class] ?
                              UIImagePNGRepresentation((UIImage *)object) :
                              nil;
            NSString *name = fileName(suggested.stringByDeletingPathExtension, UTTypePNG, index);
            NSURL *target = [folder URLByAppendingPathComponent:name];
            finished(png && [png writeToURL:target atomically:YES] ? target.path : nil);
            dispatch_group_leave(group);
          }];
      return;
    }
  }

  /* Any other file (models, .blend, fonts, ...): copy it as it is. */
  for (NSString *identifier in provider.registeredTypeIdentifiers) {
    UTType *type = [UTType typeWithIdentifier:identifier];
    if (type && [type conformsToType:UTTypeData] && ![type conformsToType:UTTypeURL]) {
      [self copyFile:provider
                type:type
           suggested:suggested
               index:index
                into:folder
               group:group
            finished:finished];
      return;
    }
  }
  finished(nil);
}

- (void)copyFile:(NSItemProvider *)provider
            type:(UTType *)type
       suggested:(NSString *)suggested
           index:(NSUInteger)index
            into:(NSURL *)folder
           group:(dispatch_group_t)group
        finished:(void (^)(NSString *path))finished
{
  dispatch_group_enter(group);
  [provider loadFileRepresentationForTypeIdentifier:type.identifier
                                  completionHandler:^(NSURL *url, NSError *) {
                                    /* The file is deleted when this returns, so copy it now. */
                                    NSString *path = nil;
                                    if (url) {
                                      NSString *name = fileName(url.lastPathComponent.length ?
                                                                    url.lastPathComponent :
                                                                    suggested,
                                                                type,
                                                                index);
                                      NSURL *target = [folder URLByAppendingPathComponent:name];
                                      if ([NSFileManager.defaultManager copyItemAtURL:url
                                                                                toURL:target
                                                                                error:nil]) {
                                        path = target.path;
                                      }
                                    }
                                    finished(path);
                                    dispatch_group_leave(group);
                                  }];
}

@end
