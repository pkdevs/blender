/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup wm
 *
 * Shared by the native interface's files: Blender's icons, its SwiftUI classes by name, and
 * Blender's menus as UIKit menus.
 */

#import "wm_apple_ios_interface.hh"

/* Semantic keys in release/ios/blender_icons.json select Blender artwork without
 * conflating SF Symbols reused for different tools. UIImage caches the assets;
 * tool artwork retains its colors, while interface icons use UIKit template tinting. */
UIImage *blenderIOSIcon(NSString *key, NSString *systemImage)
{
  const BOOL selected_tool = [key hasPrefix:@"selected.tool."];
  if (selected_tool) {
    key = [key substringFromIndex:@"selected.".length];
  }
  static NSDictionary *icons = nil;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    NSURL *url = [NSBundle.mainBundle URLForResource:@"blender_icons" withExtension:@"json"];
    NSData *data = url ? [NSData dataWithContentsOfURL:url] : nil;
    id decoded = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    if ([decoded isKindOfClass:NSDictionary.class]) {
      icons = [decoded copy];
    }
  });
  NSString *name = key ? icons[key] : nil;
  if (name == nil && systemImage != nil) {
    name = icons[[@"symbol." stringByAppendingString:systemImage]];
  }
  NSString *prefix = [key hasPrefix:@"tool."] ? @"blender.tool." : @"blender.";
  if (name && [key hasPrefix:@"tool."]) {
    NSString *colored_prefix = selected_tool ? @"blender.tool.selected." : @"blender.tool.color.";
    UIImage *colored = [UIImage imageNamed:[colored_prefix stringByAppendingString:name]];
    if (colored) {
      return [colored imageWithRenderingMode:UIImageRenderingModeAlwaysOriginal];
    }
  }
  UIImage *image = nil;
  if (name && [key hasPrefix:@"tool."]) {
    image = [[UIImage imageNamed:[prefix stringByAppendingString:name]]
        imageWithRenderingMode:UIImageRenderingModeAlwaysTemplate];
  }
  else if (name) {
    image = WMIOSBlenderIcon(name, NO);
  }
  if (image != nil) {
    return image;
  }
  return systemImage ? [UIImage systemImageNamed:systemImage] : nil;
}

/** Brush previews are square renders; crop them to a circle like Blender's brush cursor. */
UIImage *blenderIOSBrushImage(NSString *previewPath, CGFloat diameter)
{
  static NSCache<NSString *, UIImage *> *cache = [[NSCache alloc] init];
  NSString *cache_key = [NSString stringWithFormat:@"%@@%.0f", previewPath ?: @"", diameter];
  if (UIImage *cached = [cache objectForKey:cache_key]) {
    return cached;
  }
  UIImage *preview = previewPath.length ? [UIImage imageWithContentsOfFile:previewPath] : nil;
  UIImage *symbol = preview ?
                        nil :
                        [UIImage systemImageNamed:@"paintbrush.pointed.fill"
                                withConfiguration:[UIImageSymbolConfiguration
                                                      configurationWithPointSize:diameter * 0.42]];
  const CGRect bounds = CGRectMake(0, 0, diameter, diameter);
  UIGraphicsImageRendererFormat *format = [UIGraphicsImageRendererFormat preferredFormat];
  UIGraphicsImageRenderer *renderer = [[[UIGraphicsImageRenderer alloc] initWithSize:bounds.size
                                                                              format:format]
      autorelease];
  UIImage *image = [renderer imageWithActions:^(UIGraphicsImageRendererContext * /*context*/) {
    UIBezierPath *circle = [UIBezierPath bezierPathWithOvalInRect:bounds];
    [circle addClip];
    if (preview) {
      /* Some previews are partly transparent; keep every brush a solid disc on the glass. */
      [[UIColor colorWithWhite:0.16 alpha:1.0] setFill];
      UIRectFill(bounds);
      /* Aspect-fill the square preview so the circle has no empty corners. */
      const CGSize size = preview.size;
      const CGFloat scale = diameter / MAX(1.0, MIN(size.width, size.height));
      const CGSize fill = CGSizeMake(size.width * scale, size.height * scale);
      [preview drawInRect:CGRectMake((diameter - fill.width) / 2,
                                     (diameter - fill.height) / 2,
                                     fill.width,
                                     fill.height)];
    }
    else {
      [[UIColor colorWithWhite:0.5 alpha:0.28] setFill];
      UIRectFill(bounds);
      UIImage *tinted = [symbol imageWithTintColor:[UIColor colorWithWhite:0.92 alpha:1.0]
                                     renderingMode:UIImageRenderingModeAlwaysOriginal];
      [tinted drawAtPoint:CGPointMake((diameter - tinted.size.width) / 2,
                                      (diameter - tinted.size.height) / 2)];
    }
    UIBezierPath *ring = [UIBezierPath bezierPathWithOvalInRect:CGRectInset(bounds, 0.5, 0.5)];
    ring.lineWidth = 1.0;
    [[UIColor colorWithWhite:1.0 alpha:0.22] setStroke];
    [ring stroke];
  }];
  image = [image imageWithRenderingMode:UIImageRenderingModeAlwaysOriginal];
  if (preview || !previewPath.length) {
    /* A missing file may still be written by Blender; only cache what is final. */
    [cache setObject:image forKey:cache_key];
  }
  return image;
}

UIImage *blenderIOSCommandIcon(NSInteger command, NSString *systemImage)
{
  return blenderIOSIcon([NSString stringWithFormat:@"command.%ld", (long)command], systemImage);
}

UIImage *blenderIOSActionIcon(NSString *opName, NSString *propertyName, NSString *propertyValue)
{
  NSString *key = [@"operator." stringByAppendingString:opName ?: @""];
  if (propertyName.length > 0 && propertyValue.length > 0) {
    UIImage *image = blenderIOSIcon(
        [NSString stringWithFormat:@"%@.%@.%@", key, propertyName, propertyValue], nil);
    if (image != nil) {
      return image;
    }
  }

  /* Only unambiguous, operator-specific SF fallbacks belong in action shortcuts.
   * Unmapped actions are text-only, never given a generic placeholder. */
  NSDictionary *fallbacks = @{
    @"ED_OT_undo" : @"arrow.uturn.backward",
    @"ED_OT_redo" : @"arrow.uturn.forward",
    @"OBJECT_OT_duplicate_move" : @"plus.square.on.square",
    @"OBJECT_OT_delete" : @"trash",
    @"MESH_OT_delete" : @"trash",
  };
  return blenderIOSIcon(key, opName ? fallbacks[opName] : nil);
}

/* The interface's SwiftUI classes, found by name as this library is built without Swift. */
Class blenderIOSSwiftClass(NSString *name)
{
  Class swift_class = NSClassFromString(name);
  NSCAssert(swift_class, @"The SwiftUI class %@ is missing", name);
  return swift_class;
}

NSString *blenderIOSMenuRoute(NSDictionary *route)
{
  NSData *data = [NSJSONSerialization dataWithJSONObject:route options:0 error:nil];
  return [[[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] autorelease];
}

UIImage *blenderIOSMenuIcon(NSDictionary *item)
{
  NSString *name = item[@"icon"];
  if (![name isKindOfClass:NSString.class] || name.length == 0 ||
      [@[ @"NONE", @"BLANK1" ] containsObject:name])
  {
    return nil;
  }
  return WMIOSBlenderIcon(name, NO);
}

NSArray *blenderIOSMenuResult(uint64_t identifier)
{
  char *json = WM_apple_ios_ui_menu_take_result(identifier);
  if (!json) {
    return nil;
  }
  NSData *data = [NSData dataWithBytes:json length:strlen(json)];
  free(json);
  id items = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
  return [items isKindOfClass:[NSArray class]] ? items : @[];
}

UIMenu *blenderIOSNativeSubmenu(NSDictionary *item, NSString *identifier)
{
  NSString *route = blenderIOSMenuRoute(item[@"route"]);
  /* Uncached providers refresh dynamic menus (recent files, undo, add-ons) on open.
   * The engine fills the request in its event pump; UIKit never calls into Python. */
  UIDeferredMenuElement *content = [UIDeferredMenuElement
      elementWithUncachedProvider:^(void (^completion)(NSArray<UIMenuElement *> *)) {
        const uint64_t request = WM_apple_ios_ui_menu_request(route.UTF8String, false);
        const CFAbsoluteTime deadline = CFAbsoluteTimeGetCurrent() + 60.0;
        NSTimer *poll = [NSTimer
            timerWithTimeInterval:0.025
                          repeats:YES
                            block:^(NSTimer *timer) {
                              NSArray *result = blenderIOSMenuResult(request);
                              if (result || CFAbsoluteTimeGetCurrent() >= deadline) {
                                [timer invalidate];
                                NSArray *elements = blenderIOSNativeMenuElements(result);
                                if (elements.count == 0) {
                                  UIAction *empty = [UIAction
                                      actionWithTitle:result ? @"No Available Commands" :
                                                               @"Blender Is Busy"
                                                image:nil
                                           identifier:nil
                                              handler:^(__kindof UIAction *){
                                              }];
                                  empty.attributes = UIMenuElementAttributesDisabled;
                                  elements = @[ empty ];
                                }
                                completion(elements);
                              }
                            }];
        [NSRunLoop.mainRunLoop addTimer:poll forMode:NSRunLoopCommonModes];
      }];
  return [UIMenu menuWithTitle:item[@"title"]
                         image:blenderIOSMenuIcon(item)
                    identifier:identifier
                       options:0
                      children:@[ content ]];
}

NSArray<UIMenuElement *> *blenderIOSNativeMenuElements(NSArray *items)
{
  NSMutableArray *sections = [NSMutableArray array];
  NSMutableArray *children = [NSMutableArray array];
  void (^flush)(void) = ^{
    if (children.count) {
      [sections addObject:[UIMenu menuWithTitle:@""
                                          image:nil
                                     identifier:nil
                                        options:UIMenuOptionsDisplayInline
                                       children:[[children copy] autorelease]]];
      [children removeAllObjects];
    }
  };
  for (NSDictionary *item in items) {
    if (![item isKindOfClass:NSDictionary.class]) {
      continue;
    }
    if ([item[@"separator"] boolValue]) {
      flush();
      continue;
    }
    if (![item[@"title"] isKindOfClass:NSString.class] || ![item[@"title"] length] ||
        ![item[@"route"] isKindOfClass:NSDictionary.class])
    {
      continue;
    }
    if ([item[@"menu"] isEqual:@"TOPBAR_MT_file_new"]) {
      [children
          addObject:[UIKeyCommand commandWithTitle:@"New…"
                                             image:[UIImage systemImageNamed:@"doc.badge.plus"]
                                            action:NSSelectorFromString(@"newDocument:")
                                             input:@"n"
                                     modifierFlags:UIKeyModifierCommand
                                      propertyList:nil]];
      continue;
    }
    if ([item[@"submenu"] boolValue] && ![item[@"disabled"] boolValue]) {
      [children addObject:blenderIOSNativeSubmenu(item, nil)];
      continue;
    }
    NSString *route = blenderIOSMenuRoute(item[@"route"]);
    UIImage *icon = blenderIOSMenuIcon(item);
    UIAction *action = [UIAction actionWithTitle:item[@"title"]
                                           image:icon
                                      identifier:nil
                                         handler:^(__kindof UIAction *) {
                                           WM_apple_ios_ui_menu_request(route.UTF8String, true);
                                         }];
    action.attributes = [item[@"disabled"] boolValue] ? UIMenuElementAttributesDisabled : 0;
    action.state = [item[@"checked"] boolValue] ? UIMenuElementStateOn : UIMenuElementStateOff;
    [children addObject:action];
  }
  flush();
  return sections;
}
