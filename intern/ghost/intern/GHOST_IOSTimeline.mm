/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#import "GHOST_IOSTimeline.hh"
#import <QuartzCore/QuartzCore.h>
#include <cmath>
#include <initializer_list>

extern "C" void WM_apple_ios_ui_setting_enqueue(const char *, const char *);

static const CGFloat kTimelineHeight = 56.0;
/* Expanded: a slim row of controls along the bottom, and the full-width scrubber above it with
 * the keys over the knob's band. */
static const CGFloat kControlsRowHeight = 44.0;
static const CGFloat kKnobBandHeight = 50.0;
static const CGFloat kKeyAreaHeight = 42.0;
/* Small enough that the grabbed knob, scaled up, stays inside its band. */
static const CGFloat kKnobSize = 32.0;
static const CGFloat kKnobMinWidth = 46.0;

/* A keyframe diamond centered on `center`, as Blender draws them. */
static void blenderFillDiamond(CGPoint center, CGFloat size, UIColor *fill, UIColor *outline)
{
  UIBezierPath *diamond = [UIBezierPath bezierPath];
  [diamond moveToPoint:CGPointMake(center.x, center.y - size / 2)];
  [diamond addLineToPoint:CGPointMake(center.x + size / 2, center.y)];
  [diamond addLineToPoint:CGPointMake(center.x, center.y + size / 2)];
  [diamond addLineToPoint:CGPointMake(center.x - size / 2, center.y)];
  [diamond closePath];
  [fill setFill];
  [diamond fill];
  if (outline) {
    diamond.lineWidth = 1;
    [outline setStroke];
    [diamond stroke];
  }
}

static UIColor *blenderKeyColor(BOOL selected)
{
  return selected ? UIColor.systemOrangeColor : UIColor.labelColor;
}

/* The timeline's scrubber: a track with the keyed frames and a round glass knob that shows and
 * sets the current frame. Expanded, the same view grows upward into a key track, where keys are
 * selected by tapping or boxing them and dragged together to other frames; the knob's band and
 * the ruler still scrub. One frame mapping places the knob, its playhead line and the keys. */
@interface GHOSTIOSScrubber : UIView
@property(nonatomic, copy) NSArray *keys;
@property(nonatomic) NSInteger start, end;
@property(nonatomic) BOOL preview, expanded;
@property(nonatomic, readonly) NSInteger playhead;
@property(nonatomic, readonly) BOOL scrubbing;
@property(nonatomic, copy) void (^scrub)(NSInteger frame);
/* Replace the key selection with these frames. */
@property(nonatomic, copy) void (^selectKeys)(NSArray<NSNumber *> *frames);
/* Move the selected keys by a number of frames. */
@property(nonatomic, copy) void (^moveKeys)(NSInteger delta);
/* Blender's frame; ignored while a finger holds the knob. */
- (void)setPlayheadFromBlender:(NSInteger)frame;
@end

@implementation GHOSTIOSScrubber {
  UIVisualEffectView *_knob;
  UILabel *_knobLabel;
  UISelectionFeedbackGenerator *_ticks;
  UIImpactFeedbackGenerator *_grab;
  BOOL _scrubbing, _movingKeys, _boxing;
  NSInteger _playhead, _grabbed, _delta, _sent;
  CGFloat _boxFrom, _boxTo;
}

- (instancetype)initWithFrame:(CGRect)frame
{
  if ((self = [super initWithFrame:frame])) {
    self.opaque = NO;
    self.backgroundColor = UIColor.clearColor;
    self.contentMode = UIViewContentModeRedraw;
    self.accessibilityIdentifier = @"blender.timeline.scrubber";
    self.isAccessibilityElement = YES;
    self.accessibilityLabel = @"Current Frame";
    self.accessibilityTraits = UIAccessibilityTraitAdjustable;
    _start = 1;
    _end = 250;
    _playhead = 1;
    _sent = NSIntegerMin;

    /* Tinted like iOS's prominent glass buttons, in the playhead's blue. */
    UIGlassEffect *glass = [UIGlassEffect effectWithStyle:UIGlassEffectStyleRegular];
    glass.interactive = YES;
    glass.tintColor = UIColor.systemBlueColor;
    _knob = [[UIVisualEffectView alloc] initWithEffect:glass];
    _knob.layer.cornerRadius = kKnobSize / 2;
    _knob.layer.cornerCurve = kCACornerCurveContinuous;
    _knob.clipsToBounds = YES;
    /* Touches reach the knob so its glass responds to the finger; the scrubber's own gestures
     * still receive them and do the scrubbing. */
    _knob.userInteractionEnabled = YES;
    _knobLabel = [[UILabel alloc] init];
    _knobLabel.font = [UIFont monospacedDigitSystemFontOfSize:14 weight:UIFontWeightBold];
    _knobLabel.textAlignment = NSTextAlignmentCenter;
    _knobLabel.textColor = UIColor.whiteColor;
    _knobLabel.adjustsFontSizeToFitWidth = YES;
    _knobLabel.minimumScaleFactor = 0.7;
    _knobLabel.accessibilityIdentifier = @"blender.timeline.frame";
    [_knob.contentView addSubview:_knobLabel];
    [self addSubview:_knob];
    /* Grabbing the knob gives a tap, and each frame it passes a tick, like a picker wheel. */
    _ticks = [[UISelectionFeedbackGenerator alloc] init];
    _grab = [[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleLight];

    UIPanGestureRecognizer *pan = [[[UIPanGestureRecognizer alloc] initWithTarget:self
                                                                           action:@selector(pan:)] autorelease];
    pan.maximumNumberOfTouches = 1;
    [self addGestureRecognizer:pan];
    [self addGestureRecognizer:[[[UITapGestureRecognizer alloc] initWithTarget:self
                                                                        action:@selector(tap:)] autorelease]];
    [self updateKnob];
  }
  return self;
}

- (void)dealloc
{
  [_knob release];
  [_knobLabel release];
  [_ticks release];
  [_grab release];
  [_keys release];
  [_scrub release];
  [_selectKeys release];
  [_moveKeys release];
  [super dealloc];
}

- (NSInteger)playhead { return _playhead; }
- (BOOL)scrubbing { return _scrubbing; }

- (void)setKeys:(NSArray *)keys
{
  if ([_keys isEqual:keys]) {
    return;
  }
  [_keys release];
  _keys = [keys copy];
  [self setNeedsDisplay];
}

- (void)setPlayheadFromBlender:(NSInteger)frame
{
  if (_scrubbing || frame == _playhead) {
    return;
  }
  _playhead = frame;
  [self updateKnob];
  [self setNeedsDisplay];
}

/* The bottom band holds the track and the knob; the key area sits above it when expanded. */
- (CGFloat)band
{
  return self.expanded ? kKnobBandHeight : self.bounds.size.height;
}

- (CGFloat)trackY
{
  return self.bounds.size.height - [self band] / 2;
}

- (CGFloat)keysY
{
  return 14 + (self.bounds.size.height - [self band] - 14) / 2;
}

/* Frames map to whole-frame cells, inset so the knob stays inside at the first and last frame. */
- (CGFloat)xForFrame:(CGFloat)frame
{
  const CGFloat inset = kKnobSize / 2 + 4, cells = MAX(1, self.end - self.start + 1);
  return inset + (frame - self.start + 0.5) / cells * MAX(1, self.bounds.size.width - 2 * inset);
}

- (NSInteger)frameAtX:(CGFloat)x
{
  const CGFloat inset = kKnobSize / 2 + 4, cells = MAX(1, self.end - self.start + 1);
  const NSInteger frame =
      self.start + NSInteger(floor((x - inset) / MAX(1, self.bounds.size.width - 2 * inset) * cells));
  return MIN(self.end, MAX(self.start, frame));
}

/* Expanded, the space between the ruler and the knob's band belongs to the keys. */
- (BOOL)inKeyArea:(CGPoint)point
{
  return self.expanded && point.y >= 16 && point.y <= self.bounds.size.height - [self band] + 4;
}

- (NSInteger)keyNear:(CGPoint)point
{
  if (![self inKeyArea:point]) {
    return NSNotFound;
  }
  NSInteger best = NSNotFound;
  CGFloat distance = 22;
  for (NSDictionary *key in self.keys) {
    const NSInteger frame = [key[@"f"] integerValue];
    const CGFloat d = fabs([self xForFrame:frame] - point.x);
    if (frame >= self.start && frame <= self.end && d < distance) {
      distance = d;
      best = frame;
    }
  }
  return best;
}

- (void)layoutSubviews
{
  [super layoutSubviews];
  [self updateKnob];
}

- (void)updateKnob
{
  _knobLabel.text = [NSString stringWithFormat:@"%ld", (long)_playhead];
  const CGFloat width = MAX(kKnobMinWidth, ceil([_knobLabel.text sizeWithAttributes:@{
                          NSFontAttributeName: _knobLabel.font}].width) + 20);
  _knob.bounds = CGRectMake(0, 0, width, kKnobSize);
  _knob.center = CGPointMake([self xForFrame:_playhead], [self trackY]);
  _knobLabel.frame = _knob.bounds;
  self.accessibilityValue = _knobLabel.text;
}

- (void)drawRect:(CGRect)rect
{
  const CGFloat width = self.bounds.size.width, track_y = [self trackY];
  /* Track: the playback range, orange when it is the preview range, as in Blender. */
  UIColor *range = self.preview ? UIColor.systemOrangeColor : UIColor.tertiaryLabelColor;
  [range setFill];
  /* The knob is a glass layer above the track, which simply runs behind it. */
  const CGRect knob = _knob.frame;
  UIRectFill(CGRectMake([self xForFrame:self.start] - 2, track_y - 1.5,
                        [self xForFrame:self.end] - [self xForFrame:self.start] + 4, 3));
  if (!self.expanded) {
    /* Keyed frames along the track, behind the knob like the track itself. */
    for (NSDictionary *key in self.keys) {
      const NSInteger frame = [key[@"f"] integerValue];
      const CGFloat x = [self xForFrame:frame];
      if (frame < self.start || frame > self.end) {
        continue;
      }
      blenderFillDiamond(CGPointMake(x, track_y), 9, blenderKeyColor([key[@"s"] boolValue]),
                         [UIColor colorWithWhite:0 alpha:0.55]);
    }
    return;
  }
  /* Ruler: label every frame step that leaves room for its number. */
  const CGFloat area_bottom = self.bounds.size.height - [self band];
  const CGFloat per_frame = MAX(0.001, [self xForFrame:self.start + 1] - [self xForFrame:self.start]);
  NSInteger step = 1;
  for (NSInteger candidate : {1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500, 1000, 2500, 5000}) {
    step = candidate;
    if (per_frame * candidate >= 44) {
      break;
    }
  }
  NSDictionary *attributes = @{
    NSFontAttributeName: [UIFont monospacedDigitSystemFontOfSize:10 weight:UIFontWeightRegular],
    NSForegroundColorAttributeName: UIColor.secondaryLabelColor,
  };
  [[UIColor.secondaryLabelColor colorWithAlphaComponent:0.3] setFill];
  for (NSInteger frame = NSInteger(ceil(double(self.start) / step)) * step; frame <= self.end; frame += step) {
    const CGFloat x = [self xForFrame:frame];
    UIRectFill(CGRectMake(x - 0.5, 14, 1, area_bottom - 14));
    NSString *label = [NSString stringWithFormat:@"%ld", (long)frame];
    const CGSize size = [label sizeWithAttributes:attributes];
    [label drawAtPoint:CGPointMake(MIN(width - size.width - 2, MAX(2, x - size.width / 2)), 1)
        withAttributes:attributes];
  }
  /* The playhead line rises from the knob through the keys. */
  const CGFloat playhead = [self xForFrame:_playhead];
  [UIColor.systemBlueColor setFill];
  UIRectFill(CGRectMake(playhead - 1, 12, 2, CGRectGetMinY(knob) - 12));
  /* The selection box spans the key track between the fingers' frames. */
  const CGFloat box_left = MIN(_boxFrom, _boxTo), box_right = MAX(_boxFrom, _boxTo);
  if (_boxing) {
    UIBezierPath *box = [UIBezierPath bezierPathWithRoundedRect:CGRectMake(box_left, 16, box_right - box_left,
                                                                           area_bottom - 18)
                                                   cornerRadius:6];
    [[UIColor.systemBlueColor colorWithAlphaComponent:0.15] setFill];
    [box fill];
    box.lineWidth = 1;
    [[UIColor.systemBlueColor colorWithAlphaComponent:0.7] setStroke];
    [box stroke];
  }
  /* Keys. Moving, the selected keys show at their new frames and faded at their old ones; boxing,
   * the keys inside the box show as they will be selected. */
  const CGFloat keys_y = [self keysY];
  UIColor *rim = [UIColor.systemBackgroundColor colorWithAlphaComponent:0.7];
  for (NSDictionary *key in self.keys) {
    const NSInteger frame = [key[@"f"] integerValue];
    const CGFloat x = [self xForFrame:frame];
    if (frame < self.start || frame > self.end) {
      continue;
    }
    const BOOL selected = _boxing ? (x >= box_left && x <= box_right) : [key[@"s"] boolValue];
    UIColor *color = blenderKeyColor(selected);
    if (_movingKeys && selected) {
      color = [color colorWithAlphaComponent:0.3];
    }
    blenderFillDiamond(CGPointMake(x, keys_y), 16, color, rim);
  }
  if (_movingKeys) {
    for (NSDictionary *key in self.keys) {
      const NSInteger frame = [key[@"f"] integerValue];
      if ([key[@"s"] boolValue] && frame != _grabbed) {
        blenderFillDiamond(CGPointMake([self xForFrame:frame + _delta], keys_y), 16, UIColor.systemOrangeColor, rim);
      }
    }
    const CGPoint center = CGPointMake([self xForFrame:_grabbed + _delta], keys_y);
    blenderFillDiamond(center, 20, UIColor.systemOrangeColor, UIColor.systemBackgroundColor);
    NSString *label = [NSString stringWithFormat:@"%ld", (long)(_grabbed + _delta)];
    NSDictionary *bold = @{
      NSFontAttributeName: [UIFont monospacedDigitSystemFontOfSize:11 weight:UIFontWeightSemibold],
      NSForegroundColorAttributeName: UIColor.labelColor,
    };
    const CGSize size = [label sizeWithAttributes:bold];
    [label drawAtPoint:CGPointMake(MIN(width - size.width - 2, MAX(2, center.x + 12)), center.y - size.height / 2)
        withAttributes:bold];
  }
}

- (void)scrubTo:(NSInteger)frame
{
  if (frame != _playhead) {
    if (_scrubbing) {
      [_ticks selectionChanged];
      [_ticks prepare];
    }
    _playhead = frame;
    [self updateKnob];
    [self setNeedsDisplay];
  }
  if (frame != _sent) {
    _sent = frame;
    if (self.scrub) {
      self.scrub(frame);
    }
  }
}

- (NSArray<NSNumber *> *)framesFrom:(CGFloat)left to:(CGFloat)right
{
  NSMutableArray *frames = [NSMutableArray array];
  for (NSDictionary *key in self.keys) {
    const NSInteger frame = [key[@"f"] integerValue];
    const CGFloat x = [self xForFrame:frame];
    if (frame >= self.start && frame <= self.end && x >= MIN(left, right) && x <= MAX(left, right)) {
      [frames addObject:@(frame)];
    }
  }
  return frames;
}

- (BOOL)isSelected:(NSInteger)frame
{
  for (NSDictionary *key in self.keys) {
    if ([key[@"f"] integerValue] == frame) {
      return [key[@"s"] boolValue];
    }
  }
  return NO;
}

- (void)tap:(UITapGestureRecognizer *)gesture
{
  const CGPoint point = [gesture locationInView:self];
  /* In the key track a tap selects the key under it, or clears the selection. */
  if ([self inKeyArea:point]) {
    const NSInteger key = [self keyNear:point];
    if (self.selectKeys) {
      self.selectKeys(key != NSNotFound ? @[@(key)] : @[]);
    }
    return;
  }
  _sent = NSIntegerMin;
  [self scrubTo:[self frameAtX:point.x]];
}

- (void)pan:(UIPanGestureRecognizer *)gesture
{
  const CGPoint point = [gesture locationInView:self];
  const BOOL finished = gesture.state >= UIGestureRecognizerStateEnded;
  if (gesture.state == UIGestureRecognizerStateBegan) {
    const CGPoint translation = [gesture translationInView:self];
    const CGPoint origin = CGPointMake(point.x - translation.x, point.y - translation.y);
    const NSInteger key = [self keyNear:origin];
    /* Grabbing a key drags the selection, selecting the key first unless it is already part of
     * it; empty key track draws a selection box; anywhere else scrubs. */
    _movingKeys = key != NSNotFound;
    _boxing = !_movingKeys && [self inKeyArea:origin];
    _scrubbing = !_movingKeys && !_boxing;
    _grabbed = key;
    _delta = 0;
    _boxFrom = _boxTo = origin.x;
    _sent = NSIntegerMin;
    if (_movingKeys && ![self isSelected:key] && self.selectKeys) {
      self.selectKeys(@[@(key)]);
    }
    if (!_boxing) {
      [_grab impactOccurred];
    }
    [_ticks prepare];
    if (_scrubbing) {
      [self liftKnob:YES];
    }
  }
  if (_movingKeys) {
    const NSInteger delta = [self frameAtX:point.x] - _grabbed;
    if (delta != _delta && !finished) {
      _delta = delta;
      [_ticks selectionChanged];
      [_ticks prepare];
    }
    if (gesture.state == UIGestureRecognizerStateEnded && _delta != 0 && self.moveKeys) {
      self.moveKeys(_delta);
    }
    if (finished) {
      _movingKeys = NO;
    }
    [self setNeedsDisplay];
    return;
  }
  if (_boxing) {
    _boxTo = point.x;
    if (gesture.state == UIGestureRecognizerStateEnded && self.selectKeys) {
      self.selectKeys([self framesFrom:_boxFrom to:_boxTo]);
    }
    if (finished) {
      _boxing = NO;
    }
    [self setNeedsDisplay];
    return;
  }
  if (gesture.state <= UIGestureRecognizerStateEnded) {
    [self scrubTo:[self frameAtX:point.x]];
  }
  if (finished) {
    _scrubbing = NO;
    [self liftKnob:NO];
  }
}

/* Held, the knob turns to clear glass and swells, as system slider thumbs do; released, it
 * settles back into the playhead's tinted glass. */
- (void)liftKnob:(BOOL)lifted
{
  UIGlassEffect *glass = [UIGlassEffect effectWithStyle:lifted ? UIGlassEffectStyleClear : UIGlassEffectStyleRegular];
  glass.interactive = YES;
  glass.tintColor = lifted ? nil : UIColor.systemBlueColor;
  UIVisualEffectView *knob = _knob;
  UILabel *label = _knobLabel;
  [UIView animateWithDuration:UIAccessibilityIsReduceMotionEnabled() ? 0 : 0.35
                        delay:0
       usingSpringWithDamping:0.6
        initialSpringVelocity:0
                      options:UIViewAnimationOptionBeginFromCurrentState | UIViewAnimationOptionAllowUserInteraction
                   animations:^{
                     knob.effect = glass;
                     label.textColor = lifted ? UIColor.labelColor : UIColor.whiteColor;
                     knob.transform = lifted ? CGAffineTransformMakeScale(1.2, 1.2) : CGAffineTransformIdentity;
                   }
                   completion:nil];
}

- (void)accessibilityIncrement
{
  _sent = NSIntegerMin;
  [self scrubTo:MIN(self.end, _playhead + 1)];
}

- (void)accessibilityDecrement
{
  _sent = NSIntegerMin;
  [self scrubTo:MAX(self.start, _playhead - 1)];
}
@end

@implementation GHOSTIOSTimeline {
  UIView *_overlay;
  UIBarButtonItem *_button;
  UIVisualEffectView *_panel;
  UIButton *_play, *_jumpStart, *_jumpEnd, *_record, *_keysButton, *_deleteKeys;
  BOOL _recording;
  GHOSTIOSScrubber *_scrubber;
  NSDictionary *_snapshot;
  BOOL _visible;
  BOOL _expanded;
  NSArray<NSLayoutConstraint *> *_collapsedLayout, *_expandedLayout;
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
    __block GHOSTIOSTimeline *owner = self;
    [_record addAction:[UIAction actionWithHandler:^(__kindof UIAction *action) {
      /* Show the new state at once; Blender's snapshot confirms it. */
      [owner setRecording:!owner->_recording];
    }] forControlEvents:UIControlEventTouchUpInside];
    UIButton *start = _jumpStart = [self transportButton:@"backward.end.fill" label:@"Jump to Start" command:@"start"];
    UIButton *previous = [self transportButton:@"backward.frame.fill" label:@"Previous Keyframe" command:@"prev_key"];
    _play = [self transportButton:@"play.fill" label:@"Play" command:@"play"];
    UIButton *next = [self transportButton:@"forward.frame.fill" label:@"Next Keyframe" command:@"next_key"];
    UIButton *end = _jumpEnd = [self transportButton:@"forward.end.fill" label:@"Jump to End" command:@"end"];

    _scrubber = [[GHOSTIOSScrubber alloc] initWithFrame:CGRectZero];
    _scrubber.translatesAutoresizingMaskIntoConstraints = NO;
    _scrubber.scrub = ^(NSInteger frame) {
      NSString *value = [NSString stringWithFormat:@"%ld", (long)frame];
      WM_apple_ios_ui_setting_enqueue("timeline_frame", value.UTF8String);
    };
    _scrubber.selectKeys = ^(NSArray<NSNumber *> *frames) {
      [owner selectKeyFrames:frames];
    };
    _scrubber.moveKeys = ^(NSInteger delta) {
      [owner moveSelectedKeysBy:delta];
    };

    /* Deletes the selected keys; shown while the key track has a selection. */
    _deleteKeys = [UIButton buttonWithType:UIButtonTypeSystem];
    [_deleteKeys setImage:[UIImage systemImageNamed:@"trash"] forState:UIControlStateNormal];
    _deleteKeys.tintColor = UIColor.systemRedColor;
    _deleteKeys.accessibilityLabel = @"Delete Keyframes";
    _deleteKeys.accessibilityIdentifier = @"blender.timeline.delete-keys";
    _deleteKeys.translatesAutoresizingMaskIntoConstraints = NO;
    _deleteKeys.hidden = YES;
    [_deleteKeys addAction:[UIAction actionWithHandler:^(__kindof UIAction *action) {
      [owner deleteSelectedKeys];
    }] forControlEvents:UIControlEventTouchUpInside];

    /* Opens the key track, to move keyframes by dragging them. */
    _keysButton = [UIButton buttonWithType:UIButtonTypeSystem];
    _keysButton.tintColor = UIColor.labelColor;
    _keysButton.accessibilityLabel = @"Edit Keyframes";
    _keysButton.accessibilityIdentifier = @"blender.timeline.keys-toggle";
    _keysButton.translatesAutoresizingMaskIntoConstraints = NO;
    [_keysButton addAction:[UIAction actionWithHandler:^(__kindof UIAction *action) {
      [owner toggleExpanded:action.sender];
    }] forControlEvents:UIControlEventTouchUpInside];
    [self updateKeysButtonImage];

    UIStackView *transport = [[[UIStackView alloc]
        initWithArrangedSubviews:@[_record, start, previous, _play, next, end]] autorelease];
    transport.spacing = 0;
    [transport setCustomSpacing:4 afterView:_record];
    transport.translatesAutoresizingMaskIntoConstraints = NO;
    UIView *content = _panel.contentView;
    [content addSubview:_scrubber];
    [content addSubview:transport];
    [content addSubview:_keysButton];
    [content addSubview:_deleteKeys];
    /* Collapsed, one row: the controls, the scrubber between them, and the keyframe button.
     * Expanded, the controls center in a slim bottom row, between Delete and the keyframe button,
     * and the scrubber takes the full width above, its key area over the knob it shares a frame
     * mapping with. */
    [NSLayoutConstraint activateConstraints:@[
      [_deleteKeys.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:8],
      [_deleteKeys.centerYAnchor constraintEqualToAnchor:transport.centerYAnchor],
      [_deleteKeys.widthAnchor constraintEqualToConstant:44],
      [_deleteKeys.heightAnchor constraintEqualToConstant:44],
      [_keysButton.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-8],
      [_keysButton.centerYAnchor constraintEqualToAnchor:transport.centerYAnchor],
      [_keysButton.widthAnchor constraintEqualToConstant:44],
      [_keysButton.heightAnchor constraintEqualToConstant:44],
      [_scrubber.topAnchor constraintEqualToAnchor:content.topAnchor],
    ]];
    _collapsedLayout = [@[
      [transport.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:8],
      [transport.centerYAnchor constraintEqualToAnchor:content.bottomAnchor constant:-kTimelineHeight / 2],
      [_scrubber.leadingAnchor constraintEqualToAnchor:transport.trailingAnchor constant:4],
      [_scrubber.trailingAnchor constraintEqualToAnchor:_keysButton.leadingAnchor constant:-2],
      [_scrubber.bottomAnchor constraintEqualToAnchor:content.bottomAnchor],
    ] retain];
    _expandedLayout = [@[
      [transport.centerXAnchor constraintEqualToAnchor:content.centerXAnchor],
      [transport.centerYAnchor constraintEqualToAnchor:content.bottomAnchor constant:-kControlsRowHeight / 2],
      [_scrubber.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:8],
      [_scrubber.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-8],
      [_scrubber.bottomAnchor constraintEqualToAnchor:content.bottomAnchor constant:-kControlsRowHeight],
    ] retain];
    [NSLayoutConstraint activateConstraints:_collapsedLayout];
    [_overlay addSubview:_panel];
    [self updateSnapshot:@{@"frame": @1, @"start": @1, @"end": @250, @"playing": @NO, @"autokey": @NO,
                           @"keys": @[]}];
  }
  return self;
}

- (void)dealloc
{
  [_panel removeFromSuperview];
  [_panel release];
  [_button release];
  [_scrubber release];
  [_collapsedLayout release];
  [_expandedLayout release];
  [_snapshot release];
  [_visibilityDidChange release];
  [super dealloc];
}

- (UIBarButtonItem *)button { return _button; }
- (UIView *)view { return _panel; }
- (BOOL)visible { return _visible; }
- (BOOL)expanded { return _expanded; }
- (CGFloat)height
{
  return _expanded ? kControlsRowHeight + kKnobBandHeight + kKeyAreaHeight : kTimelineHeight;
}

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

- (void)updateSnapshot:(NSDictionary *)snapshot
{
  if (![snapshot isKindOfClass:NSDictionary.class] || [_snapshot isEqual:snapshot]) {
    return;
  }
  [_snapshot release];
  _snapshot = [snapshot copy];
  const NSInteger start = [snapshot[@"start"] integerValue];
  const NSInteger end = MAX(start, [snapshot[@"end"] integerValue]);
  const BOOL playing = [snapshot[@"playing"] boolValue];
  if (_scrubber.start != start || _scrubber.end != end || _scrubber.preview != [snapshot[@"preview"] boolValue]) {
    _scrubber.start = start;
    _scrubber.end = end;
    _scrubber.preview = [snapshot[@"preview"] boolValue];
    [_scrubber setNeedsLayout];
    [_scrubber setNeedsDisplay];
  }
  [self showKeys:[snapshot[@"keys"] isKindOfClass:NSArray.class] ? snapshot[@"keys"] : @[]];
  /* Keep the knob under the finger; Blender's frame catches up through the inbox. */
  [_scrubber setPlayheadFromBlender:[snapshot[@"frame"] integerValue]];
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

- (void)showKeys:(NSArray *)keys
{
  _scrubber.keys = keys;
  BOOL selected = NO;
  for (NSDictionary *key in keys) {
    selected = selected || [key[@"s"] boolValue];
  }
  _deleteKeys.hidden = !(_expanded && selected);
}

static NSArray *sortedKeys(NSDictionary<NSNumber *, NSNumber *> *keys)
{
  NSMutableArray *sorted = [NSMutableArray array];
  for (NSNumber *frame in [keys.allKeys sortedArrayUsingSelector:@selector(compare:)]) {
    [sorted addObject:@{@"f": frame, @"s": keys[frame]}];
  }
  return sorted;
}

/* Each edit shows at once; Blender's snapshot confirms it. */
- (void)selectKeyFrames:(NSArray<NSNumber *> *)frames
{
  NSMutableArray *keys = [NSMutableArray array];
  for (NSDictionary *key in _scrubber.keys) {
    [keys addObject:@{@"f": key[@"f"], @"s": @([frames containsObject:key[@"f"]])}];
  }
  [self showKeys:keys];
  NSString *value = [[frames componentsJoinedByString:@","] stringByAppendingString:@"|0"];
  WM_apple_ios_ui_setting_enqueue("timeline_key_select", value.UTF8String);
}

- (void)moveSelectedKeysBy:(NSInteger)delta
{
  /* Moved keys replace the unselected keys where they land, as in Blender. */
  NSMutableDictionary *keys = [NSMutableDictionary dictionary];
  for (NSDictionary *key in _scrubber.keys) {
    if (![key[@"s"] boolValue] && keys[key[@"f"]] == nil) {
      keys[key[@"f"]] = @NO;
    }
  }
  for (NSDictionary *key in _scrubber.keys) {
    if ([key[@"s"] boolValue]) {
      keys[@([key[@"f"] integerValue] + delta)] = @YES;
    }
  }
  [self showKeys:sortedKeys(keys)];
  WM_apple_ios_ui_setting_enqueue("timeline_key_move", [NSString stringWithFormat:@"%ld", (long)delta].UTF8String);
}

- (void)deleteSelectedKeys
{
  NSMutableArray *keys = [NSMutableArray array];
  for (NSDictionary *key in _scrubber.keys) {
    if (![key[@"s"] boolValue]) {
      [keys addObject:key];
    }
  }
  [self showKeys:keys];
  WM_apple_ios_ui_setting_enqueue("timeline_key_delete", "");
}

/* Blender's keyframe icon, filled while the key track is open. */
- (void)updateKeysButtonImage
{
  UIImage *icon = [[UIImage imageNamed:_expanded ? @"blender.keyframe_hlt" : @"blender.keyframe"]
      imageWithRenderingMode:UIImageRenderingModeAlwaysTemplate];
  [_keysButton setImage:icon ?: [UIImage systemImageNamed:_expanded ? @"diamond.fill" : @"diamond"]
               forState:UIControlStateNormal];
}

- (void)toggleExpanded:(id)sender
{
  _expanded = !_expanded;
  _scrubber.expanded = _expanded;
  [NSLayoutConstraint deactivateConstraints:_expanded ? _collapsedLayout : _expandedLayout];
  [NSLayoutConstraint activateConstraints:_expanded ? _expandedLayout : _collapsedLayout];
  [_scrubber setNeedsLayout];
  [_scrubber setNeedsDisplay];
  [self showKeys:_scrubber.keys];
  _keysButton.tintColor = _expanded ? UIColor.systemBlueColor : UIColor.labelColor;
  _keysButton.accessibilityTraits = UIAccessibilityTraitButton | (_expanded ? UIAccessibilityTraitSelected : 0);
  [self updateKeysButtonImage];
  _panel.layer.cornerRadius = _expanded ? 24 : kTimelineHeight / 2;
  if (_visibilityDidChange) {
    _visibilityDidChange();
  }
}

- (void)toggle:(id)sender
{
  _visible = !_visible;
  _button.selected = _visible;
  /* Blender lists keyframes only while the timeline shows them. */
  WM_apple_ios_ui_setting_enqueue("timeline_visible", _visible ? "1" : "0");
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
  const CGFloat width = MAX(0, rect.size.width);
  const CGFloat height = self.height;
  /* Frame is undefined while transformed; layout must not disturb the presentation animation. */
  _panel.bounds = CGRectMake(0, 0, width, height);
  /* Phones keep play, keyframe steps and the scrubber. */
  _jumpStart.hidden = _jumpEnd.hidden = width < 520;
  _panel.center = CGPointMake(CGRectGetMidX(rect), CGRectGetMaxY(rect) - height / 2);
}

@end
