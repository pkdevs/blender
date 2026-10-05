/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#import "GHOST_IOSScenePanels.hh"
#include <cmath>

extern "C" void WM_apple_ios_ui_setting_enqueue(const char *, const char *);

@interface GHOSTIOSPropertyField : UITextField
@property(nonatomic, retain) NSDictionary *descriptor;
@property(nonatomic, copy) NSString *objectID;
@property(nonatomic, copy) NSString *originalText;
@end
@implementation GHOSTIOSPropertyField
- (CGRect)textRectForBounds:(CGRect)bounds { return CGRectInset(bounds, 8, 0); }
- (CGRect)editingRectForBounds:(CGRect)bounds { return CGRectInset(bounds, 8, 0); }
- (void)dealloc
{
  [_descriptor release];
  [_objectID release];
  [_originalText release];
  [super dealloc];
}
@end

@interface GHOSTIOSColorButton : UIButton
@property(nonatomic, retain) NSDictionary *descriptor;
@property(nonatomic, copy) NSString *objectID;
@property(nonatomic, readonly) UIColorWell *well;
@end

@implementation GHOSTIOSColorButton
- (instancetype)initWithFrame:(CGRect)frame
{
  if ((self = [super initWithFrame:frame])) {
    _well = [[UIColorWell alloc] init];
    /* Keep the system swatch, with explicit picker delegation for undo boundaries. */
    _well.userInteractionEnabled = NO;
    _well.isAccessibilityElement = NO;
    _well.translatesAutoresizingMaskIntoConstraints = NO;
    [self addSubview:_well];
    [NSLayoutConstraint activateConstraints:@[
      [_well.centerXAnchor constraintEqualToAnchor:self.centerXAnchor],
      [_well.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
      [_well.widthAnchor constraintEqualToConstant:32],
      [_well.heightAnchor constraintEqualToConstant:32],
    ]];
  }
  return self;
}
- (void)setEnabled:(BOOL)enabled
{
  [super setEnabled:enabled];
  _well.enabled = enabled;
}
- (void)dealloc
{
  [_well release];
  [_descriptor release];
  [_objectID release];
  [super dealloc];
}
@end

static UIColor *blenderNativeColor(NSArray *components)
{
  if (components.count != 3 && components.count != 4) {
    return UIColor.clearColor;
  }
  CGFloat values[] = {[components[0] doubleValue], [components[1] doubleValue],
                      [components[2] doubleValue], components.count == 4 ? [components[3] doubleValue] : 1};
  CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceExtendedSRGB);
  CGColorRef color = CGColorCreate(space, values);
  UIColor *result = [UIColor colorWithCGColor:color];
  CGColorRelease(color);
  CGColorSpaceRelease(space);
  return result;
}

static NSArray *blenderNativeColorComponents(UIColor *color, BOOL alpha)
{
  CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceExtendedSRGB);
  CGColorRef converted = CGColorCreateCopyByMatchingToColorSpace(space, kCGRenderingIntentDefault,
                                                               color.CGColor, nullptr);
  CGColorSpaceRelease(space);
  if (!converted || CGColorGetNumberOfComponents(converted) != 4) {
    if (converted) {
      CGColorRelease(converted);
    }
    return nil;
  }
  const CGFloat *values = CGColorGetComponents(converted);
  NSArray *result = alpha ? @[@(values[0]), @(values[1]), @(values[2]), @(values[3])] :
                           @[@(values[0]), @(values[1]), @(values[2])];
  CGColorRelease(converted);
  return result;
}

@interface GHOSTIOSScenePanels () <UITableViewDataSource, UITableViewDelegate,
                                   UISearchBarDelegate, UITextFieldDelegate,
                                   UIColorPickerViewControllerDelegate, UIPopoverPresentationControllerDelegate>
@end

@implementation GHOSTIOSScenePanels {
  UIView *_overlay;
  UIImage *(^_icon)(NSString *);
  UIBarButtonItem *_inspectorButton, *_hierarchyButton;
  UIVisualEffectView *_inspector, *_hierarchy;
  UILabel *_inspectorTitle, *_status;
  UIScrollView *_inspectorScroll;
  UITableView *_form;
  NSMutableArray *_formSections;
  NSMutableArray *_headerControls;
  NSArray *_sectionTitles;
  NSArray *_sectionIdentifiers, *_tabs;
  NSString *_selectedTab;
  NSMutableDictionary *_expandedPanels;
  UIScrollView *_tabsScroll;
  UIStackView *_tabsStack;
  UITableView *_tree;
  UISearchBar *_search;
  NSMutableSet *_collapsed;
  NSArray *_nodes, *_rows, *_schema;
  NSDictionary *_snapshot;
  NSString *_objectID;
  NSMutableDictionary *_controls;
  uint64_t _sequence;
  BOOL _showInspector, _showHierarchy;
  CGRect _layoutRect;
  UIColorPickerViewController *_colorPicker;
  GHOSTIOSColorButton *_colorButton;
  BOOL _colorPending;
}

- (instancetype)initWithOverlay:(UIView *)overlay icon:(UIImage *(^)(NSString *))icon
{
  if ((self = [super init])) {
    _overlay = overlay;
    _icon = [icon copy];
    _collapsed = [[NSMutableSet alloc] init];
    _controls = [[NSMutableDictionary alloc] init];
    _expandedPanels = [[NSMutableDictionary alloc] init];
    _inspectorButton = [[UIBarButtonItem alloc] initWithImage:[UIImage systemImageNamed:@"sidebar.right"]
        style:UIBarButtonItemStylePlain target:self action:@selector(toggleInspector:)];
    _hierarchyButton = [[UIBarButtonItem alloc] initWithImage:[UIImage systemImageNamed:@"list.bullet.indent"]
        style:UIBarButtonItemStylePlain target:self action:@selector(toggleHierarchy:)];
    _inspectorButton.title = _inspectorButton.accessibilityLabel = @"Properties";
    _hierarchyButton.title = _hierarchyButton.accessibilityLabel = @"Hierarchy";
    _inspectorButton.identifier = _inspectorButton.accessibilityIdentifier = @"blender.inspector";
    _hierarchyButton.identifier = _hierarchyButton.accessibilityIdentifier = @"blender.hierarchy";
    _inspector = [[self panelWithTitle:@"Properties" close:@selector(toggleInspector:)] retain];
    _hierarchy = [[self panelWithTitle:@"Hierarchy" close:@selector(toggleHierarchy:)] retain];
    _inspector.accessibilityIdentifier = @"blender.inspector.panel";
    _hierarchy.accessibilityIdentifier = @"blender.hierarchy.panel";
    _inspectorTitle = (UILabel *)[_inspector.contentView viewWithTag:100];
    _tabsScroll = [[[UIScrollView alloc] init] autorelease];
    _tabsScroll.showsHorizontalScrollIndicator = NO;
    _tabsScroll.translatesAutoresizingMaskIntoConstraints = NO;
    [_inspector.contentView addSubview:_tabsScroll];
    _tabsStack = [[[UIStackView alloc] init] autorelease];
    _tabsStack.spacing = 2;
    _tabsStack.translatesAutoresizingMaskIntoConstraints = NO;
    [_tabsScroll addSubview:_tabsStack];
    [NSLayoutConstraint activateConstraints:@[
      [_tabsScroll.topAnchor constraintEqualToAnchor:_inspector.contentView.topAnchor constant:52],
      [_tabsScroll.leadingAnchor constraintEqualToAnchor:_inspector.contentView.leadingAnchor constant:8],
      [_tabsScroll.trailingAnchor constraintEqualToAnchor:_inspector.contentView.trailingAnchor constant:-8],
      [_tabsScroll.heightAnchor constraintEqualToConstant:44],
      [_tabsStack.topAnchor constraintEqualToAnchor:_tabsScroll.contentLayoutGuide.topAnchor],
      [_tabsStack.bottomAnchor constraintEqualToAnchor:_tabsScroll.contentLayoutGuide.bottomAnchor],
      [_tabsStack.leadingAnchor constraintEqualToAnchor:_tabsScroll.contentLayoutGuide.leadingAnchor],
      [_tabsStack.trailingAnchor constraintEqualToAnchor:_tabsScroll.contentLayoutGuide.trailingAnchor],
      [_tabsStack.heightAnchor constraintEqualToAnchor:_tabsScroll.frameLayoutGuide.heightAnchor],
    ]];
    _formSections = [[NSMutableArray alloc] init];
    _form = [[[UITableView alloc] initWithFrame:CGRectZero style:UITableViewStyleInsetGrouped] autorelease];
    _form.dataSource = self;
    _form.delegate = self;
    _form.backgroundColor = UIColor.clearColor;
    _form.rowHeight = UITableViewAutomaticDimension;
    _form.estimatedRowHeight = 48;
    _form.sectionHeaderHeight = UITableViewAutomaticDimension;
    _form.estimatedSectionHeaderHeight = 32;
    _inspectorScroll = _form;
    _inspectorScroll.translatesAutoresizingMaskIntoConstraints = NO;
    _inspectorScroll.keyboardDismissMode = UIScrollViewKeyboardDismissModeInteractive;
    [_inspector.contentView addSubview:_inspectorScroll];
    [NSLayoutConstraint activateConstraints:@[
      [_inspectorScroll.topAnchor constraintEqualToAnchor:_tabsScroll.bottomAnchor constant:4],
      [_inspectorScroll.bottomAnchor constraintEqualToAnchor:_inspector.contentView.bottomAnchor],
      [_inspectorScroll.leadingAnchor constraintEqualToAnchor:_inspector.contentView.leadingAnchor],
      [_inspectorScroll.trailingAnchor constraintEqualToAnchor:_inspector.contentView.trailingAnchor],
    ]];
    _search = [[[UISearchBar alloc] init] autorelease];
    _search.searchBarStyle = UISearchBarStyleMinimal;
    _search.placeholder = @"Search";
    _search.delegate = self;
    _search.accessibilityIdentifier = @"blender.hierarchy.search";
    [_hierarchy.contentView addSubview:_search];
    _tree = [[[UITableView alloc] initWithFrame:CGRectZero style:UITableViewStylePlain] autorelease];
    _tree.backgroundColor = UIColor.clearColor;
    _tree.separatorStyle = UITableViewCellSeparatorStyleNone;
    _tree.rowHeight = 44;
    _tree.dataSource = self;
    _tree.delegate = self;
    _tree.keyboardDismissMode = UIScrollViewKeyboardDismissModeOnDrag;
    [_hierarchy.contentView addSubview:_tree];
  }
  return self;
}

- (void)dealloc
{
  [_inspector removeFromSuperview];
  [_hierarchy removeFromSuperview];
  [_icon release];
  [_inspectorButton release];
  [_hierarchyButton release];
  [_inspector release];
  [_hierarchy release];
  [_collapsed release];
  [_controls release];
  [_formSections release];
  [_headerControls release];
  [_menuProvider release];
  [_sectionTitles release];
  [_sectionIdentifiers release];
  [_tabs release];
  [_selectedTab release];
  [_expandedPanels release];
  [_nodes release];
  [_rows release];
  [_schema release];
  [_snapshot release];
  [_objectID release];
  _colorPicker.delegate = nil;
  _colorPicker.presentationController.delegate = nil;
  [_colorPicker release];
  [_colorButton release];
  [super dealloc];
}

- (UIBarButtonItem *)inspectorButton { return _inspectorButton; }
- (UIBarButtonItem *)hierarchyButton { return _hierarchyButton; }

- (UIVisualEffectView *)panelWithTitle:(NSString *)title close:(SEL)selector
{
  UIVisualEffectView *panel = [[[UIVisualEffectView alloc]
      initWithEffect:[UIGlassEffect effectWithStyle:UIGlassEffectStyleRegular]] autorelease];
  panel.layer.cornerRadius = 16;
  panel.clipsToBounds = YES;
  panel.hidden = YES;
  UILabel *label = [[[UILabel alloc] init] autorelease];
  label.tag = 100;
  label.text = title;
  label.font = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
  label.adjustsFontForContentSizeCategory = YES;
  label.translatesAutoresizingMaskIntoConstraints = NO;
  [panel.contentView addSubview:label];
  UIButton *close = [UIButton buttonWithType:UIButtonTypeSystem];
  close.tintColor = UIColor.secondaryLabelColor;
  [close setImage:[UIImage systemImageNamed:@"xmark"] forState:UIControlStateNormal];
  close.accessibilityLabel = [@"Close " stringByAppendingString:title];
  [close addTarget:self action:selector forControlEvents:UIControlEventTouchUpInside];
  close.translatesAutoresizingMaskIntoConstraints = NO;
  [panel.contentView addSubview:close];
  [NSLayoutConstraint activateConstraints:@[
    [label.leadingAnchor constraintEqualToAnchor:panel.contentView.leadingAnchor constant:16],
    [label.centerYAnchor constraintEqualToAnchor:close.centerYAnchor],
    [label.trailingAnchor constraintLessThanOrEqualToAnchor:close.leadingAnchor constant:-8],
    [close.topAnchor constraintEqualToAnchor:panel.contentView.topAnchor constant:5],
    [close.trailingAnchor constraintEqualToAnchor:panel.contentView.trailingAnchor constant:-5],
    [close.widthAnchor constraintEqualToConstant:44], [close.heightAnchor constraintEqualToConstant:44],
  ]];
  [_overlay addSubview:panel];
  return panel;
}

- (void)animatePanel:(UIView *)panel visible:(BOOL)visible
{
  if (visible) {
    [_overlay bringSubviewToFront:panel];
    if (panel.hidden) {
      panel.alpha = 0;
      panel.transform = CGAffineTransformMakeTranslation(panel == _hierarchy ? -12 : 12, 0);
    }
    panel.hidden = NO;
  }
  [UIView animateWithDuration:UIAccessibilityIsReduceMotionEnabled() ? 0 : 0.22
      delay:0 options:UIViewAnimationOptionBeginFromCurrentState | UIViewAnimationOptionAllowUserInteraction
      animations:^{
        panel.alpha = visible ? 1 : 0;
        panel.transform = CGAffineTransformIdentity;
      } completion:^(BOOL finished) {
        if (finished && panel.alpha == 0) {
          panel.hidden = YES;
        }
      }];
}

- (void)applyVisibility
{
  if (!_showInspector) {
    [self closeColorPicker];
  }
  _inspectorButton.selected = _showInspector;
  _hierarchyButton.selected = _showHierarchy;
  [self animatePanel:_inspector visible:_showInspector];
  [self animatePanel:_hierarchy visible:_showHierarchy];
  NSString *mask = [NSString stringWithFormat:@"%d", (_showInspector ? 1 : 0) | (_showHierarchy ? 2 : 0)];
  WM_apple_ios_ui_setting_enqueue("native_panels", mask.UTF8String);
  [self layoutInRect:_layoutRect];
}

- (void)toggleInspector:(id)sender
{
  [_inspector endEditing:YES];
  _showInspector = !_showInspector;
  if (_showInspector && _layoutRect.size.width < 700) {
    _showHierarchy = NO;
    [_hierarchy endEditing:YES];
  }
  [self applyVisibility];
}

- (void)toggleHierarchy:(id)sender
{
  [_hierarchy endEditing:YES];
  _showHierarchy = !_showHierarchy;
  if (_showHierarchy && _layoutRect.size.width < 700) {
    _showInspector = NO;
    [_inspector endEditing:YES];
  }
  [self applyVisibility];
}

- (void)dismiss
{
  [_inspector endEditing:YES];
  [_hierarchy endEditing:YES];
  _showInspector = _showHierarchy = NO;
  [self applyVisibility];
}

- (void)layoutInRect:(CGRect)rect
{
  _layoutRect = rect;
  if (rect.size.width < 700 && _showInspector && _showHierarchy) {
    _showHierarchy = NO;
    [self applyVisibility];
    return;
  }
  const CGFloat width = MIN(336, MAX(0, rect.size.width - 24));
  const CGFloat height = MIN(620, MAX(0, rect.size.height - 24));
  _hierarchy.frame = CGRectMake(rect.origin.x + 12, rect.origin.y + 12, width, height);
  _inspector.frame = CGRectMake(CGRectGetMaxX(rect) - width - 12, rect.origin.y + 12, width, height);
  _search.frame = CGRectMake(4, 52, width - 8, 52);
  _tree.frame = CGRectMake(0, 106, width, MAX(0, height - 106));
}

- (void)send:(NSDictionary *)request
{
  NSMutableDictionary *payload = [[request mutableCopy] autorelease];
  payload[@"sequence"] = @(++_sequence);
  NSData *data = [NSJSONSerialization dataWithJSONObject:payload options:0 error:nil];
  NSString *json = [[[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] autorelease];
  WM_apple_ios_ui_setting_enqueue([request[@"native"] boolValue] ? "native_properties_edit" : "native_panel_edit", json.UTF8String);
}

- (NSString *)keyForProperty:(NSDictionary *)property
{
  if (property[@"key"]) {
    return property[@"key"];
  }
  return [NSString stringWithFormat:@"%@.%@.%@", property[@"owner"], property[@"property"], property[@"index"] ?: @""];
}

- (void)editProperty:(NSDictionary *)property object:(NSString *)objectID value:(id)value
{
  NSMutableDictionary *request = [[property mutableCopy] autorelease];
  request[@"action"] = @"property";
  request[@"id"] = objectID;
  request[@"value"] = value;
  [self send:request];
}

- (UILabel *)label:(NSString *)text style:(UIFontTextStyle)style
{
  UILabel *label = [[[UILabel alloc] init] autorelease];
  label.text = text;
  label.font = [UIFont preferredFontForTextStyle:style];
  label.adjustsFontForContentSizeCategory = YES;
  label.numberOfLines = 0;
  return label;
}

- (void)rebuildInspector:(NSDictionary *)inspector
{
  [self closeColorPicker];
  [_headerControls release];
  _headerControls = [[NSMutableArray alloc] init];
  [_formSections removeAllObjects];
  NSMutableArray *titles = [NSMutableArray array];
  NSMutableArray *identifiers = [NSMutableArray array];
  [_controls removeAllObjects];
  _status = [self label:@"" style:UIFontTextStyleFootnote];
  _status.textColor = UIColor.systemRedColor;
  _status.hidden = YES;
  _status.frame = CGRectMake(0, 0, _form.bounds.size.width, 44);
  _status.textAlignment = NSTextAlignmentCenter;
  _form.tableFooterView = _status;
  if (!inspector) {
    _form.backgroundView = [self label:@"No active object" style:UIFontTextStyleBody];
    ((UILabel *)_form.backgroundView).textAlignment = NSTextAlignmentCenter;
    [_sectionTitles release];
    _sectionTitles = nil;
    [_form reloadData];
    return;
  }
  _form.backgroundView = nil;
  __block GHOSTIOSScenePanels *owner = self;
  NSString *objectID = inspector[@"id"];
  for (NSDictionary *section in inspector[@"sections"]) {
    [titles addObject:section[@"title"]];
    [identifiers addObject:section[@"id"] ?: section[@"title"]];
    NSMutableArray *sectionRows = [NSMutableArray array];
    [_formSections addObject:sectionRows];
    UIStackView *headerControls = [[[UIStackView alloc] init] autorelease];
    headerControls.spacing = 8;
    [_headerControls addObject:headerControls];
    NSMutableDictionary *vectors = [NSMutableDictionary dictionary];
    NSString *innerSection = nil;
    UIStackView *innerHeading = nil;
    for (NSDictionary *property in section[@"properties"]) {
      if (property[@"section"] && ![innerSection isEqual:property[@"section"]]) {
        innerSection = property[@"section"];
        UILabel *heading = [self label:innerSection style:UIFontTextStyleSubheadline];
        heading.textColor = UIColor.secondaryLabelColor;
        [heading.heightAnchor constraintGreaterThanOrEqualToConstant:36].active = YES;
        innerHeading = [[[UIStackView alloc] initWithArrangedSubviews:@[heading]] autorelease];
        innerHeading.alignment = UIStackViewAlignmentCenter;
        innerHeading.spacing = 8;
        [heading setContentCompressionResistancePriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];
        [sectionRows addObject:innerHeading];
      }
      NSString *group = property[@"group"];
      const BOOL vector = group.length > 0;
      UIStackView *vectorRow = nil;
      if (vector) {
        NSString *groupKey = [NSString stringWithFormat:@"%@.%@.%@", innerSection ?: @"", property[@"owner"], property[@"property"]];
        vectorRow = vectors[groupKey];
        if (!vectorRow) {
          vectorRow = [[[UIStackView alloc] init] autorelease];
          vectorRow.spacing = 4;
          vectorRow.distribution = UIStackViewDistributionFillEqually;
          vectors[groupKey] = vectorRow;
          UILabel *title = [self label:group style:UIFontTextStyleCaption1];
          title.textColor = UIColor.secondaryLabelColor;
          UIStackView *vectorGroup = [[[UIStackView alloc] initWithArrangedSubviews:@[title, vectorRow]] autorelease];
          vectorGroup.axis = UILayoutConstraintAxisVertical;
          vectorGroup.spacing = 0;
          [sectionRows addObject:vectorGroup];
        }
      }
      UILabel *label = [self label:property[@"label"] style:UIFontTextStyleBody];
      UIControl *control;
      NSString *kind = property[@"type"];
      if ([kind isEqual:@"OPERATOR"] || [kind isEqual:@"MENU"]) {
        UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
        UIButtonConfiguration *config = [UIButtonConfiguration plainButtonConfiguration];
        config.title = property[@"label"];
        config.image = _icon(property[@"icon"]);
        config.imagePadding = 8;
        button.configuration = config;
        if ([kind isEqual:@"MENU"]) {
          button.showsMenuAsPrimaryAction = YES;
          button.menu = self.menuProvider ? self.menuProvider(property) : nil;
        }
        else {
          [button addAction:[UIAction actionWithHandler:^(__kindof UIAction *action) {
            [owner send:property];
          }] forControlEvents:UIControlEventTouchUpInside];
        }
        control = button;
        label.hidden = YES;
      }
      else if ([kind isEqual:@"COLOR"]) {
        GHOSTIOSColorButton *button = [[[GHOSTIOSColorButton alloc] initWithFrame:CGRectZero] autorelease];
        button.descriptor = property;
        button.objectID = objectID;
        button.well.title = property[@"label"];
        button.well.supportsAlpha = [property[@"alpha"] boolValue];
        [button addTarget:self action:@selector(openColorPicker:) forControlEvents:UIControlEventTouchUpInside];
        control = button;
      }
      else if ([kind isEqual:@"BOOLEAN"]) {
        UISwitch *toggle = [[[UISwitch alloc] init] autorelease];
        [toggle addAction:[UIAction actionWithHandler:^(__kindof UIAction *action) {
          [owner editProperty:property object:objectID value:@(((UISwitch *)action.sender).on)];
        }] forControlEvents:UIControlEventValueChanged];
        control = toggle;
      }
      else if ([kind isEqual:@"ENUM"]) {
        UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
        button.showsMenuAsPrimaryAction = YES;
        control = button;
      }
      else {
        GHOSTIOSPropertyField *field = [[[GHOSTIOSPropertyField alloc] init] autorelease];
        field.descriptor = property;
        field.objectID = objectID;
        field.delegate = self;
        field.borderStyle = UITextBorderStyleNone;
        field.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
        field.adjustsFontForContentSizeCategory = YES;
        field.textAlignment = NSTextAlignmentRight;
        field.adjustsFontSizeToFitWidth = YES;
        field.minimumFontSize = 12;
        field.autocorrectionType = UITextAutocorrectionTypeNo;
        field.returnKeyType = UIReturnKeyDone;
        field.keyboardType = [kind isEqual:@"STRING"] ? UIKeyboardTypeDefault : UIKeyboardTypeNumbersAndPunctuation;
        if (![kind isEqual:@"STRING"]) {
          field.font = [UIFont monospacedDigitSystemFontOfSize:15 weight:UIFontWeightRegular];
          UIToolbar *accessory = [[[UIToolbar alloc] initWithFrame:CGRectMake(0, 0, 320, 44)] autorelease];
          UIStepper *stepper = [[[UIStepper alloc] init] autorelease];
          stepper.accessibilityIdentifier = [self keyForProperty:property];
          stepper.accessibilityLabel = [NSString stringWithFormat:@"Adjust %@ %@", section[@"title"], property[@"label"]];
          double factor = [property[@"angle"] boolValue] ? 180.0 / M_PI : 1;
          stepper.minimumValue = MAX(-1e12, [property[@"min"] doubleValue] * factor);
          stepper.maximumValue = MIN(1e12, [property[@"max"] doubleValue] * factor);
          stepper.stepValue = [property[@"angle"] boolValue] || [kind isEqual:@"INT"] ? 1 : 0.1;
          [stepper addTarget:self action:@selector(stepNumber:) forControlEvents:UIControlEventValueChanged];
          accessory.items = @[
            [[[UIBarButtonItem alloc] initWithCustomView:stepper] autorelease],
            [[[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemFlexibleSpace target:nil action:nil] autorelease],
            [[[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemDone target:self action:@selector(finishNumber:)] autorelease],
          ];
          field.inputAccessoryView = accessory;
        }
        [field addTarget:self action:@selector(commitField:) forControlEvents:UIControlEventEditingDidEnd];
        control = field;
      }
      control.enabled = [property[@"editable"] boolValue];
      control.accessibilityLabel = [NSString stringWithFormat:@"%@ %@", section[@"title"], property[@"label"]];
      NSString *key = [self keyForProperty:property];
      control.accessibilityIdentifier = [@"blender.property." stringByAppendingString:key];
      _controls[key] = control;
      if ([property[@"header"] boolValue] && [control isKindOfClass:UISwitch.class]) {
        [control setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
        [control setContentCompressionResistancePriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
        [(property[@"section"] ? innerHeading : headerControls) addArrangedSubview:control];
        continue;
      }
      UIStackView *row = [[[UIStackView alloc] initWithArrangedSubviews:@[label, control]] autorelease];
      row.spacing = vector ? 4 : 10;
      row.axis = UILayoutConstraintAxisHorizontal;
      row.alignment = UIStackViewAlignmentCenter;
      if (vector) {
        label.font = [UIFont preferredFontForTextStyle:UIFontTextStyleCaption1];
        [label setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
        [label setContentCompressionResistancePriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
        label.textColor = [property[@"label"] isEqual:@"X"] || [property[@"label"] isEqual:@"R"] ? UIColor.systemRedColor :
                          [property[@"label"] isEqual:@"Y"] || [property[@"label"] isEqual:@"G"] ? UIColor.systemGreenColor :
                          [property[@"label"] isEqual:@"Z"] || [property[@"label"] isEqual:@"B"] ? UIColor.systemBlueColor : UIColor.secondaryLabelColor;
      }
      if (!vector) {
        [label setContentCompressionResistancePriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];
      }
      if (![control isKindOfClass:UISwitch.class]) {
        if ([kind isEqual:@"COLOR"]) {
          [control.widthAnchor constraintEqualToConstant:44].active = YES;
        }
        else if (!vector && ![kind isEqual:@"OPERATOR"] && ![kind isEqual:@"MENU"]) {
          [control.widthAnchor constraintEqualToAnchor:row.widthAnchor multiplier:0.53].active = YES;
        }
        [control.heightAnchor constraintGreaterThanOrEqualToConstant:40].active = YES;
      }
      [row.heightAnchor constraintGreaterThanOrEqualToConstant:44].active = YES;
      if (vectorRow) {
        [vectorRow addArrangedSubview:row];
      }
      else {
        [sectionRows addObject:row];
      }
    }
  }
  [_sectionTitles release];
  _sectionTitles = [titles copy];
  [_sectionIdentifiers release];
  _sectionIdentifiers = [identifiers copy];
  [_form reloadData];
}

- (void)openColorPicker:(GHOSTIOSColorButton *)button
{
  if (_colorPicker || !button.enabled) {
    return;
  }
  UIViewController *presenter = _overlay.window.rootViewController;
  while (presenter.presentedViewController) {
    presenter = presenter.presentedViewController;
  }
  if (!presenter || presenter.isBeingDismissed) {
    return;
  }
  [_inspector endEditing:YES];
  _colorButton = [button retain];
  _colorPicker = [[UIColorPickerViewController alloc] init];
  _colorPicker.title = button.descriptor[@"label"];
  _colorPicker.supportsAlpha = button.well.supportsAlpha;
  _colorPicker.maximumLinearExposure = MAX(1, [button.descriptor[@"exposure"] doubleValue]);
  _colorPicker.selectedColor = button.well.selectedColor;
  _colorPicker.delegate = self;
  _colorPicker.modalPresentationStyle = UIModalPresentationPopover;
  _colorPicker.popoverPresentationController.sourceView = button;
  _colorPicker.popoverPresentationController.sourceRect = button.bounds;
  _colorPicker.popoverPresentationController.delegate = self;
  [presenter presentViewController:_colorPicker animated:YES completion:nil];
}

- (void)commitColor
{
  if (!_colorPending || !_colorButton) {
    return;
  }
  _colorPending = NO;
  NSArray *components = blenderNativeColorComponents(_colorButton.well.selectedColor,
                                                     _colorButton.well.supportsAlpha);
  if (components) {
    [self editProperty:_colorButton.descriptor object:_colorButton.objectID value:components];
  }
}

- (void)closeColorPicker
{
  [self commitColor];
  if (_colorButton && [_snapshot[@"ack"] unsignedLongLongValue] >= _sequence) {
    for (NSDictionary *section in _snapshot[@"inspector"][@"sections"]) {
      for (NSDictionary *property in section[@"properties"]) {
        if ([property[@"key"] isEqual:_colorButton.descriptor[@"key"]] &&
            [property[@"type"] isEqual:@"COLOR"]) {
          _colorButton.well.selectedColor = blenderNativeColor(property[@"value"]);
        }
      }
    }
  }
  _colorPicker.delegate = nil;
  _colorPicker.presentationController.delegate = nil;
  [_colorPicker dismissViewControllerAnimated:YES completion:nil];
  [_colorPicker release];
  _colorPicker = nil;
  [_colorButton release];
  _colorButton = nil;
}

- (void)colorPickerViewController:(UIColorPickerViewController *)picker
                  didSelectColor:(UIColor *)color continuously:(BOOL)continuously
{
  if (picker != _colorPicker) {
    return;
  }
  _colorButton.well.selectedColor = color;
  _colorPending = YES;
  if (!continuously) {
    [self commitColor];
  }
}

- (void)colorPickerViewControllerDidFinish:(UIColorPickerViewController *)picker
{
  [self closeColorPicker];
}

- (void)presentationControllerDidDismiss:(UIPresentationController *)presentationController
{
  [self closeColorPicker];
}

- (void)popoverPresentationControllerDidDismissPopover:(UIPopoverPresentationController *)popover
{
  [self closeColorPicker];
}

- (void)requestProperties
{
  NSMutableDictionary *state = [NSMutableDictionary dictionaryWithObject:_selectedTab ?: @"OBJECT" forKey:@"tab"];
  NSSet *expanded = _expandedPanels[_selectedTab ?: @"OBJECT"];
  if (expanded) {
    state[@"expanded"] = expanded.allObjects;
  }
  NSData *data = [NSJSONSerialization dataWithJSONObject:state options:0 error:nil];
  NSString *json = [[[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] autorelease];
  WM_apple_ios_ui_setting_enqueue("native_properties_state", json.UTF8String);
}

- (void)updatePropertiesTabs:(NSDictionary *)inspector
{
  NSArray *tabs = inspector[@"tabs"] ?: @[];
  if (![_tabs isEqual:tabs]) {
    [_tabs release];
    _tabs = [tabs copy];
    for (UIView *view in [[_tabsStack.arrangedSubviews copy] autorelease]) {
      [view removeFromSuperview];
    }
    __block GHOSTIOSScenePanels *owner = self;
    for (NSDictionary *tab in tabs) {
      UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
      UIButtonConfiguration *configuration = [UIButtonConfiguration plainButtonConfiguration];
      configuration.image = _icon(tab[@"icon"]);
      configuration.preferredSymbolConfigurationForImage = [UIImageSymbolConfiguration configurationWithPointSize:22];
      configuration.contentInsets = NSDirectionalEdgeInsetsMake(10, 10, 10, 10);
      button.configuration = configuration;
      button.accessibilityIdentifier = tab[@"id"];
      button.accessibilityLabel = tab[@"title"];
      [button addInteraction:[[[UIToolTipInteraction alloc] initWithDefaultToolTip:tab[@"title"]] autorelease]];
      [button addAction:[UIAction actionWithHandler:^(__kindof UIAction *action) {
        [owner->_inspector endEditing:YES];
        [owner->_selectedTab release];
        owner->_selectedTab = [tab[@"id"] copy];
        [owner requestProperties];
        for (UIButton *item in owner->_tabsStack.arrangedSubviews) {
          item.selected = [item.accessibilityIdentifier isEqual:owner->_selectedTab];
        }
      }] forControlEvents:UIControlEventTouchUpInside];
      button.configurationUpdateHandler = ^(UIButton *item) {
        UIButtonConfiguration *config = item.configuration;
        config.baseForegroundColor = item.selected ? owner->_overlay.tintColor : UIColor.secondaryLabelColor;
        config.background.backgroundColor = item.selected ? UIColor.tertiarySystemFillColor : UIColor.clearColor;
        config.background.cornerRadius = 8;
        item.configuration = config;
      };
      [button.widthAnchor constraintEqualToConstant:42].active = YES;
      [_tabsStack addArrangedSubview:button];
    }
  }
  if (!_selectedTab || ![tabs filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"id == %@", _selectedTab]].count) {
    [_selectedTab release];
    _selectedTab = [inspector[@"tab"] copy];
  }
  for (UIButton *button in _tabsStack.arrangedSubviews) {
    button.selected = [button.accessibilityIdentifier isEqual:_selectedTab];
  }
  if (_selectedTab && !_expandedPanels[_selectedTab] && [inspector[@"tab"] isEqual:_selectedTab]) {
    NSMutableSet *expanded = [NSMutableSet set];
    for (NSDictionary *section in inspector[@"sections"]) {
      if ([section[@"open"] boolValue]) {
        [expanded addObject:section[@"id"]];
      }
    }
    _expandedPanels[_selectedTab] = expanded;
  }
}

- (void)finishNumber:(id)sender { [_inspector endEditing:YES]; }

- (void)stepNumber:(UIStepper *)stepper
{
  UITextField *field = _controls[stepper.accessibilityIdentifier];
  NSNumberFormatter *formatter = [[[NSNumberFormatter alloc] init] autorelease];
  formatter.numberStyle = NSNumberFormatterDecimalStyle;
  formatter.usesGroupingSeparator = NO;
  formatter.maximumFractionDigits = 6;
  field.text = [formatter stringFromNumber:@(stepper.value)];
}

- (void)commitField:(GHOSTIOSPropertyField *)field
{
  if ([field.text isEqual:field.originalText]) {
    return;
  }
  id value = field.text ?: @"";
  if (![field.descriptor[@"type"] isEqual:@"STRING"]) {
    NSScanner *scanner = [NSScanner scannerWithString:field.text];
    scanner.locale = NSLocale.currentLocale;
    double number;
    if (![scanner scanDouble:&number] || !scanner.isAtEnd || !std::isfinite(number)) {
      _status.text = @"Enter a valid number.";
      _status.hidden = NO;
      return;
    }
    if ([field.descriptor[@"angle"] boolValue]) {
      number *= M_PI / 180.0;
    }
    value = @(number);
  }
  if (![value isEqual:field.descriptor[@"value"]]) {
    [self editProperty:field.descriptor object:field.objectID value:value];
  }
}

- (BOOL)textFieldShouldReturn:(UITextField *)field
{
  [field resignFirstResponder];
  return YES;
}

- (void)textFieldDidBeginEditing:(UITextField *)field
{
  if ([field.inputAccessoryView isKindOfClass:UIToolbar.class]) {
    UIToolbar *bar = (UIToolbar *)field.inputAccessoryView;
    UIStepper *stepper = (UIStepper *)bar.items.firstObject.customView;
    NSNumberFormatter *formatter = [[[NSNumberFormatter alloc] init] autorelease];
    formatter.numberStyle = NSNumberFormatterDecimalStyle;
    stepper.value = [[formatter numberFromString:field.text] doubleValue];
  }
  dispatch_async(dispatch_get_main_queue(), ^{
    [self->_inspectorScroll scrollRectToVisible:[field convertRect:field.bounds toView:self->_inspectorScroll]
                                     animated:YES];
  });
}

- (void)updateSnapshot:(NSDictionary *)snapshot
{
  if (![snapshot isKindOfClass:NSDictionary.class] || [snapshot[@"ack"] unsignedLongLongValue] < _sequence) {
    return;
  }
  if ([_snapshot isEqual:snapshot]) {
    return;
  }
  [_snapshot release];
  _snapshot = [snapshot copy];
  NSArray *nodes = snapshot[@"hierarchy"];
  if ([nodes isKindOfClass:NSArray.class] && ![_nodes isEqual:nodes]) {
    [_nodes release];
    _nodes = [nodes copy];
    [self rebuildRows];
  }
  NSString *error = snapshot[@"error"] ?: @"";
  if (error.length) {
    UILabel *message = [self label:error style:UIFontTextStyleFootnote];
    message.textColor = UIColor.systemRedColor;
    message.frame = CGRectMake(0, 0, _tree.bounds.size.width, 72);
    message.textAlignment = NSTextAlignmentCenter;
    _tree.tableFooterView = message;
  }
  else {
    _tree.tableFooterView = nil;
  }
  if (!_showInspector || !snapshot[@"inspector"]) {
    return;
  }
  NSDictionary *inspector = [snapshot[@"inspector"] isKindOfClass:NSDictionary.class] ? snapshot[@"inspector"] : nil;
  [self updatePropertiesTabs:inspector];
  if (inspector[@"tab"] && ![inspector[@"tab"] isEqual:_selectedTab]) {
    return;
  }
  NSMutableArray *schema = [NSMutableArray array];
  for (NSDictionary *section in inspector[@"sections"]) {
    [schema addObject:section[@"title"]];
    [schema addObject:section[@"open"] ?: @YES];
    for (NSDictionary *property in section[@"properties"]) {
      NSMutableDictionary *shape = [[property mutableCopy] autorelease];
      [shape removeObjectForKey:@"value"];
      [shape removeObjectForKey:@"exposure"];
      [schema addObject:shape];
    }
  }
  if (![_objectID isEqual:inspector[@"id"]] || ![_schema isEqual:schema]) {
    /* Commit to the original ID before replacing the controls on a selection change. */
    [_inspector endEditing:YES];
    [_objectID release];
    _objectID = [inspector[@"id"] copy];
    [_schema release];
    _schema = [schema copy];
    [self rebuildInspector:inspector];
  }
  _inspectorTitle.text = inspector[@"name"] ?: @"Properties";
  _status.text = snapshot[@"error"] ?: @"";
  _status.hidden = _status.text.length == 0;
  __block GHOSTIOSScenePanels *owner = self;
  for (NSDictionary *section in inspector[@"sections"]) {
    for (NSDictionary *property in section[@"properties"]) {
      UIControl *control = _controls[[self keyForProperty:property]];
      id value = property[@"value"];
      if ([control isKindOfClass:GHOSTIOSColorButton.class]) {
        GHOSTIOSColorButton *button = (GHOSTIOSColorButton *)control;
        if (button != _colorButton) {
          button.descriptor = property;
          button.well.selectedColor = blenderNativeColor(value);
        }
      }
      else if ([control isKindOfClass:GHOSTIOSPropertyField.class]) {
        GHOSTIOSPropertyField *field = (GHOSTIOSPropertyField *)control;
        if (!field.isFirstResponder) {
          field.descriptor = property;
          if ([value isKindOfClass:NSNumber.class]) {
            double number = [value doubleValue] * ([property[@"angle"] boolValue] ? 180.0 / M_PI : 1.0);
            NSNumberFormatter *formatter = [[[NSNumberFormatter alloc] init] autorelease];
            formatter.numberStyle = NSNumberFormatterDecimalStyle;
            formatter.usesGroupingSeparator = NO;
            formatter.usesSignificantDigits = YES;
            formatter.maximumSignificantDigits = 9;
            field.text = [formatter stringFromNumber:@(number)];
          }
          else {
            field.text = value;
          }
          field.originalText = field.text;
        }
      }
      else if ([control isKindOfClass:UISwitch.class]) {
        ((UISwitch *)control).on = [value boolValue];
      }
      else if ([property[@"type"] isEqual:@"ENUM"] && ![control.accessibilityValue isEqual:value]) {
        UIButton *button = (UIButton *)control;
        control.accessibilityValue = value;
        NSMutableArray *actions = [NSMutableArray array];
        NSString *objectID = inspector[@"id"];
        for (NSDictionary *option in property[@"options"]) {
          UIAction *action = [UIAction actionWithTitle:option[@"label"] image:_icon(option[@"icon"]) identifier:nil handler:^(__kindof UIAction *selected) {
            [owner editProperty:property object:objectID value:option[@"value"]];
          }];
          action.state = option[@"checked"] ? ([option[@"checked"] boolValue] ? UIMenuElementStateOn : UIMenuElementStateOff) :
                                              ([value isEqual:option[@"value"]] ? UIMenuElementStateOn : UIMenuElementStateOff);
          if (action.state == UIMenuElementStateOn) {
            [button setTitle:option[@"label"] forState:UIControlStateNormal];
          }
          [actions addObject:action];
        }
        button.menu = [UIMenu menuWithTitle:property[@"label"] children:actions];
        button.titleLabel.lineBreakMode = NSLineBreakByTruncatingTail;
      }
    }
  }
}

- (void)rebuildRows
{
  NSString *query = _search.text ?: @"";
  NSMutableSet *matches = [NSMutableSet set];
  if (query.length) {
    for (NSDictionary *node in _nodes) {
      if ([node[@"name"] localizedStandardContainsString:query]) {
        NSString *key = node[@"key"];
        while (key.length) {
          [matches addObject:key];
          key = [key stringByDeletingLastPathComponent];
          if ([key isEqual:@"/"]) {
            break;
          }
        }
      }
    }
  }
  NSMutableArray *rows = [NSMutableArray array];
  NSMutableSet *hidden = [NSMutableSet set];
  for (NSDictionary *node in _nodes) {
    if (query.length) {
      if ([matches containsObject:node[@"key"]]) {
        [rows addObject:node];
      }
    }
    else if ([hidden containsObject:node[@"parent"]]) {
      [hidden addObject:node[@"key"]];
    }
    else {
      [rows addObject:node];
      if ([_collapsed containsObject:node[@"key"]]) {
        [hidden addObject:node[@"key"]];
      }
    }
  }
  [_rows release];
  _rows = [rows copy];
  [_tree reloadData];
}

- (void)searchBar:(UISearchBar *)searchBar textDidChange:(NSString *)searchText
{
  [self rebuildRows];
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section
{
  if (tableView == _form) {
    NSSet *expanded = _expandedPanels[_selectedTab ?: @"OBJECT"];
    if (expanded && ![expanded containsObject:_sectionIdentifiers[section]]) {
      return 0;
    }
    return [(NSArray *)_formSections[section] count];
  }
  return _rows.count;
}

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView
{
  return tableView == _form ? _formSections.count : 1;
}

- (NSString *)tableView:(UITableView *)tableView titleForHeaderInSection:(NSInteger)section
{
  return nil;
}

- (UIView *)tableView:(UITableView *)tableView viewForHeaderInSection:(NSInteger)section
{
  if (tableView != _form) {
    return nil;
  }
  NSString *identifier = _sectionIdentifiers[section];
  BOOL expanded = !_selectedTab || [_expandedPanels[_selectedTab] containsObject:identifier];
  UIButton *header = [UIButton buttonWithType:UIButtonTypeSystem];
  UIButtonConfiguration *configuration = [UIButtonConfiguration plainButtonConfiguration];
  configuration.title = _sectionTitles[section];
  configuration.image = [UIImage systemImageNamed:expanded ? @"chevron.down" : @"chevron.right"];
  configuration.preferredSymbolConfigurationForImage = [UIImageSymbolConfiguration configurationWithPointSize:11 weight:UIImageSymbolWeightSemibold];
  configuration.imagePadding = 8;
  configuration.baseForegroundColor = UIColor.labelColor;
  configuration.contentInsets = NSDirectionalEdgeInsetsMake(8, 12, 8, 12);
  header.configuration = configuration;
  header.contentHorizontalAlignment = UIControlContentHorizontalAlignmentLeading;
  header.accessibilityValue = expanded ? @"Expanded" : @"Collapsed";
  __block GHOSTIOSScenePanels *owner = self;
  [header addAction:[UIAction actionWithHandler:^(__kindof UIAction *action) {
    [owner->_inspector endEditing:YES];
    NSMutableSet *open = owner->_expandedPanels[owner->_selectedTab];
    if ([open containsObject:identifier]) {
      [open removeObject:identifier];
    }
    else {
      [open addObject:identifier];
    }
    [owner->_form reloadData];
    [owner requestProperties];
  }] forControlEvents:UIControlEventTouchUpInside];
  UIStackView *switches = _headerControls[section];
  if (!switches.arrangedSubviews.count) {
    return header;
  }
  UIStackView *row = [[[UIStackView alloc] initWithArrangedSubviews:@[header, switches]] autorelease];
  row.alignment = UIStackViewAlignmentCenter;
  row.layoutMarginsRelativeArrangement = YES;
  row.directionalLayoutMargins = NSDirectionalEdgeInsetsMake(0, 0, 0, 16);
  [header setContentCompressionResistancePriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];
  return row;
}

- (CGFloat)tableView:(UITableView *)tableView heightForHeaderInSection:(NSInteger)section
{
  return tableView == _form ? 44 : 0;
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath
{
  if (tableView == _form) {
    UITableViewCell *cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil] autorelease];
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    cell.backgroundColor = UIColor.secondarySystemGroupedBackgroundColor;
    UIView *row = _formSections[indexPath.section][indexPath.row];
    [row removeFromSuperview];
    row.translatesAutoresizingMaskIntoConstraints = NO;
    [cell.contentView addSubview:row];
    [NSLayoutConstraint activateConstraints:@[
      [row.leadingAnchor constraintEqualToAnchor:cell.contentView.leadingAnchor constant:12],
      [row.trailingAnchor constraintEqualToAnchor:cell.contentView.trailingAnchor constant:-12],
      [row.topAnchor constraintEqualToAnchor:cell.contentView.topAnchor constant:2],
      [row.bottomAnchor constraintEqualToAnchor:cell.contentView.bottomAnchor constant:-2],
    ]];
    return cell;
  }
  UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:@"node"];
  if (!cell) {
    cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:@"node"] autorelease];
  }
  NSDictionary *node = _rows[indexPath.row];
  BOOL collection = [node[@"kind"] isEqual:@"collection"];
  UIListContentConfiguration *content = [UIListContentConfiguration cellConfiguration];
  content.text = node[@"name"];
  content.textProperties.numberOfLines = 1;
  content.textProperties.font = [UIFont systemFontOfSize:15 weight:[node[@"active"] boolValue] ? UIFontWeightSemibold : UIFontWeightRegular];
  content.textProperties.color = [node[@"enabled"] boolValue] ? UIColor.labelColor : UIColor.secondaryLabelColor;
  content.image = _icon(node[@"icon"]) ?: [UIImage systemImageNamed:collection ? @"folder" : @"cube"];
  content.imageProperties.maximumSize = CGSizeMake(22, 22);
  content.imageProperties.tintColor = [node[@"active"] boolValue] ? _overlay.tintColor : UIColor.secondaryLabelColor;
  const CGFloat indent = MIN(6, [node[@"depth"] integerValue]) * 12;
  content.directionalLayoutMargins = NSDirectionalEdgeInsetsMake(0, 40 + indent, 0, 12);
  cell.contentConfiguration = content;
  cell.indentationLevel = 0;
  cell.backgroundColor = [node[@"selected"] boolValue] ? [_overlay.tintColor colorWithAlphaComponent:0.14] : UIColor.clearColor;
  cell.accessibilityIdentifier = [@"blender.hierarchy." stringByAppendingString:node[@"id"]];
  cell.accessibilityValue = [node[@"active"] boolValue] ? @"Active" : @"";
  __block GHOSTIOSScenePanels *owner = self;
  cell.accessoryView = nil;
  if (!collection && [node[@"hidden"] boolValue]) {
    UIButton *accessory = [UIButton buttonWithType:UIButtonTypeSystem];
    accessory.tintColor = UIColor.secondaryLabelColor;
    accessory.frame = CGRectMake(0, 0, 44, 44);
    [accessory setImage:[UIImage systemImageNamed:@"eye.slash"] forState:UIControlStateNormal];
    accessory.accessibilityLabel = @"Show in viewport";
    [accessory addAction:[UIAction actionWithHandler:^(__kindof UIAction *action) {
      [owner send:@{@"action": @"visibility", @"id": node[@"id"], @"value": @NO}];
    }] forControlEvents:UIControlEventTouchUpInside];
    cell.accessoryView = accessory;
  }
  [[cell.contentView viewWithTag:9002] removeFromSuperview];
  if (collection || [node[@"children"] boolValue]) {
    UIButton *disclosure = [UIButton buttonWithType:UIButtonTypeSystem];
    UIImageSymbolConfiguration *size = [UIImageSymbolConfiguration configurationWithPointSize:9 weight:UIImageSymbolWeightSemibold];
    [disclosure setImage:[UIImage systemImageNamed:[_collapsed containsObject:node[@"key"]] ? @"arrowtriangle.right.fill" : @"arrowtriangle.down.fill"
                               withConfiguration:size]
               forState:UIControlStateNormal];
    disclosure.tag = 9002;
    disclosure.tintColor = UIColor.secondaryLabelColor;
    disclosure.accessibilityLabel = @"Expand or collapse children";
    [disclosure addAction:[UIAction actionWithHandler:^(__kindof UIAction *action) {
      [owner toggleNode:node];
    }] forControlEvents:UIControlEventTouchUpInside];
    disclosure.translatesAutoresizingMaskIntoConstraints = NO;
    [cell.contentView addSubview:disclosure];
    [NSLayoutConstraint activateConstraints:@[
      [disclosure.leadingAnchor constraintEqualToAnchor:cell.contentView.leadingAnchor constant:indent],
      [disclosure.centerYAnchor constraintEqualToAnchor:cell.contentView.centerYAnchor],
      [disclosure.widthAnchor constraintEqualToConstant:40],
      [disclosure.heightAnchor constraintEqualToConstant:44],
    ]];
  }
  return cell;
}

- (UISwipeActionsConfiguration *)tableView:(UITableView *)tableView
    trailingSwipeActionsConfigurationForRowAtIndexPath:(NSIndexPath *)indexPath
{
  if (tableView == _form || ![_rows[indexPath.row][@"kind"] isEqual:@"object"]) {
    return nil;
  }
  NSDictionary *node = _rows[indexPath.row];
  const BOOL hidden = [node[@"hidden"] boolValue];
  __block GHOSTIOSScenePanels *owner = self;
  UIContextualAction *action = [UIContextualAction contextualActionWithStyle:UIContextualActionStyleNormal
      title:hidden ? @"Show" : @"Hide" handler:^(UIContextualAction *action, UIView *view, void (^complete)(BOOL)) {
        [owner send:@{@"action": @"visibility", @"id": node[@"id"], @"value": @(!hidden)}];
        complete(YES);
      }];
  action.image = [UIImage systemImageNamed:hidden ? @"eye" : @"eye.slash"];
  UISwipeActionsConfiguration *actions = [UISwipeActionsConfiguration configurationWithActions:@[action]];
  actions.performsFirstActionWithFullSwipe = NO;
  return actions;
}

- (void)toggleNode:(NSDictionary *)node
{
  NSString *key = node[@"key"];
  if ([_collapsed containsObject:key]) {
    [_collapsed removeObject:key];
  }
  else {
    [_collapsed addObject:key];
  }
  [self rebuildRows];
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath
{
  if (tableView == _form) {
    return;
  }
  NSDictionary *node = _rows[indexPath.row];
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  if ([node[@"kind"] isEqual:@"collection"]) {
    [self toggleNode:node];
  }
  else if ([node[@"enabled"] boolValue]) {
    [self send:@{@"action": @"select", @"id": node[@"id"]}];
  }
}
@end
