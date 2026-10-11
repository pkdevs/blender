/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#import "GHOST_IOSCompileIndicator.hh"
#import "GHOST_IOSPlatform.hh"
#import "GHOST_IOSRuntime.hh"
#import <QuartzCore/QuartzCore.h>

/* Brief hitches are not worth a message: only show once Blender has been held up this long. */
static const double kShowAfter = 0.3;
/* Once shown, stay long enough to be read rather than flicker. */
static const double kMinimumVisible = 0.5;

/* The pill's contents in SwiftUI (`intern/swift/CompileIndicator.swift`). */
@interface UIView (GHOSTIOSSwiftCompileProgressView)
- (void)setFinished:(NSInteger)finished total:(NSInteger)total;
@end

@implementation GHOSTIOSCompileIndicator
{
  UIVisualEffectView *_pill;
  UIView *_content;
  NSTimer *_timer;
  BOOL _shown;
  CFTimeInterval _shownAt;
}

- (instancetype)initWithOverlay:(UIView *)overlay below:(UIView *)anchor
{
  if ((self = [super init])) {
    _pill = [[UIVisualEffectView alloc] initWithEffect:GHOSTIOSGlassEffect(NO, NO)];
    _pill.userInteractionEnabled = NO;
    _pill.layer.cornerRadius = 16;
    _pill.layer.cornerCurve = kCACornerCurveContinuous;
    _pill.clipsToBounds = YES;
    _pill.alpha = 0;
    _pill.hidden = YES;
    _pill.translatesAutoresizingMaskIntoConstraints = NO;
    _pill.accessibilityIdentifier = @"blender.shader-compilation";
    _pill.isAccessibilityElement = YES;
    _pill.accessibilityLabel = @"Compiling Shaders";

    Class content_class = NSClassFromString(@"GHOSTIOSSwiftCompileProgressView");
    NSCAssert(content_class, @"The SwiftUI compile progress view is missing");
    _content = [[content_class alloc] initWithFrame:CGRectZero];
    _content.translatesAutoresizingMaskIntoConstraints = NO;
    [_pill.contentView addSubview:_content];
    [overlay addSubview:_pill];
    [NSLayoutConstraint activateConstraints:@[
      [_content.leadingAnchor constraintEqualToAnchor:_pill.contentView.leadingAnchor],
      [_content.trailingAnchor constraintEqualToAnchor:_pill.contentView.trailingAnchor],
      [_content.heightAnchor constraintEqualToAnchor:_pill.contentView.heightAnchor],
      [_content.centerYAnchor constraintEqualToAnchor:_pill.contentView.centerYAnchor],
      [_pill.heightAnchor constraintEqualToConstant:32],
      [_pill.centerXAnchor constraintEqualToAnchor:overlay.centerXAnchor],
      [_pill.topAnchor constraintEqualToAnchor:anchor ? anchor.bottomAnchor :
                                                        overlay.safeAreaLayoutGuide.topAnchor
                                      constant:8],
    ]];

    /* The block must not keep the indicator alive; `stop` ends the timer. */
    __block GHOSTIOSCompileIndicator *owner = self;
    _timer = [[NSTimer timerWithTimeInterval:0.1
                                     repeats:YES
                                       block:^(NSTimer *) {
                                         [owner update];
                                       }] retain];
    [NSRunLoop.mainRunLoop addTimer:_timer forMode:NSRunLoopCommonModes];
  }
  return self;
}

- (void)dealloc
{
  [self stop];
  [_pill removeFromSuperview];
  [_pill release];
  [_content release];
  [super dealloc];
}

- (void)stop
{
  [_timer invalidate];
  [_timer release];
  _timer = nil;
  [self setShown:NO];
}

- (void)update
{
  int finished = 0, total = 0;
  const BOOL compiling = GHOST_ios_shader_compilation(&finished, &total);
  const BOOL held_up = GHOST_ios_engine_busy_seconds() > kShowAfter;
  if (compiling && held_up) {
    [_content setFinished:finished total:total];
    _pill.accessibilityValue = [NSString stringWithFormat:@"%d of %d", finished, total];
    [self setShown:YES];
  }
  else if (_shown && CACurrentMediaTime() - _shownAt >= kMinimumVisible) {
    [self setShown:NO];
  }
}

- (void)setShown:(BOOL)shown
{
  if (_shown == shown) {
    return;
  }
  _shown = shown;
  UIView *pill = _pill;
  const BOOL reduce_motion = UIAccessibilityIsReduceMotionEnabled();
  if (shown) {
    _shownAt = CACurrentMediaTime();
    pill.hidden = NO;
    pill.transform = reduce_motion ? CGAffineTransformIdentity :
                                     CGAffineTransformMakeTranslation(0, -6);
  }
  [UIView animateWithDuration:reduce_motion ? 0 : 0.2
      delay:0
      options:UIViewAnimationOptionBeginFromCurrentState
      animations:^{
        pill.alpha = shown ? 1 : 0;
        pill.transform = CGAffineTransformIdentity;
      }
      completion:^(BOOL) {
        if (!self->_shown) {
          pill.hidden = YES;
        }
      }];
}

@end
