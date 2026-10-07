/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#import "GHOST_IOSTimeline.hh"
#import <QuartzCore/QuartzCore.h>
#include <cmath>

extern "C" void WM_apple_ios_ui_setting_enqueue(const char *, const char *);

static const CGFloat kTimelineHeight = 56.0;
static const CGFloat kTimelineMaxWidth = 760.0;

@implementation GHOSTIOSTimeline {
  UIView *_overlay;
  UIBarButtonItem *_button;
  UIVisualEffectView *_panel;
  UIButton *_play, *_jumpStart, *_jumpEnd, *_record;
  BOOL _recording;
  UISlider *_slider;
  UILabel *_startLabel, *_endLabel, *_frameLabel;
  NSDictionary *_snapshot;
  NSInteger _sentFrame;
  BOOL _scrubbing;
  BOOL _visible;
}

@synthesize visibilityDidChange = _visibilityDidChange;

- (instancetype)initWithOverlay:(UIView *)overlay
{
  if ((self = [super init])) {
    _overlay = overlay;
    UIImage *icon = [[UIImage imageNamed:@"blender.time"]
        imageWithRenderingMode:UIImageRenderingModeAlwaysTemplate];
    _button = [[UIBarButtonItem alloc] initWithImage:icon ?: [UIImage systemImageNamed:@"timeline.selection"]
        style:UIBarButtonItemStylePlain target:self action:@selector(toggle:)];
    _button.title = _button.accessibilityLabel = @"Timeline";
    _button.identifier = _button.accessibilityIdentifier = @"blender.timeline";

    _panel = [[UIVisualEffectView alloc] initWithEffect:[UIGlassEffect effectWithStyle:UIGlassEffectStyleRegular]];
    _panel.accessibilityIdentifier = @"blender.timeline.panel";
    _panel.layer.cornerRadius = kTimelineHeight / 2;
    _panel.layer.cornerCurve = kCACornerCurveContinuous;
    _panel.clipsToBounds = YES;
    _panel.hidden = YES;
    _panel.alpha = 0;

    _record = [self transportButton:@"record.circle" label:@"Auto Keying" command:@"autokey"];
    __block GHOSTIOSTimeline *recorder = self;
    [_record addAction:[UIAction actionWithHandler:^(__kindof UIAction *action) {
      /* Show the new state at once; Blender's snapshot confirms it. */
      [recorder setRecording:!recorder->_recording];
    }] forControlEvents:UIControlEventTouchUpInside];
    UIButton *start = _jumpStart = [self transportButton:@"backward.end.fill" label:@"Jump to Start" command:@"start"];
    UIButton *previous = [self transportButton:@"backward.frame.fill" label:@"Previous Keyframe" command:@"prev_key"];
    _play = [self transportButton:@"play.fill" label:@"Play" command:@"play"];
    UIButton *next = [self transportButton:@"forward.frame.fill" label:@"Next Keyframe" command:@"next_key"];
    UIButton *end = _jumpEnd = [self transportButton:@"forward.end.fill" label:@"Jump to End" command:@"end"];

    _startLabel = [self rangeLabel];
    _endLabel = [self rangeLabel];
    _slider = [[[UISlider alloc] init] autorelease];
    _slider.continuous = YES;
    _slider.accessibilityLabel = @"Current Frame";
    _slider.accessibilityIdentifier = @"blender.timeline.scrubber";
    __block GHOSTIOSTimeline *owner = self;
    [_slider addAction:[UIAction actionWithHandler:^(__kindof UIAction *action) {
      [owner beginScrub:action.sender];
    }] forControlEvents:UIControlEventTouchDown];
    [_slider addAction:[UIAction actionWithHandler:^(__kindof UIAction *action) {
      [owner scrub:action.sender];
    }] forControlEvents:UIControlEventValueChanged];
    [_slider addAction:[UIAction actionWithHandler:^(__kindof UIAction *action) {
      [owner endScrub:action.sender];
    }] forControlEvents:UIControlEventTouchUpInside | UIControlEventTouchUpOutside | UIControlEventTouchCancel];
    [_slider setContentHuggingPriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];

    _frameLabel = [[[UILabel alloc] init] autorelease];
    _frameLabel.font = [[UIFontMetrics metricsForTextStyle:UIFontTextStyleHeadline]
        scaledFontForFont:[UIFont monospacedDigitSystemFontOfSize:17 weight:UIFontWeightSemibold]];
    _frameLabel.adjustsFontForContentSizeCategory = YES;
    _frameLabel.textAlignment = NSTextAlignmentCenter;
    _frameLabel.accessibilityIdentifier = @"blender.timeline.frame";
    [_frameLabel.widthAnchor constraintGreaterThanOrEqualToConstant:56].active = YES;
    [_frameLabel setContentCompressionResistancePriority:UILayoutPriorityRequired
                                                 forAxis:UILayoutConstraintAxisHorizontal];

    UIStackView *transport = [[[UIStackView alloc]
        initWithArrangedSubviews:@[_record, start, previous, _play, next, end]] autorelease];
    transport.spacing = 0;
    UIStackView *row = [[[UIStackView alloc]
        initWithArrangedSubviews:@[transport, _startLabel, _slider, _endLabel, _frameLabel]] autorelease];
    row.alignment = UIStackViewAlignmentCenter;
    row.spacing = 8;
    [row setCustomSpacing:12 afterView:transport];
    [row setCustomSpacing:12 afterView:_endLabel];
    row.translatesAutoresizingMaskIntoConstraints = NO;
    [_panel.contentView addSubview:row];
    [NSLayoutConstraint activateConstraints:@[
      [row.leadingAnchor constraintEqualToAnchor:_panel.contentView.leadingAnchor constant:8],
      [row.trailingAnchor constraintEqualToAnchor:_panel.contentView.trailingAnchor constant:-16],
      [row.topAnchor constraintEqualToAnchor:_panel.contentView.topAnchor],
      [row.bottomAnchor constraintEqualToAnchor:_panel.contentView.bottomAnchor],
    ]];
    [_overlay addSubview:_panel];
    [transport setCustomSpacing:4 afterView:_record];
    [self updateSnapshot:@{@"frame": @1, @"start": @1, @"end": @250, @"playing": @NO, @"autokey": @NO}];
  }
  return self;
}

- (void)dealloc
{
  [_panel removeFromSuperview];
  [_panel release];
  [_button release];
  [_snapshot release];
  [_visibilityDidChange release];
  [super dealloc];
}

- (UIBarButtonItem *)button { return _button; }
- (UIView *)view { return _panel; }
- (BOOL)visible { return _visible; }
- (CGFloat)height { return kTimelineHeight; }

- (UIButton *)transportButton:(NSString *)symbol label:(NSString *)label command:(NSString *)command
{
  UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
  [button setImage:[UIImage systemImageNamed:symbol] forState:UIControlStateNormal];
  button.tintColor = UIColor.labelColor;
  button.accessibilityLabel = label;
  button.accessibilityIdentifier = [@"blender.timeline." stringByAppendingString:command];
  [button addAction:[UIAction actionWithHandler:^(__kindof UIAction *action) {
    WM_apple_ios_ui_setting_enqueue("timeline", command.UTF8String);
  }] forControlEvents:UIControlEventTouchUpInside];
  [button.widthAnchor constraintEqualToConstant:44].active = YES;
  [button.heightAnchor constraintEqualToConstant:44].active = YES;
  return button;
}

- (UILabel *)rangeLabel
{
  UILabel *label = [[[UILabel alloc] init] autorelease];
  label.font = [[UIFontMetrics metricsForTextStyle:UIFontTextStyleCaption1]
      scaledFontForFont:[UIFont monospacedDigitSystemFontOfSize:12 weight:UIFontWeightRegular]];
  label.adjustsFontForContentSizeCategory = YES;
  label.textColor = UIColor.secondaryLabelColor;
  [label setContentCompressionResistancePriority:UILayoutPriorityRequired
                                         forAxis:UILayoutConstraintAxisHorizontal];
  return label;
}

- (void)updateSnapshot:(NSDictionary *)snapshot
{
  if (![snapshot isKindOfClass:NSDictionary.class] || [_snapshot isEqual:snapshot]) {
    return;
  }
  [_snapshot release];
  _snapshot = [snapshot copy];
  const NSInteger start = [snapshot[@"start"] integerValue];
  const NSInteger end = MAX(start, [snapshot[@"end"] integerValue]);
  const NSInteger frame = [snapshot[@"frame"] integerValue];
  const BOOL playing = [snapshot[@"playing"] boolValue];
  UIColor *rangeColor = [snapshot[@"preview"] boolValue] ? UIColor.systemOrangeColor :
                                                          UIColor.secondaryLabelColor;
  _startLabel.text = [NSString stringWithFormat:@"%ld", (long)start];
  _endLabel.text = [NSString stringWithFormat:@"%ld", (long)end];
  _startLabel.textColor = _endLabel.textColor = rangeColor;
  _slider.minimumValue = start;
  _slider.maximumValue = end;
  /* Keep the thumb under the finger; Blender's frame catches up through the inbox. */
  if (!_scrubbing) {
    _slider.value = frame;
    _frameLabel.text = [NSString stringWithFormat:@"%ld", (long)frame];
    _slider.accessibilityValue = _frameLabel.text;
  }
  [_play setImage:[UIImage systemImageNamed:playing ? @"pause.fill" : @"play.fill"]
         forState:UIControlStateNormal];
  _play.accessibilityLabel = playing ? @"Pause" : @"Play";
  [self setRecording:[snapshot[@"autokey"] boolValue]];
}

- (void)setRecording:(BOOL)recording
{
  _recording = recording;
  [_record setImage:[UIImage systemImageNamed:recording ? @"record.circle.fill" : @"record.circle"]
           forState:UIControlStateNormal];
  _record.tintColor = recording ? UIColor.systemRedColor : UIColor.labelColor;
  _record.accessibilityValue = recording ? @"On" : @"Off";
  _record.accessibilityTraits = UIAccessibilityTraitButton | (recording ? UIAccessibilityTraitSelected : 0);
}

- (void)beginScrub:(UISlider *)slider
{
  _scrubbing = YES;
  _sentFrame = NSIntegerMin;
}

- (void)scrub:(UISlider *)slider
{
  const NSInteger frame = lround(slider.value);
  _frameLabel.text = [NSString stringWithFormat:@"%ld", (long)frame];
  _slider.accessibilityValue = _frameLabel.text;
  if (frame == _sentFrame) {
    return;
  }
  _sentFrame = frame;
  NSString *value = [NSString stringWithFormat:@"%ld", (long)frame];
  WM_apple_ios_ui_setting_enqueue("timeline_frame", value.UTF8String);
}

- (void)endScrub:(UISlider *)slider
{
  [self scrub:slider];
  _scrubbing = NO;
  _slider.value = lround(slider.value);
}

- (void)toggle:(id)sender
{
  _visible = !_visible;
  _button.selected = _visible;
  const BOOL reduceMotion = UIAccessibilityIsReduceMotionEnabled();
  CGAffineTransform hiddenTransform = reduceMotion ? CGAffineTransformIdentity :
      CGAffineTransformConcat(CGAffineTransformMakeTranslation(0, 14), CGAffineTransformMakeScale(0.98, 0.98));
  _panel.userInteractionEnabled = _visible;
  if (_visible) {
    [_overlay bringSubviewToFront:_panel];
    if (_panel.hidden) {
      _panel.alpha = 0;
      _panel.transform = hiddenTransform;
    }
    _panel.hidden = NO;
  }
  const BOOL visible = _visible;
  UIView *panel = _panel;
  [UIView animateWithDuration:reduceMotion ? 0 : 0.28
      delay:0 usingSpringWithDamping:0.9 initialSpringVelocity:0
      options:UIViewAnimationOptionBeginFromCurrentState | UIViewAnimationOptionAllowUserInteraction
      animations:^{
        panel.alpha = visible ? 1 : 0;
        panel.transform = visible ? CGAffineTransformIdentity : hiddenTransform;
      } completion:^(BOOL finished) {
        if (finished && panel.alpha == 0) {
          panel.hidden = YES;
        }
      }];
  if (_visibilityDidChange) {
    _visibilityDidChange();
  }
}

- (void)layoutInRect:(CGRect)rect
{
  const CGFloat width = MIN(kTimelineMaxWidth, MAX(0, rect.size.width));
  /* Frame is undefined while transformed; layout must not disturb the presentation animation. */
  _panel.bounds = CGRectMake(0, 0, width, kTimelineHeight);
  /* Phones keep play, keyframe steps and the scrubber; the range is still on the slider. */
  const BOOL compact = width < 520;
  _jumpStart.hidden = _jumpEnd.hidden = _startLabel.hidden = _endLabel.hidden = compact;
  _panel.center = CGPointMake(CGRectGetMidX(rect), CGRectGetMaxY(rect) - kTimelineHeight / 2);
}

@end
