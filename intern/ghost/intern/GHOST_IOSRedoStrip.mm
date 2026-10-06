/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#import "GHOST_IOSRedoStrip.hh"
#import "GHOST_IOSNumericInput.hh"
#import <objc/runtime.h>
#import <QuartzCore/QuartzCore.h>
#include <cmath>

extern "C" void WM_apple_ios_ui_setting_enqueue(const char *, const char *);

@interface GHOSTIOSRedoField : UITextField
@property(nonatomic, copy) NSDictionary *descriptor;
@property(nonatomic, copy) NSString *operationID;
@property(nonatomic, copy) NSString *originalText;
@end
@implementation GHOSTIOSRedoField
- (void)dealloc
{
  [_descriptor release];
  [_operationID release];
  [_originalText release];
  [super dealloc];
}
@end

@interface GHOSTIOSRedoSlider : UISlider
@property(nonatomic, copy) NSDictionary *descriptor;
@property(nonatomic, copy) NSString *operationID;
@property(nonatomic, copy) NSString *interactionID;
@property(nonatomic) double startValue;
@property(nonatomic) CFTimeInterval lastPreview;
@end
@implementation GHOSTIOSRedoSlider
- (void)dealloc
{
  [_descriptor release]; [_operationID release]; [_interactionID release];
  [super dealloc];
}
@end

@interface GHOSTIOSRedoStrip () <UITextFieldDelegate, UIColorPickerViewControllerDelegate, UIScrollViewDelegate>
@end

@implementation GHOSTIOSRedoStrip {
  UIView *_overlay;
  UIVisualEffectView *_rail;
  UIScrollView *_scroll;
  CAGradientLayer *_fadeMask;
  UIStackView *_stack;
  UIButton *_done;
  UIButton *_collapsedButton;
  NSDictionary *_snapshot;
  NSString *_expandedID;
  NSString *_lastError;
  NSArray *_shape;
  NSMutableDictionary *_controls;
  NSMutableDictionary *_pending;
  uint64_t _sequence;
  BOOL _visible;
  GHOSTIOSRedoField *_scrubField;
  CGPoint _scrubStart;
  double _scrubValue;
  double _scrubCurrentValue;
  CGFloat _scrubTranslation;
  NSString *_scrubInteraction;
  CFTimeInterval _scrubPreviewTime;
  UIImage *(^_icon)(NSString *);
  UIColorPickerViewController *_colorPicker;
  NSDictionary *_colorProperty;
  NSString *_colorOperationID;
  CFTimeInterval _colorPreviewTime;
}

- (instancetype)initWithOverlay:(UIView *)overlay icon:(UIImage *(^)(NSString *))icon
{
  if ((self = [super init])) {
    _overlay = overlay;
    _icon = [icon copy];
    _controls = [[NSMutableDictionary alloc] init];
    _pending = [[NSMutableDictionary alloc] init];
    UIGlassEffect *glass = [UIGlassEffect effectWithStyle:UIGlassEffectStyleRegular];
    glass.interactive = YES;
    _rail = [[UIVisualEffectView alloc] initWithEffect:glass];
    _rail.layer.cornerRadius = 20;
    _rail.clipsToBounds = YES;
    _rail.hidden = YES;
    _rail.accessibilityIdentifier = @"blender.redo.strip";
    _rail.tintColor = UIColor.systemBlueColor;
    [overlay addSubview:_rail];
    _scroll = [[[UIScrollView alloc] init] autorelease];
    _scroll.showsHorizontalScrollIndicator = NO;
    _scroll.alwaysBounceHorizontal = YES;
    _scroll.delegate = self;
    _scroll.keyboardDismissMode = UIScrollViewKeyboardDismissModeOnDrag;
    [_rail.contentView addSubview:_scroll];
    _fadeMask = [CAGradientLayer layer];
    _fadeMask.startPoint = CGPointMake(0, 0.5);
    _fadeMask.endPoint = CGPointMake(1, 0.5);
    _scroll.layer.mask = _fadeMask;
    _stack = [[[UIStackView alloc] init] autorelease];
    _stack.spacing = 14;
    _stack.alignment = UIStackViewAlignmentCenter;
    _stack.translatesAutoresizingMaskIntoConstraints = NO;
    [_scroll addSubview:_stack];
    [NSLayoutConstraint activateConstraints:@[
      [_stack.leadingAnchor constraintEqualToAnchor:_scroll.contentLayoutGuide.leadingAnchor constant:12],
      [_stack.trailingAnchor constraintEqualToAnchor:_scroll.contentLayoutGuide.trailingAnchor constant:-12],
      [_stack.topAnchor constraintEqualToAnchor:_scroll.contentLayoutGuide.topAnchor],
      [_stack.bottomAnchor constraintEqualToAnchor:_scroll.contentLayoutGuide.bottomAnchor],
      [_stack.heightAnchor constraintEqualToAnchor:_scroll.frameLayoutGuide.heightAnchor],
    ]];
    _done = [UIButton buttonWithType:UIButtonTypeSystem];
    UIButtonConfiguration *configuration = [UIButtonConfiguration plainButtonConfiguration];
    configuration.image = [UIImage systemImageNamed:@"checkmark"];
    _done.configuration = configuration;
    _done.accessibilityLabel = @"Collapse Last Operation";
    _done.accessibilityIdentifier = @"blender.redo.done";
    [_done addTarget:self action:@selector(dismiss:) forControlEvents:UIControlEventTouchUpInside];
    [_rail.contentView addSubview:_done];
    _collapsedButton = [[UIButton buttonWithType:UIButtonTypeSystem] retain];
    _collapsedButton.configuration = [UIButtonConfiguration glassButtonConfiguration];
    _collapsedButton.titleLabel.lineBreakMode = NSLineBreakByTruncatingTail;
    _collapsedButton.tintColor = UIColor.labelColor;
    _collapsedButton.accessibilityIdentifier = @"blender.redo.collapsed";
    _collapsedButton.hidden = YES;
    [_collapsedButton addTarget:self action:@selector(expand:) forControlEvents:UIControlEventTouchUpInside];
  }
  return self;
}

- (UIView *)view { return _rail; }
- (UIButton *)collapsedButton { return _collapsedButton; }
- (CGFloat)collapsedWidth
{
  if ([_snapshot[@"id"] isEqual:_expandedID] || ![_snapshot[@"properties"] count]) { return 0; }
  return MIN(136, MAX(72, [_collapsedButton sizeThatFits:CGSizeMake(CGFLOAT_MAX, 44)].width));
}

- (void)dealloc
{
  _colorPicker.delegate = nil;
  [_colorPicker dismissViewControllerAnimated:NO completion:nil];
  [_colorPicker release];
  [_colorProperty release];
  [_colorOperationID release];
  [_scrubInteraction release];
  [_rail removeFromSuperview];
  [_rail release];
  [_snapshot release];
  [_collapsedButton removeFromSuperview];
  [_collapsedButton release];
  [_expandedID release];
  [_presentationDidChange release];
  [_lastError release];
  [_shape release];
  [_controls release];
  [_pending release];
  [_icon release];
  [super dealloc];
}

- (void)layoutInRect:(CGRect)rect
{
  const CGFloat width = MIN(1000, MAX(0, rect.size.width));
  _rail.frame = CGRectMake(CGRectGetMidX(rect) - width / 2, CGRectGetMaxY(rect) - 60, width, 60);
  _scroll.frame = CGRectMake(0, 0, MAX(0, width - 48), 60);
  _done.frame = CGRectMake(MAX(0, width - 48), 6, 44, 48);
  [self scrollViewDidScroll:_scroll];
}

- (void)scrollViewDidScroll:(UIScrollView *)scroll
{
  CGFloat width = MAX(1, scroll.bounds.size.width);
  BOOL left = scroll.contentOffset.x > 1;
  BOOL right = scroll.contentSize.width - scroll.contentOffset.x > width + 1;
  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  _fadeMask.frame = scroll.bounds;
  _fadeMask.colors = @[(id)(left ? UIColor.clearColor : UIColor.blackColor).CGColor,
                        (id)UIColor.blackColor.CGColor, (id)UIColor.blackColor.CGColor,
                        (id)(right ? UIColor.clearColor : UIColor.blackColor).CGColor];
  _fadeMask.locations = @[@0, @(MIN(0.25, 12 / width)), @(MAX(0.75, 1 - 20 / width)), @1];
  [CATransaction commit];
}

- (void)sendProperty:(NSDictionary *)property value:(id)value operation:(NSString *)operation
{
  [self sendProperty:property value:value operation:operation interaction:nil phase:nil];
}

- (void)sendProperty:(NSDictionary *)property value:(id)value operation:(NSString *)operation
         interaction:(NSString *)interaction phase:(NSString *)phase
{
  if (value && !property) { return; }
  if (![operation isEqual:_snapshot[@"id"]]) { return; }
  NSMutableDictionary *request = [NSMutableDictionary dictionaryWithDictionary:
      @{@"id": operation, @"sequence": @(++_sequence)}];
  if (interaction) {
    request[@"interaction"] = interaction;
    request[@"phase"] = phase;
  }
  if (property) {
    request[@"key"] = property[@"key"];
    request[@"value"] = value;
    _pending[property[@"key"]] = @(_sequence);
  }
  else { request[@"action"] = @"dismiss"; }
  NSData *data = [NSJSONSerialization dataWithJSONObject:request options:0 error:nil];
  NSString *json = [[[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] autorelease];
  WM_apple_ios_ui_setting_enqueue("native_redo_edit", json.UTF8String);
}

- (void)dismiss:(id)sender
{
  [_rail endEditing:YES];
  [_expandedID release];
  _expandedID = nil;
  _collapsedButton.hidden = !self.collapsedWidth;
  if (_presentationDidChange) { _presentationDidChange(YES); }
  [self setVisible:NO];
}

- (void)expand:(id)sender
{
  if (!self.collapsedWidth) { return; }
  [_expandedID release];
  _expandedID = [_snapshot[@"id"] copy];
  _collapsedButton.hidden = YES;
  if (_presentationDidChange) { _presentationDidChange(YES); }
  [self setVisible:YES];
}

- (void)updateCollapsedTitle
{
  NSDictionary *shortNames = @{
    @"TRANSFORM_OT_translate": @"Move", @"TRANSFORM_OT_rotate": @"Rotate",
    @"TRANSFORM_OT_resize": @"Scale", @"MESH_OT_extrude_region_move": @"Extrude",
    @"MESH_OT_extrude_context_move": @"Extrude", @"MESH_OT_extrude_faces_move": @"Extrude",
    @"MESH_OT_duplicate_move": @"Duplicate", @"OBJECT_OT_duplicate_move": @"Duplicate",
    @"OBJECT_OT_duplicate_move_linked": @"Duplicate Linked",
  };
  UIButtonConfiguration *config = _collapsedButton.configuration;
  NSString *title = shortNames[_snapshot[@"operator"] ?: @""] ?: _snapshot[@"title"];
  if ([config.title isEqual:title]) { return; }
  config.title = title;
  config.titleLineBreakMode = NSLineBreakByTruncatingTail;
  config.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
  config.contentInsets = NSDirectionalEdgeInsetsMake(8, 12, 8, 12);
  _collapsedButton.configuration = config;
  _collapsedButton.titleLabel.numberOfLines = 1;
  _collapsedButton.accessibilityLabel = [NSString stringWithFormat:@"Adjust %@", _snapshot[@"title"]];
  _collapsedButton.accessibilityHint = @"Show last operation controls";
  _collapsedButton.toolTip = _collapsedButton.accessibilityLabel;
}

- (void)setVisible:(BOOL)visible
{
  if (_visible == visible) { return; }
  _visible = visible;
  if (!visible) { [_rail endEditing:YES]; }
  if (UIAccessibilityIsReduceMotionEnabled() || !UIView.areAnimationsEnabled) {
    _rail.alpha = visible ? 1 : 0;
    _rail.hidden = !visible;
    _rail.userInteractionEnabled = visible;
    return;
  }
  _rail.hidden = NO;
  [UIView animateWithDuration:UIAccessibilityIsReduceMotionEnabled() ? 0 : 0.2
      delay:0 options:UIViewAnimationOptionBeginFromCurrentState | UIViewAnimationOptionAllowUserInteraction
      animations:^{ self->_rail.alpha = visible ? 1 : 0; }
      completion:^(BOOL finished) { if (finished) { self->_rail.hidden = !visible; } }];
  _rail.userInteractionEnabled = visible;
}

- (UILabel *)label:(NSString *)text
{
  UILabel *label = [[[UILabel alloc] init] autorelease];
  label.font = [UIFont preferredFontForTextStyle:UIFontTextStyleCaption1];
  label.textColor = UIColor.secondaryLabelColor;
  label.text = text;
  return label;
}

- (NSDictionary *)propertyForKey:(NSString *)key
{
  for (NSDictionary *property in _snapshot[@"properties"]) {
    if ([property[@"key"] isEqual:key]) { return property; }
  }
  return nil;
}

- (void)rebuild
{
  _scrubField = nil;
  _scroll.scrollEnabled = YES;
  for (UIView *view in [[_stack.arrangedSubviews copy] autorelease]) {
    [_stack removeArrangedSubview:view];
    [view removeFromSuperview];
  }
  [_controls removeAllObjects];
  UILabel *title = [self label:_snapshot[@"title"]];
  title.font = [UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline];
  title.textColor = UIColor.labelColor;
  title.numberOfLines = 2;
  title.accessibilityIdentifier = @"blender.redo.title";
  [title.widthAnchor constraintEqualToConstant:112].active = YES;
  [_stack addArrangedSubview:title];
  __block GHOSTIOSRedoStrip *owner = self;
  NSString *operation = _snapshot[@"id"];
  for (NSDictionary *property in _snapshot[@"properties"]) {
    NSString *key = property[@"key"], *type = property[@"type"];
    NSString *name = property[@"group"] ? [NSString stringWithFormat:@"%@ %@", property[@"group"], property[@"label"]] : property[@"label"];
    UILabel *label = [self label:name];
    UIControl *control;
    CGFloat width = 110;
    if ([type isEqual:@"BOOLEAN"]) {
      UISwitch *toggle = [[[UISwitch alloc] init] autorelease];
      toggle.onTintColor = UIColor.systemBlueColor;
      [toggle addAction:[UIAction actionWithHandler:^(UIAction *action) {
        [owner sendProperty:[owner propertyForKey:key] value:@(((UISwitch *)action.sender).on) operation:operation];
      }] forControlEvents:UIControlEventValueChanged];
      control = toggle;
      width = MAX(60, MIN(180, [label sizeThatFits:CGSizeMake(CGFLOAT_MAX, 16)].width));
    }
    else if ([type isEqual:@"ENUM"] || [type isEqual:@"COLOR"]) {
      UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
      button.configuration = [UIButtonConfiguration plainButtonConfiguration];
      button.showsMenuAsPrimaryAction = [type isEqual:@"ENUM"];
      if ([type isEqual:@"COLOR"]) {
        [button addAction:[UIAction actionWithHandler:^(UIAction *action) {
          [owner showColor:[owner propertyForKey:key] from:button];
        }] forControlEvents:UIControlEventTouchUpInside];
        width = 72;
      }
      control = button;
      if ([type isEqual:@"ENUM"]) { width = 148; }
    }
    else if (property[@"slider"]) {
      GHOSTIOSRedoSlider *slider = [[[GHOSTIOSRedoSlider alloc] init] autorelease];
      slider.descriptor = property;
      slider.operationID = operation;
      slider.minimumValue = [property[@"slider"][@"min"] floatValue];
      slider.maximumValue = [property[@"slider"][@"max"] floatValue];
      slider.continuous = YES;
      [slider addTarget:self action:@selector(beginSlider:) forControlEvents:UIControlEventTouchDown];
      [slider addTarget:self action:@selector(changeSlider:) forControlEvents:UIControlEventValueChanged];
      [slider addTarget:self action:@selector(finishSlider:) forControlEvents:UIControlEventTouchUpInside | UIControlEventTouchUpOutside];
      [slider addTarget:self action:@selector(cancelSlider:) forControlEvents:UIControlEventTouchCancel];
      control = slider;
      width = 148;
    }
    else {
      GHOSTIOSRedoField *field = [[[GHOSTIOSRedoField alloc] init] autorelease];
      field.delegate = self;
      field.descriptor = property;
      field.operationID = operation;
      field.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
      field.textAlignment = NSTextAlignmentCenter;
      field.backgroundColor = UIColor.tertiarySystemFillColor;
      field.layer.cornerRadius = 6;
      field.returnKeyType = UIReturnKeyDone;
      field.keyboardType = [type isEqual:@"STRING"] ? UIKeyboardTypeDefault : UIKeyboardTypeNumbersAndPunctuation;
      field.autocorrectionType = UITextAutocorrectionTypeNo;
      if (![type isEqual:@"STRING"]) {
        field.font = [[UIFontMetrics metricsForTextStyle:UIFontTextStyleBody]
            scaledFontForFont:[UIFont monospacedDigitSystemFontOfSize:15 weight:UIFontWeightRegular]];
        field.adjustsFontForContentSizeCategory = YES;
        field.adjustsFontSizeToFitWidth = YES;
        field.minimumFontSize = 12;
        GHOSTIOSNumberFieldAppearance(field, false);
      }
      [field addTarget:self action:@selector(commitField:) forControlEvents:UIControlEventEditingDidEnd];
      if (property[@"dragStep"]) {
        UILongPressGestureRecognizer *scrub = [[[UILongPressGestureRecognizer alloc]
            initWithTarget:self action:@selector(scrubNumber:)] autorelease];
        scrub.minimumPressDuration = 0.25;
        [field addGestureRecognizer:scrub];
      }
      control = field;
    }
    control.accessibilityIdentifier = [@"blender.redo.property." stringByAppendingString:key];
    control.accessibilityLabel = name;
    control.enabled = [property[@"editable"] boolValue];
    _controls[key] = control;
    UIStackView *column = [[[UIStackView alloc] initWithArrangedSubviews:@[label, control]] autorelease];
    column.axis = UILayoutConstraintAxisVertical;
    column.alignment = UIStackViewAlignmentLeading;
    column.spacing = 2;
    [column.widthAnchor constraintEqualToConstant:width].active = YES;
    [label.widthAnchor constraintLessThanOrEqualToAnchor:column.widthAnchor].active = YES;
    label.adjustsFontSizeToFitWidth = YES;
    label.minimumScaleFactor = 0.8;
    if (![control isKindOfClass:UISwitch.class]) {
      [control.widthAnchor constraintEqualToAnchor:column.widthAnchor].active = YES;
      [control.heightAnchor constraintEqualToConstant:32].active = YES;
    }
    [_stack addArrangedSubview:column];
  }
}

- (void)commitField:(GHOSTIOSRedoField *)field
{
  if (field == _scrubField) { return; }
  if (![field.descriptor[@"type"] isEqual:@"STRING"]) { GHOSTIOSNumberFieldAppearance(field, false); }
  if ([field.text isEqual:field.originalText]) { return; }
  id value = field.text ?: @"";
  if (![field.descriptor[@"type"] isEqual:@"STRING"]) {
    NSScanner *scanner = [NSScanner scannerWithString:field.text];
    scanner.locale = NSLocale.currentLocale;
    double number;
    if (![scanner scanDouble:&number] || !scanner.isAtEnd || !std::isfinite(number)) {
      field.text = field.originalText;
      UIAccessibilityPostNotification(UIAccessibilityAnnouncementNotification, @"Enter a valid number.");
      return;
    }
    value = @([field.descriptor[@"angle"] boolValue] ? number * M_PI / 180.0 : number);
  }
  [self sendProperty:field.descriptor value:value operation:field.operationID];
  NSMutableDictionary *updated = [[field.descriptor mutableCopy] autorelease];
  updated[@"value"] = value;
  field.descriptor = updated;
  field.originalText = field.text;
}

- (void)beginSlider:(GHOSTIOSRedoSlider *)slider
{
  slider.interactionID = NSUUID.UUID.UUIDString;
  slider.startValue = slider.value;
  slider.lastPreview = 0;
}
- (void)changeSlider:(GHOSTIOSRedoSlider *)slider
{
  if (slider.interactionID && CACurrentMediaTime() - slider.lastPreview < 1.0 / 60.0) { return; }
  double value = [slider.descriptor[@"type"] isEqual:@"INT"] ? round(slider.value) : slider.value;
  [self sendProperty:slider.descriptor value:@(value) operation:slider.operationID
        interaction:slider.interactionID phase:@"update"];
  slider.lastPreview = CACurrentMediaTime();
}
- (void)finishSlider:(GHOSTIOSRedoSlider *)slider
{
  if (!slider.interactionID) { return; }
  double value = [slider.descriptor[@"type"] isEqual:@"INT"] ? round(slider.value) : slider.value;
  [self sendProperty:slider.descriptor value:@(value) operation:slider.operationID
        interaction:slider.interactionID phase:@"end"];
  slider.interactionID = nil;
}
- (void)cancelSlider:(GHOSTIOSRedoSlider *)slider
{
  if (!slider.interactionID) { return; }
  slider.value = slider.startValue;
  [self sendProperty:slider.descriptor value:@(slider.startValue) operation:slider.operationID
        interaction:slider.interactionID phase:@"cancel"];
  slider.interactionID = nil;
}

- (void)scrubNumber:(UILongPressGestureRecognizer *)gesture
{
  GHOSTIOSRedoField *field = (GHOSTIOSRedoField *)gesture.view;
  if (gesture.state == UIGestureRecognizerStateBegan) {
    if (!field.enabled || ![field.operationID isEqual:_snapshot[@"id"]]) { return; }
    [_rail endEditing:YES];
    _scrubField = field;
    _scrubStart = [gesture locationInView:_overlay];
    _scrubValue = [field.descriptor[@"value"] doubleValue];
    _scrubCurrentValue = _scrubValue;
    _scrubTranslation = 0;
    [_scrubInteraction release]; _scrubInteraction = [NSUUID.UUID.UUIDString copy];
    _scrubPreviewTime = 0;
    GHOSTIOSNumberFieldAppearance(field, true);
    _scroll.scrollEnabled = NO;
  }
  else if (_scrubField == field) {
    CGPoint point = [gesture locationInView:_overlay];
    if (!std::isfinite(point.x) || !std::isfinite(point.y)) { return; }
    const double translation = point.x - _scrubStart.x;
    _scrubCurrentValue += (translation - _scrubTranslation) * [field.descriptor[@"dragStep"] doubleValue] *
                         GHOSTIOSNumberSensitivity(point.y - _scrubStart.y);
    _scrubCurrentValue = fmax([field.descriptor[@"min"] doubleValue],
                             fmin([field.descriptor[@"max"] doubleValue], _scrubCurrentValue));
    _scrubTranslation = translation;
    double value = _scrubCurrentValue;
    if ([field.descriptor[@"type"] isEqual:@"INT"]) { value = round(value); }
    NSNumberFormatter *format = [[[NSNumberFormatter alloc] init] autorelease];
    format.numberStyle = NSNumberFormatterDecimalStyle;
    format.usesGroupingSeparator = NO;
    format.maximumFractionDigits = 4;
    field.text = [format stringFromNumber:@([field.descriptor[@"angle"] boolValue] ? value * 180 / M_PI : value)];
    if (gesture.state == UIGestureRecognizerStateChanged && CACurrentMediaTime() - _scrubPreviewTime >= 1.0 / 60.0) {
      [self sendProperty:field.descriptor value:@(value) operation:field.operationID
            interaction:_scrubInteraction phase:@"update"];
      _scrubPreviewTime = CACurrentMediaTime();
    }
    if (gesture.state == UIGestureRecognizerStateEnded || gesture.state == UIGestureRecognizerStateCancelled ||
        gesture.state == UIGestureRecognizerStateFailed) {
      _scrubField = nil;
      _scroll.scrollEnabled = YES;
      GHOSTIOSNumberFieldAppearance(field, false);
      const BOOL ended = gesture.state == UIGestureRecognizerStateEnded;
      [self sendProperty:field.descriptor value:@(ended ? value : _scrubValue) operation:field.operationID
            interaction:_scrubInteraction phase:ended ? @"end" : @"cancel"];
      NSMutableDictionary *updated = [[field.descriptor mutableCopy] autorelease];
      updated[@"value"] = @(ended ? value : _scrubValue);
      field.descriptor = updated;
      if (ended) { field.originalText = field.text; }
      else { field.text = field.originalText; }
      [_scrubInteraction release]; _scrubInteraction = nil;
    }
  }
}

- (void)textFieldDidBeginEditing:(UITextField *)field
{
  if (![[(GHOSTIOSRedoField *)field descriptor][@"type"] isEqual:@"STRING"]) {
    GHOSTIOSNumberFieldAppearance(field, false);
  }
  ((GHOSTIOSRedoField *)field).originalText = field.text;
  [_scroll scrollRectToVisible:[field convertRect:field.bounds toView:_stack] animated:YES];
}
- (BOOL)textFieldShouldReturn:(UITextField *)field { [field resignFirstResponder]; return YES; }

- (void)showColor:(NSDictionary *)property from:(UIButton *)button
{
  UIViewController *controller = _overlay.window.rootViewController;
  if (!property || controller.presentedViewController) { return; }
  [_colorProperty release]; _colorProperty = [property copy];
  [_colorOperationID release]; _colorOperationID = [_snapshot[@"id"] copy];
  [_colorPicker release];
  _colorPicker = [[UIColorPickerViewController alloc] init];
  _colorPicker.delegate = self;
  _colorPicker.selectedColor = button.backgroundColor;
  _colorPicker.supportsAlpha = [property[@"alpha"] boolValue];
  _colorPicker.modalPresentationStyle = UIModalPresentationPopover;
  _colorPicker.popoverPresentationController.sourceView = button;
  _colorPicker.popoverPresentationController.sourceRect = button.bounds;
  [controller presentViewController:_colorPicker animated:YES completion:nil];
}
- (void)colorPickerViewControllerDidFinish:(UIColorPickerViewController *)picker
{
  CGFloat r, g, b, a;
  if ([picker.selectedColor getRed:&r green:&g blue:&b alpha:&a]) {
    NSArray *value = picker.supportsAlpha ? @[@(r), @(g), @(b), @(a)] : @[@(r), @(g), @(b)];
    [self sendProperty:_colorProperty value:value operation:_colorOperationID];
  }
}

- (void)colorPickerViewController:(UIColorPickerViewController *)picker didSelectColor:(UIColor *)color continuously:(BOOL)continuously
{
  if (!continuously || CACurrentMediaTime() - _colorPreviewTime >= 1.0 / 60.0) {
    CGFloat r, g, b, a;
    if ([color getRed:&r green:&g blue:&b alpha:&a]) {
      NSArray *value = picker.supportsAlpha ? @[@(r), @(g), @(b), @(a)] : @[@(r), @(g), @(b)];
      [self sendProperty:_colorProperty value:value operation:_colorOperationID
            interaction:[@"color:" stringByAppendingString:_colorOperationID] phase:continuously ? @"update" : @"end"];
      _colorPreviewTime = CACurrentMediaTime();
    }
  }
}

- (void)updateSnapshot:(NSDictionary *)snapshot
{
  const CGFloat previousWidth = self.collapsedWidth;
  if (![snapshot isKindOfClass:NSDictionary.class]) {
    [_snapshot release]; _snapshot = nil;
    _scrubField = nil;
    _scroll.scrollEnabled = YES;
    [self setVisible:NO];
    _collapsedButton.hidden = YES;
    if (previousWidth && _presentationDidChange) { _presentationDidChange(NO); }
    return;
  }
  BOOL changedOperation = ![_snapshot[@"id"] isEqual:snapshot[@"id"]];
  if (changedOperation) {
    /* Expansion belongs only to the operation the user explicitly opened. */
    if (![_expandedID isEqual:snapshot[@"id"]]) {
      [_expandedID release]; _expandedID = nil;
    }
    /* Retiring fields must not commit against a newly published operation. */
    [_snapshot release]; _snapshot = nil;
    [_rail endEditing:YES];
    [_pending removeAllObjects];
    _scrubField = nil;
    _scroll.scrollEnabled = YES;
    _colorPicker.delegate = nil;
    [_colorPicker dismissViewControllerAnimated:YES completion:nil];
  }
  [_snapshot release]; _snapshot = [snapshot copy];
  [self updateCollapsedTitle];
  _sequence = MAX(_sequence, [snapshot[@"ack"] unsignedLongLongValue]);
  NSMutableArray *shape = [NSMutableArray array];
  for (NSDictionary *property in snapshot[@"properties"]) {
    [shape addObject:@[property[@"key"], property[@"type"], property[@"label"], property[@"group"] ?: @"", @(property[@"slider"] != nil)]];
  }
  if (changedOperation || ![_shape isEqual:shape]) {
    [_shape release]; _shape = [shape copy];
    [self rebuild];
    if (changedOperation) { _scroll.contentOffset = CGPointZero; }
  }
  __block GHOSTIOSRedoStrip *owner = self;
  for (NSDictionary *property in snapshot[@"properties"]) {
    NSString *key = property[@"key"];
    UIControl *control = _controls[key];
    control.enabled = [property[@"editable"] boolValue];
    if ([_pending[key] unsignedLongLongValue] > [snapshot[@"ack"] unsignedLongLongValue] ||
        control.isFirstResponder || control == _scrubField || control.tracking ||
        ([control isKindOfClass:GHOSTIOSRedoSlider.class] && ((GHOSTIOSRedoSlider *)control).interactionID)) { continue; }
    [_pending removeObjectForKey:key];
    id value = property[@"value"];
    if ([control isKindOfClass:GHOSTIOSRedoField.class]) {
      GHOSTIOSRedoField *field = (GHOSTIOSRedoField *)control;
      field.descriptor = property;
      if ([property[@"type"] isEqual:@"STRING"]) { field.text = value; }
      else {
        NSNumberFormatter *formatter = [[[NSNumberFormatter alloc] init] autorelease];
        formatter.numberStyle = NSNumberFormatterDecimalStyle;
        formatter.usesGroupingSeparator = NO;
        formatter.maximumFractionDigits = [property[@"type"] isEqual:@"INT"] ? 0 : 4;
        double number = [value doubleValue] * ([property[@"angle"] boolValue] ? 180 / M_PI : 1);
        field.text = [formatter stringFromNumber:@(number)];
      }
      field.originalText = field.text;
    }
    else if ([control isKindOfClass:UISwitch.class]) { ((UISwitch *)control).on = [value boolValue]; }
    else if ([control isKindOfClass:UISlider.class]) {
      ((UISlider *)control).minimumValue = [property[@"slider"][@"min"] floatValue];
      ((UISlider *)control).maximumValue = [property[@"slider"][@"max"] floatValue];
      ((UISlider *)control).value = [value floatValue];
      control.accessibilityValue = [value description];
    }
    else if ([property[@"type"] isEqual:@"COLOR"]) {
      control.backgroundColor = [UIColor colorWithRed:[value[0] doubleValue] green:[value[1] doubleValue]
          blue:[value[2] doubleValue] alpha:[value count] == 4 ? [value[3] doubleValue] : 1];
      control.layer.cornerRadius = 6;
    }
    else if ([property[@"type"] isEqual:@"ENUM"]) {
      UIButton *button = (UIButton *)control;
      if ([button.accessibilityValue isEqual:value] && [objc_getAssociatedObject(button, @selector(updateSnapshot:)) isEqual:property[@"options"]]) { continue; }
      objc_setAssociatedObject(button, @selector(updateSnapshot:), property[@"options"], OBJC_ASSOCIATION_COPY_NONATOMIC);
      button.accessibilityValue = value;
      NSMutableArray *actions = [NSMutableArray array];
      NSString *operation = snapshot[@"id"];
      UIButtonConfiguration *config = button.configuration;
      config.title = @"None";
      config.image = nil;
      for (NSDictionary *option in property[@"options"]) {
        UIAction *action = [UIAction actionWithTitle:option[@"label"] image:_icon(option[@"icon"])
            identifier:nil handler:^(UIAction *action) {
          [owner sendProperty:[owner propertyForKey:key] value:option[@"value"] operation:operation];
        }];
        action.state = [option[@"checked"] boolValue] ? UIMenuElementStateOn : UIMenuElementStateOff;
        if (action.state == UIMenuElementStateOn) { config.title = option[@"label"]; config.image = _icon(option[@"icon"]); }
        [actions addObject:action];
      }
      config.imagePadding = 4;
      button.configuration = config;
      button.menu = [UIMenu menuWithTitle:property[@"label"] children:actions];
    }
  }
  const CGFloat collapsedWidth = self.collapsedWidth;
  _collapsedButton.hidden = !collapsedWidth;
  [self setVisible:!collapsedWidth && [snapshot[@"properties"] count] > 0];
  if (previousWidth != collapsedWidth && _presentationDidChange) { _presentationDidChange(NO); }
  if ([snapshot[@"error"] length]) {
    _done.accessibilityHint = snapshot[@"error"];
    NSString *identity = [NSString stringWithFormat:@"%@:%@:%@", snapshot[@"id"], snapshot[@"ack"], snapshot[@"error"]];
    UIViewController *controller = _overlay.window.rootViewController;
    if (![_lastError isEqual:identity] && controller && !controller.presentedViewController) {
      [_lastError release]; _lastError = [identity copy];
      UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Unable to Adjust Operation"
          message:snapshot[@"error"] preferredStyle:UIAlertControllerStyleAlert];
      [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
      [controller presentViewController:alert animated:YES completion:nil];
    }
  }
}
@end
