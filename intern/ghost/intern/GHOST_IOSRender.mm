/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#import "GHOST_IOSRender.hh"
#import <AVFoundation/AVFoundation.h>
#import <AVKit/AVKit.h>

extern "C" void WM_apple_ios_ui_setting_enqueue(const char *, const char *);

/* -------------------------------------------------------------------- */
/* Video from rendered frames. */

/** Encode PNG `frames` into an H.264 video at `fps`. Calls `progress` and then `done` on the main
 * thread; `done` gets the video or nil. */
static void encodeVideo(NSArray<NSString *> *frames,
                        double fps,
                        NSURL *output,
                        void (^progress)(float),
                        void (^done)(NSURL *))
{
  progress = [[progress copy] autorelease];
  done = [[done copy] autorelease];
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    NSURL *result = nil;
    @autoreleasepool {
      UIImage *first = frames.count ? [UIImage imageWithContentsOfFile:frames.firstObject] : nil;
      CGImageRef first_image = first.CGImage;
      /* H.264 needs even dimensions. */
      const size_t width = first_image ? CGImageGetWidth(first_image) & ~size_t(1) : 0;
      const size_t height = first_image ? CGImageGetHeight(first_image) & ~size_t(1) : 0;
      [NSFileManager.defaultManager removeItemAtURL:output error:nil];
      AVAssetWriter *writer = width && height ?
                                  [AVAssetWriter assetWriterWithURL:output
                                                           fileType:AVFileTypeMPEG4
                                                              error:nil] :
                                  nil;
      AVAssetWriterInput *input = [AVAssetWriterInput
          assetWriterInputWithMediaType:AVMediaTypeVideo
                         outputSettings:@{
                           AVVideoCodecKey: AVVideoCodecTypeH264,
                           AVVideoWidthKey: @(width),
                           AVVideoHeightKey: @(height),
                         }];
      AVAssetWriterInputPixelBufferAdaptor *adaptor = [AVAssetWriterInputPixelBufferAdaptor
          assetWriterInputPixelBufferAdaptorWithAssetWriterInput:input
                                     sourcePixelBufferAttributes:@{
                                       (NSString *)kCVPixelBufferPixelFormatTypeKey:
                                           @(kCVPixelFormatType_32BGRA),
                                       (NSString *)kCVPixelBufferWidthKey: @(width),
                                       (NSString *)kCVPixelBufferHeightKey: @(height),
                                     }];
      if (writer && [writer canAddInput:input]) {
        [writer addInput:input];
        [writer startWriting];
        [writer startSessionAtSourceTime:kCMTimeZero];
        const int32_t timescale = 600;
        BOOL ok = YES;
        for (NSUInteger i = 0; ok && i < frames.count; i++) {
          @autoreleasepool {
            while (!input.readyForMoreMediaData) {
              [NSThread sleepForTimeInterval:0.005];
            }
            CGImageRef image = [UIImage imageWithContentsOfFile:frames[i]].CGImage;
            CVPixelBufferRef buffer = nullptr;
            if (image && adaptor.pixelBufferPool &&
                CVPixelBufferPoolCreatePixelBuffer(nullptr, adaptor.pixelBufferPool, &buffer) ==
                    kCVReturnSuccess)
            {
              CVPixelBufferLockBaseAddress(buffer, 0);
              CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
              CGContextRef context = CGBitmapContextCreate(
                  CVPixelBufferGetBaseAddress(buffer),
                  width,
                  height,
                  8,
                  CVPixelBufferGetBytesPerRow(buffer),
                  space,
                  kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
              CGContextDrawImage(context, CGRectMake(0, 0, width, height), image);
              CGContextRelease(context);
              CGColorSpaceRelease(space);
              CVPixelBufferUnlockBaseAddress(buffer, 0);
              const CMTime time = CMTimeMake(int64_t(llround(i * timescale / fps)), timescale);
              ok = [adaptor appendPixelBuffer:buffer withPresentationTime:time];
              CVPixelBufferRelease(buffer);
            }
            const float fraction = float(i + 1) / float(frames.count);
            dispatch_async(dispatch_get_main_queue(), ^{ progress(fraction); });
          }
        }
        [input markAsFinished];
        dispatch_semaphore_t finished = dispatch_semaphore_create(0);
        [writer finishWritingWithCompletionHandler:^{ dispatch_semaphore_signal(finished); }];
        dispatch_semaphore_wait(finished, DISPATCH_TIME_FOREVER);
        dispatch_release(finished);
        if (ok && writer.status == AVAssetWriterStatusCompleted) {
          result = [output retain];
        }
      }
    }
    dispatch_async(dispatch_get_main_queue(), ^{
      done(result);
      [result release];
    });
  });
}

/* -------------------------------------------------------------------- */
/* The render sheet. */

@interface GHOSTIOSRender () <UIAdaptivePresentationControllerDelegate>
@end

@implementation GHOSTIOSRender {
  UIViewController *_presenter;
  UINavigationController *_sheet;
  UIViewController *_content;
  UIImageView *_preview;
  UIProgressView *_progress;
  UILabel *_detail;
  UIActivityIndicatorView *_spinner;
  AVPlayer *_player;
  id _loop;
  NSURL *_result;
  NSNumber *_lastID;
  NSString *_lastFrame;
  BOOL _animation;
  BOOL _finishing;
}

- (instancetype)initWithPresenter:(UIViewController *)presenter
{
  if ((self = [super init])) {
    _presenter = presenter;
  }
  return self;
}

- (void)dealloc
{
  [self tearDown];
  [super dealloc];
}

- (void)tearDown
{
  if (_loop) {
    [NSNotificationCenter.defaultCenter removeObserver:_loop];
    [_loop release];
    _loop = nil;
  }
  [_player pause];
  [_player release];
  _player = nil;
  [_sheet release];
  _sheet = nil;
  _content = nil;
  _preview = nil;
  _progress = nil;
  _detail = nil;
  _spinner = nil;
  [_result release];
  _result = nil;
  [_lastFrame release];
  _lastFrame = nil;
  _finishing = NO;
}

- (void)renderAnimation:(BOOL)animation
{
  if (_sheet || _presenter.presentedViewController) {
    return;
  }
  _animation = animation;
  [_lastID release];
  _lastID = nil;
  WM_apple_ios_ui_setting_enqueue("render", animation ? "animation" : "image");
  [self presentSheet];
}

- (void)presentSheet
{
  _content = [[[UIViewController alloc] init] autorelease];
  _content.title = _animation ? @"Rendering Animation" : @"Rendering Image";
  _content.view.backgroundColor = UIColor.systemGroupedBackgroundColor;
  _content.navigationItem.leftBarButtonItem = [[[UIBarButtonItem alloc]
      initWithBarButtonSystemItem:UIBarButtonSystemItemCancel
                           target:self
                           action:@selector(cancel)] autorelease];

  _preview = [[[UIImageView alloc] init] autorelease];
  _preview.contentMode = UIViewContentModeScaleAspectFit;
  _preview.backgroundColor = UIColor.secondarySystemGroupedBackgroundColor;
  _preview.layer.cornerRadius = 12;
  _preview.layer.cornerCurve = kCACornerCurveContinuous;
  _preview.clipsToBounds = YES;
  _preview.accessibilityIgnoresInvertColors = YES;
  _spinner = [[[UIActivityIndicatorView alloc]
      initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium] autorelease];
  [_spinner startAnimating];
  _spinner.translatesAutoresizingMaskIntoConstraints = NO;
  [_preview addSubview:_spinner];

  _progress = [[[UIProgressView alloc] initWithProgressViewStyle:UIProgressViewStyleDefault]
      autorelease];
  _detail = [[[UILabel alloc] init] autorelease];
  _detail.font = [UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];
  _detail.adjustsFontForContentSizeCategory = YES;
  _detail.textColor = UIColor.secondaryLabelColor;
  _detail.textAlignment = NSTextAlignmentCenter;
  _detail.text = @"Preparing…";

  UIStackView *stack = [[[UIStackView alloc] initWithArrangedSubviews:@[_preview, _progress, _detail]]
      autorelease];
  stack.axis = UILayoutConstraintAxisVertical;
  stack.spacing = 12;
  stack.translatesAutoresizingMaskIntoConstraints = NO;
  [_content.view addSubview:stack];
  UILayoutGuide *safe = _content.view.safeAreaLayoutGuide;
  [NSLayoutConstraint activateConstraints:@[
    [stack.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:20],
    [stack.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-20],
    [stack.topAnchor constraintEqualToAnchor:safe.topAnchor constant:12],
    [stack.bottomAnchor constraintEqualToAnchor:safe.bottomAnchor constant:-20],
    [_spinner.centerXAnchor constraintEqualToAnchor:_preview.centerXAnchor],
    [_spinner.centerYAnchor constraintEqualToAnchor:_preview.centerYAnchor],
  ]];

  _sheet = [[UINavigationController alloc] initWithRootViewController:_content];
  _sheet.modalPresentationStyle = UIModalPresentationFormSheet;
  _sheet.preferredContentSize = CGSizeMake(540, 420);
  /* While rendering, only Cancel ends it. */
  _sheet.modalInPresentation = YES;
  _sheet.presentationController.delegate = self;
  [_presenter presentViewController:_sheet animated:YES completion:nil];
}

- (void)presentationControllerDidDismiss:(UIPresentationController *)presentationController
{
  /* The result was swiped away. */
  [self tearDown];
}

- (void)cancel
{
  WM_apple_ios_ui_setting_enqueue("render", "cancel");
  [self dismiss];
}

- (void)dismiss
{
  UINavigationController *sheet = [[_sheet retain] autorelease];
  [self tearDown];
  [sheet dismissViewControllerAnimated:YES completion:nil];
}

- (void)updateStatus:(NSDictionary *)status
{
  if (![status isKindOfClass:NSDictionary.class] || !_sheet || _finishing ||
      [_lastID isEqual:status[@"id"]])
  {
    return;
  }
  [_lastID release];
  _lastID = [status[@"id"] retain];
  NSString *state = status[@"state"];
  if ([state isEqual:@"rendering"]) {
    [self showProgress:status];
  }
  else if ([state isEqual:@"done"]) {
    _finishing = YES;
    WM_apple_ios_ui_setting_enqueue("render", "clear");
    if (_animation) {
      [self makeVideo:status];
    }
    else {
      [self showImage:status[@"image"]];
    }
  }
  else if ([state isEqual:@"error"]) {
    WM_apple_ios_ui_setting_enqueue("render", "clear");
    NSString *message = status[@"message"] ?: @"The render could not be completed.";
    UINavigationController *sheet = [[_sheet retain] autorelease];
    [self tearDown];
    UIViewController *presenter = _presenter;
    [sheet dismissViewControllerAnimated:YES completion:^{
      UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Unable to Render"
                                                                     message:message
                                                              preferredStyle:UIAlertControllerStyleAlert];
      [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
      [presenter presentViewController:alert animated:YES completion:nil];
    }];
  }
  else if ([state isEqual:@"cancelled"]) {
    WM_apple_ios_ui_setting_enqueue("render", "clear");
    [self dismiss];
  }
}

- (void)showProgress:(NSDictionary *)status
{
  const double total = MAX(1.0, [status[@"total"] doubleValue]);
  const double done = [status[@"done"] doubleValue];
  const double samples = [status[@"samples"] doubleValue];
  const float fraction = float(MIN(1.0, (done + samples) / total));
  [_progress setProgress:fraction animated:fraction >= _progress.progress];
  if (_animation) {
    _detail.text = [NSString stringWithFormat:@"Frame %ld of %ld", (long)MIN(done + 1, total),
                                              (long)total];
    NSString *last = status[@"last"];
    if (last.length && ![last isEqual:_lastFrame]) {
      [_lastFrame release];
      _lastFrame = [last copy];
      /* Show each frame as it is finished. */
      UIImage *image = [UIImage imageWithContentsOfFile:last];
      if (image) {
        [_spinner stopAnimating];
        [UIView transitionWithView:_preview
                          duration:0.15
                           options:UIViewAnimationOptionTransitionCrossDissolve
                        animations:^{ self->_preview.image = image; }
                        completion:nil];
      }
    }
  }
  else {
    _detail.text = samples > 0 ? [NSString stringWithFormat:@"%ld%%", lround(samples * 100)] :
                                 @"Rendering…";
  }
  _progress.accessibilityValue = _detail.text;
}

- (void)showImage:(NSString *)path
{
  UIImage *image = path ? [UIImage imageWithContentsOfFile:path] : nil;
  if (!image) {
    _finishing = NO;
    [self updateStatus:@{@"id": @(-1), @"state": @"error"}];
    return;
  }
  [_result release];
  _result = [[NSURL fileURLWithPath:path] retain];
  [_spinner stopAnimating];
  _preview.image = image;
  _progress.hidden = YES;
  _detail.text = [NSString stringWithFormat:@"%ld × %ld", (long)(image.size.width * image.scale),
                                            (long)(image.size.height * image.scale)];
  [self showResultTitled:@"Render"];
}

- (void)makeVideo:(NSDictionary *)status
{
  NSArray<NSString *> *frames = status[@"frames"];
  NSString *folder = status[@"folder"];
  if (![frames isKindOfClass:NSArray.class] || !frames.count || !folder) {
    _finishing = NO;
    [self updateStatus:@{@"id": @(-1), @"state": @"error", @"message": @"No frames were rendered."}];
    return;
  }
  _content.title = @"Creating Video";
  _content.navigationItem.leftBarButtonItem.enabled = NO;
  _detail.text = @"Creating Video…";
  [_progress setProgress:0 animated:NO];
  const double fps = MAX(1.0, [status[@"fps"] doubleValue]);
  __block GHOSTIOSRender *owner = self;
  encodeVideo(
      frames,
      fps,
      [NSURL fileURLWithPath:[folder stringByAppendingPathComponent:@"Animation.mp4"]],
      ^(float fraction) { [owner->_progress setProgress:fraction animated:YES]; },
      ^(NSURL *video) {
        if (!owner->_sheet) {
          return;
        }
        if (!video) {
          owner->_finishing = NO;
          [owner updateStatus:@{@"id": @(-1), @"state": @"error",
                                @"message": @"The video could not be created."}];
          return;
        }
        [owner showVideo:video];
      });
}

- (void)showVideo:(NSURL *)video
{
  [_result release];
  _result = [video retain];
  _player = [[AVPlayer alloc] initWithURL:video];
  _player.actionAtItemEnd = AVPlayerActionAtItemEndNone;
  AVPlayer *player = _player;
  _loop = [[NSNotificationCenter.defaultCenter
      addObserverForName:AVPlayerItemDidPlayToEndTimeNotification
                  object:player.currentItem
                   queue:NSOperationQueue.mainQueue
              usingBlock:^(NSNotification *) {
                [player seekToTime:kCMTimeZero];
                [player play];
              }] retain];
  AVPlayerViewController *playback = [[[AVPlayerViewController alloc] init] autorelease];
  playback.player = _player;
  [_content addChildViewController:playback];
  playback.view.frame = _preview.bounds;
  playback.view.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  [_preview addSubview:playback.view];
  _preview.userInteractionEnabled = YES;
  [playback didMoveToParentViewController:_content];
  [_spinner stopAnimating];
  _progress.hidden = YES;
  _detail.text = nil;
  [_player play];
  [self showResultTitled:@"Animation"];
}

- (void)showResultTitled:(NSString *)title
{
  _content.title = title;
  _sheet.modalInPresentation = NO;
  UIBarButtonItem *share = [[[UIBarButtonItem alloc]
      initWithBarButtonSystemItem:UIBarButtonSystemItemAction
                           target:self
                           action:@selector(share:)] autorelease];
  share.accessibilityLabel = @"Share";
  _content.navigationItem.leftBarButtonItem = share;
  _content.navigationItem.rightBarButtonItem = [[[UIBarButtonItem alloc]
      initWithBarButtonSystemItem:UIBarButtonSystemItemDone
                           target:self
                           action:@selector(dismiss)] autorelease];
  [[[[UINotificationFeedbackGenerator alloc] init] autorelease]
      notificationOccurred:UINotificationFeedbackTypeSuccess];
}

- (void)share:(UIBarButtonItem *)sender
{
  if (!_result) {
    return;
  }
  UIActivityViewController *activity = [[[UIActivityViewController alloc]
      initWithActivityItems:@[_result]
      applicationActivities:nil] autorelease];
  activity.popoverPresentationController.sourceItem = sender;
  [_sheet presentViewController:activity animated:YES completion:nil];
}

@end
