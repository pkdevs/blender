/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#import "GHOST_IOSCompileIndicator.hh"
#import "GHOST_IOSRuntime.hh"
#import <QuartzCore/QuartzCore.h>

/* Brief hitches are not worth a message: only show once Blender has been held up this long. */
static const double kShowAfter = 0.3;
/* Once shown, stay long enough to be read rather than flicker. */
static const double kMinimumVisible = 0.5;

@implementation GHOSTIOSCompileIndicator {
  UIVisualEffectView *_pill;
  UIProgressView *_progress;
  UILabel *_count;
  NSTimer *_timer;
  BOOL _shown;
  CFTimeInterval _shownAt;
}

- (instancetype)initWithOverlay:(UIView *)overlay below:(UIView *)anchor
{
  if ((self = [super init])) {
    _pill = [[UIVisualEffectView alloc]
        initWithEffect:[UIGlassEffect effectWithStyle:UIGlassEffectStyleRegular]];
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

    UILabel *title = [[[UILabel alloc] init] autorelease];
    title.text = @"Compiling Shaders";
    title.font = [UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];
    title.textColor = UIColor.secondaryLabelColor;

    _progress = [[UIProgressView alloc] initWithProgressViewStyle:UIProgressViewStyleDefault];
    [_progress.widthAnchor constraintEqualToConstant:88].active = YES;

    _count = [[UILabel alloc] init];
    _count.font = [UIFont monospacedDigitSystemFontOfSize:12 weight:UIFontWeightRegular];
    _count.textColor = UIColor.tertiaryLabelColor;

    UIStackView *row = [[[UIStackView alloc] initWithArrangedSubviews:@[title, _progress, _count]]
        autorelease];
    row.alignment = UIStackViewAlignmentCenter;
    row.spacing = 10;
    row.translatesAutoresizingMaskIntoConstraints = NO;
    [_pill.contentView addSubview:row];
    [overlay addSubview:_pill];
    [NSLayoutConstraint activateConstraints:@[
      [row.leadingAnchor constraintEqualToAnchor:_pill.contentView.leadingAnchor constant:14],
      [row.trailingAnchor constraintEqualToAnchor:_pill.contentView.trailingAnchor constant:-14],
      [row.centerYAnchor constraintEqualToAnchor:_pill.contentView.centerYAnchor],
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
                                       block:^(NSTimer *) { [owner update]; }] retain];
    [NSRunLoop.mainRunLoop addTimer:_timer forMode:NSRunLoopCommonModes];
  }
  return self;
}

- (void)dealloc
{
  [self stop];
  [_pill removeFromSuperview];
  [_pill release];
  [_progress release];
  [_count release];
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
    const float fraction = total > 0 ? float(finished) / float(total) : 0.0f;
    [_progress setProgress:fraction animated:_shown && fraction >= _progress.progress];
    _count.text = [NSString stringWithFormat:@"%d/%d", finished, total];
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
