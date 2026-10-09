/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * The iPad's thumbnail extension: shows the preview stored in a `.blend` file wherever iOS shows
 * the file, in the document browser, the Files app and share sheets. See `thumbnail_provider.mm`
 * for how such extensions work.
 */

#import <QuickLookThumbnailing/QuickLookThumbnailing.h>
#import <UIKit/UIKit.h>

#include <fcntl.h>
#include <unistd.h>

#include "BLI_filereader.h"
#include "blendthumb.hh"

@interface ThumbnailProvider : QLThumbnailProvider
@end

/** The preview stored in the file, or nil. */
static UIImage *thumbnail_image(NSURL *url)
{
  const int file = open(url.fileSystemRepresentation, O_RDONLY);
  if (file < 0) {
    return nil;
  }
  /* The reader closes the file. */
  blender::FileReader *reader = blender::BLI_filereader_new_file(file);
  if (reader == nullptr) {
    close(file);
    return nil;
  }
  blender::Thumbnail thumbnail;
  if (blendthumb_create_thumb_from_file(reader, &thumbnail) != blender::BT_OK) {
    return nil;
  }
  std::optional<blender::Vector<uint8_t>> png = blendthumb_create_png_data_from_thumb(&thumbnail);
  if (!png) {
    return nil;
  }
  return [UIImage imageWithData:[NSData dataWithBytes:png->data() length:png->size()]];
}

@implementation ThumbnailProvider

- (void)provideThumbnailForFileRequest:(QLFileThumbnailRequest *)request
                     completionHandler:(void (^)(QLThumbnailReply *reply, NSError *error))handler
{
  @autoreleasepool {
    UIImage *image = thumbnail_image(request.fileURL);
    if (image == nil || image.size.width <= 0 || image.size.height <= 0) {
      handler(nil, [NSError errorWithDomain:@"org.blender.thumbnailer" code:-1 userInfo:nil]);
      return;
    }
    /* Fit the preview in the requested size, keeping its shape. */
    const CGFloat scale = MIN(request.maximumSize.width / image.size.width,
                              request.maximumSize.height / image.size.height);
    const CGSize size = CGSizeMake(image.size.width * scale, image.size.height * scale);
    [image retain];
    /* Drawn into the current UIKit context, which UIImage places upright itself. */
    QLThumbnailReply *reply = [QLThumbnailReply
        replyWithContextSize:size
        currentContextDrawingBlock:^BOOL {
          [image drawInRect:CGRectMake(0, 0, size.width, size.height)];
          [image release];
          return YES;
        }];
    handler(reply, nil);
  }
}

@end
