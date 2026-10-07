/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#import "GHOST_IOSNumericInput.hh"
#import <objc/runtime.h>

static char GHOSTIOSNumberHoveredKey;
static char GHOSTIOSNumberScrubbingKey;
static char GHOSTIOSNumberChevronsKey;
static char GHOSTIOSNumberAnimationKey;

static const CGFloat GHOSTIOSScrubGaugeHeight = 24;

/* Floating glass pill shown above a field while it is scrubbed. It tells the user that vertical
 * movement changes the drag speed, then shows the current multiplier on a small gauge. */
@interface GHOSTIOSNumberScrubIndicator : UIVisualEffectView
@property(nonatomic, assign) UITextField *field;
@end

@implementation GHOSTIOSNumberScrubIndicator {
  UILabel *_label;
  UIView *_track;
  UIView *_dot;
  NSLayoutConstraint *_dotOffset;
}

+ (instancetype)shared
{
  static GHOSTIOSNumberScrubIndicator *indicator = nil;
  if (!indicator) {
    indicator = [[GHOSTIOSNumberScrubIndicator alloc] init];
  }
  return indicator;
}

- (instancetype)init
{
  UIGlassEffect *glass = [UIGlassEffect effectWithStyle:UIGlassEffectStyleRegular];
  if ((self = [super initWithEffect:glass])) {
    self.userInteractionEnabled = NO;
    self.clipsToBounds = YES;
    self.layer.cornerCurve = kCACornerCurveContinuous;
    self.accessibilityIdentifier = @"blender.number.scrub-indicator";

    UIImageSymbolConfiguration *symbol = [UIImageSymbolConfiguration
        configurationWithPointSize:12 weight:UIImageSymbolWeightSemibold];
    UIImageView *icon = [[[UIImageView alloc] initWithImage:
        [UIImage systemImageNamed:@"arrow.up.and.down" withConfiguration:symbol]] autorelease];
    icon.tintColor = UIColor.secondaryLabelColor;

    _label = [[UILabel alloc] init];
    _label.font = [UIFont monospacedDigitSystemFontOfSize:13 weight:UIFontWeightMedium];
    _label.textColor = UIColor.labelColor;

    _track = [[UIView alloc] init];
    _track.backgroundColor = UIColor.tertiarySystemFillColor;
    _track.layer.cornerRadius = 2;
    _track.translatesAutoresizingMaskIntoConstraints = NO;
    UIView *center = [[[UIView alloc] init] autorelease];
    center.backgroundColor = UIColor.tertiaryLabelColor;
    center.translatesAutoresizingMaskIntoConstraints = NO;
    [_track addSubview:center];
    _dot = [[UIView alloc] init];
    _dot.layer.cornerRadius = 4;
    _dot.translatesAutoresizingMaskIntoConstraints = NO;
    [_track addSubview:_dot];
    _dotOffset = [[_dot.centerYAnchor constraintEqualToAnchor:_track.centerYAnchor] retain];
    [NSLayoutConstraint activateConstraints:@[
      [_track.widthAnchor constraintEqualToConstant:4],
      [_track.heightAnchor constraintEqualToConstant:GHOSTIOSScrubGaugeHeight],
      [center.centerYAnchor constraintEqualToAnchor:_track.centerYAnchor],
      [center.centerXAnchor constraintEqualToAnchor:_track.centerXAnchor],
      [center.widthAnchor constraintEqualToConstant:8],
      [center.heightAnchor constraintEqualToConstant:1],
      [_dot.centerXAnchor constraintEqualToAnchor:_track.centerXAnchor],
      [_dot.widthAnchor constraintEqualToConstant:8],
      [_dot.heightAnchor constraintEqualToConstant:8],
      _dotOffset,
    ]];

    UIStackView *stack = [[[UIStackView alloc] initWithArrangedSubviews:@[icon, _label, _track]] autorelease];
    stack.axis = UILayoutConstraintAxisHorizontal;
    stack.alignment = UIStackViewAlignmentCenter;
    stack.spacing = 8;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [self.contentView addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[
      [stack.leadingAnchor constraintEqualToAnchor:self.contentView.leadingAnchor constant:12],
      [stack.trailingAnchor constraintEqualToAnchor:self.contentView.trailingAnchor constant:-12],
      [stack.topAnchor constraintEqualToAnchor:self.contentView.topAnchor constant:6],
      [stack.bottomAnchor constraintEqualToAnchor:self.contentView.bottomAnchor constant:-6],
    ]];
  }
  return self;
}

- (void)dealloc
{
  [_label release];
  [_track release];
  [_dot release];
  [_dotOffset release];
  [super dealloc];
}

- (void)showForField:(UITextField *)field vertical:(double)vertical
{
  UIWindow *window = field.window;
  if (!window) {
    [self hideForField:self.field];
    return;
  }
  /* Same mapping as #GHOSTIOSNumberSensitivity: up is faster, down is finer. */
  const double octaves = fmin(4.0, fmax(-4.0, -vertical / 80.0));
  const double factor = std::exp2(octaves);
  if (fabs(vertical) < 6) {
    _label.text = @"Up faster · Down finer";
  }
  else {
    NSString *amount = factor >= 1 ? [NSString stringWithFormat:@"×%.3g", factor] :
                                     [NSString stringWithFormat:@"×%.2g", factor];
    _label.text = [NSString stringWithFormat:@"%@ %@", amount,
                   fabs(octaves) < 0.15 ? @"Normal" : octaves > 0 ? @"Faster" : @"Finer"];
  }
  _dot.backgroundColor = fabs(octaves) < 0.15 ? UIColor.secondaryLabelColor : field.tintColor;
  _dotOffset.constant = -octaves / 4.0 * (GHOSTIOSScrubGaugeHeight - 8) / 2;

  const BOOL appearing = self.field != field || self.superview != window;
  self.field = field;
  if (self.superview != window) {
    [window addSubview:self];
  }
  [window bringSubviewToFront:self];
  CGSize size = [self systemLayoutSizeFittingSize:UILayoutFittingCompressedSize];
  size.width = MAX(size.width, 120);
  self.layer.cornerRadius = size.height / 2;
  /* Sit above the field, clear of the hand holding the Pencil; flip below near the top. */
  CGRect anchor = [field convertRect:field.bounds toView:window];
  const UIEdgeInsets safe = window.safeAreaInsets;
  CGFloat y = CGRectGetMinY(anchor) - 10 - size.height;
  if (y < safe.top + 4) {
    y = CGRectGetMaxY(anchor) + 10;
  }
  CGFloat x = CGRectGetMidX(anchor) - size.width / 2;
  x = MAX(safe.left + 8, MIN(x, window.bounds.size.width - safe.right - 8 - size.width));
  if (appearing) {
    self.frame = CGRectMake(x, y, size.width, size.height);
    self.alpha = 0;
    self.transform = CGAffineTransformMakeScale(0.9, 0.9);
    [UIView animateWithDuration:UIAccessibilityIsReduceMotionEnabled() ? 0 : 0.18
                     animations:^{
                       self.alpha = 1;
                       self.transform = CGAffineTransformIdentity;
                     }];
  }
  else {
    self.bounds = CGRectMake(0, 0, size.width, size.height);
    self.center = CGPointMake(x + size.width / 2, y + size.height / 2);
  }
  [self layoutIfNeeded];
}

- (void)hideForField:(UITextField *)field
{
  if (!field || self.field != field) {
    return;
  }
  self.field = nil;
  [UIView animateWithDuration:UIAccessibilityIsReduceMotionEnabled() ? 0 : 0.15
      animations:^{ self.alpha = 0; }
      completion:^(BOOL) {
        if (!self.field) {
          [self removeFromSuperview];
        }
      }];
}
@end

@interface GHOSTIOSNumberHoverTarget : NSObject
@end

@implementation GHOSTIOSNumberHoverTarget
- (void)hover:(UIHoverGestureRecognizer *)hover
{
  UITextField *field = (UITextField *)hover.view;
  const BOOL inside = hover.state == UIGestureRecognizerStateBegan ||
                      hover.state == UIGestureRecognizerStateChanged;
  if ([objc_getAssociatedObject(field, &GHOSTIOSNumberHoveredKey) boolValue] == inside) {
    return;
  }
  objc_setAssociatedObject(field, &GHOSTIOSNumberHoveredKey, @(inside), OBJC_ASSOCIATION_RETAIN_NONATOMIC);
  GHOSTIOSNumberFieldAppearance(
      field, [objc_getAssociatedObject(field, &GHOSTIOSNumberScrubbingKey) boolValue]);
}
@end

void GHOSTIOSNumberFieldInstallHover(UITextField *field)
{
  if (objc_getAssociatedObject(field, &GHOSTIOSNumberChevronsKey)) {
    return;
  }
  static GHOSTIOSNumberHoverTarget *target = [[GHOSTIOSNumberHoverTarget alloc] init];
  UIImageSymbolConfiguration *symbol = [UIImageSymbolConfiguration
      configurationWithPointSize:9 weight:UIImageSymbolWeightBold];
  NSMutableArray *chevrons = [NSMutableArray array];
  for (NSString *name in @[@"chevron.left", @"chevron.right"]) {
    /* Keep the space reserved so the value does not shift when the chevrons appear. */
    UIImageView *chevron = [[[UIImageView alloc] initWithImage:
        [UIImage systemImageNamed:name withConfiguration:symbol]] autorelease];
    chevron.frame = CGRectMake(0, 0, 14, 20);
    chevron.contentMode = UIViewContentModeCenter;
    chevron.tintColor = UIColor.secondaryLabelColor;
    chevron.alpha = 0;
    [chevrons addObject:chevron];
  }
  field.leftView = chevrons[0];
  field.rightView = chevrons[1];
  field.leftViewMode = UITextFieldViewModeAlways;
  field.rightViewMode = UITextFieldViewModeAlways;
  objc_setAssociatedObject(field, &GHOSTIOSNumberChevronsKey, chevrons, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
  UIHoverGestureRecognizer *hover = [[[UIHoverGestureRecognizer alloc]
      initWithTarget:target action:@selector(hover:)] autorelease];
  hover.name = @"blender.number.hover";
  [field addGestureRecognizer:hover];
}

void GHOSTIOSNumberFieldAppearance(UITextField *field, bool scrubbing)
{
  const bool active = field.isFirstResponder || scrubbing;
  const bool hovered = !active && field.enabled &&
                       [objc_getAssociatedObject(field, &GHOSTIOSNumberHoveredKey) boolValue];
  objc_setAssociatedObject(field, &GHOSTIOSNumberScrubbingKey, @(scrubbing), OBJC_ASSOCIATION_RETAIN_NONATOMIC);
  field.borderStyle = UITextBorderStyleRoundedRect;
  UIColor *animation = objc_getAssociatedObject(field, &GHOSTIOSNumberAnimationKey);
  field.backgroundColor = active    ? [field.tintColor colorWithAlphaComponent:0.12] :
                          animation ? [animation colorWithAlphaComponent:hovered ? 0.45 : 0.35] :
                          hovered   ? [field.tintColor colorWithAlphaComponent:0.07] :
                                      UIColor.tertiarySystemFillColor;
  field.layer.cornerRadius = 8;
  field.layer.cornerCurve = kCACornerCurveContinuous;
  field.layer.borderWidth = active ? 1.5 : hovered ? 1 : 0;
  field.layer.borderColor = (active ? field.tintColor : [field.tintColor colorWithAlphaComponent:0.35]).CGColor;
  field.textColor = field.enabled ? UIColor.labelColor : UIColor.tertiaryLabelColor;
  NSArray *chevrons = objc_getAssociatedObject(field, &GHOSTIOSNumberChevronsKey);
  [UIView animateWithDuration:UIAccessibilityIsReduceMotionEnabled() ? 0 : 0.12
                   animations:^{
                     for (UIView *chevron in chevrons) {
                       chevron.alpha = hovered ? 1 : 0;
                     }
                   }];
  GHOSTIOSNumberScrubIndicator *indicator = [GHOSTIOSNumberScrubIndicator shared];
  if (scrubbing) {
    if (indicator.field != field) {
      [indicator showForField:field vertical:0];
    }
  }
  else {
    [indicator hideForField:field];
  }
}

void GHOSTIOSNumberFieldSetAnimationColor(UITextField *field, UIColor *color)
{
  UIColor *current = objc_getAssociatedObject(field, &GHOSTIOSNumberAnimationKey);
  if (current == color || [current isEqual:color]) {
    return;
  }
  objc_setAssociatedObject(field, &GHOSTIOSNumberAnimationKey, color, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
  GHOSTIOSNumberFieldAppearance(
      field, [objc_getAssociatedObject(field, &GHOSTIOSNumberScrubbingKey) boolValue]);
}

void GHOSTIOSNumberScrubIndicatorUpdate(UITextField *field, double vertical)
{
  if (std::isfinite(vertical)) {
    [[GHOSTIOSNumberScrubIndicator shared] showForField:field vertical:vertical];
  }
}
