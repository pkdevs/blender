/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#import "GHOST_IOSScenePanels.hh"
#import "GHOST_IOSIcons.hh"
#import "GHOST_IOSNumericInput.hh"
#import <QuartzCore/QuartzCore.h>
#include <cfloat>
#include <cmath>

extern "C" void WM_apple_ios_ui_setting_enqueue(const char *, const char *);

static NSString *blenderNativePropertyLabel(NSDictionary *property)
{
  return [property[@"group"] length] ?
      [NSString stringWithFormat:@"%@ %@", property[@"group"], property[@"label"]] : property[@"label"];
}

@interface GHOSTIOSPropertyField : UITextField
@property(nonatomic, retain) NSDictionary *descriptor;
@property(nonatomic, copy) NSString *objectID;
@property(nonatomic, copy) NSString *originalText;
@property(nonatomic) BOOL scrubbing;
@property(nonatomic) double scrubStart;
@property(nonatomic) double scrubValue;
@property(nonatomic) CGFloat scrubTranslation;
@property(nonatomic, copy) NSString *interactionID;
@property(nonatomic) CFTimeInterval lastPreview;
@end
@implementation GHOSTIOSPropertyField
- (CGRect)textRectForBounds:(CGRect)bounds { return CGRectInset(bounds, 8, 0); }
- (CGRect)editingRectForBounds:(CGRect)bounds { return CGRectInset(bounds, 8, 0); }
- (void)dealloc
{
  [_descriptor release];
  [_objectID release];
  [_originalText release];
  [_interactionID release];
  [super dealloc];
}
@end

@interface GHOSTIOSPropertySlider : UISlider
@property(nonatomic, retain) NSDictionary *descriptor;
@property(nonatomic, copy) NSString *objectID;
@property(nonatomic) BOOL editing;
@property(nonatomic) BOOL changed;
@property(nonatomic, copy) NSString *interactionID;
@property(nonatomic) double startValue;
@property(nonatomic) CFTimeInterval lastPreview;
- (double)propertyValue;
- (void)setPropertyValue:(double)value;
@end

@implementation GHOSTIOSPropertySlider
- (double)propertyValue
{
  NSDictionary *range = self.descriptor[@"slider"];
  double minimum = [range[@"min"] doubleValue], maximum = [range[@"max"] doubleValue];
  double value = minimum + self.value * (maximum - minimum);
  if ([range[@"scale"] isEqual:@"LOG"] && minimum > 0) {
    value = minimum * pow(maximum / minimum, self.value);
  }
  else if ([range[@"scale"] isEqual:@"CUBIC"]) {
    value = (pow(value, 3) - pow(minimum, 3)) / (pow(maximum, 3) - pow(minimum, 3)) *
            (maximum - minimum) + minimum;
  }
  return [self.descriptor[@"type"] isEqual:@"INT"] ? round(value) : value;
}
- (void)setPropertyValue:(double)value
{
  NSDictionary *range = self.descriptor[@"slider"];
  double minimum = [range[@"min"] doubleValue], maximum = [range[@"max"] doubleValue];
  double fraction = (value - minimum) / (maximum - minimum);
  if ([range[@"scale"] isEqual:@"LOG"] && minimum > 0) {
    fraction = value > 0 ? log(value / minimum) / log(maximum / minimum) : 0;
  }
  else if ([range[@"scale"] isEqual:@"CUBIC"]) {
    const double cube = fraction * (pow(maximum, 3) - pow(minimum, 3)) + pow(minimum, 3);
    fraction = (cbrt(cube) - minimum) / (maximum - minimum);
  }
  self.value = std::isfinite(fraction) ? fmin(1, fmax(0, fraction)) : 0;
}
- (void)dealloc
{
  [_descriptor release];
  [_objectID release];
  [_interactionID release];
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

/* Thumbnails Blender renders for preview enums (MatCaps, HDRIs, studio lights). */
static UIImage *blenderPreviewImage(NSString *path)
{
  static NSCache *cache = nil;
  static dispatch_once_t once;
  dispatch_once(&once, ^{ cache = [[NSCache alloc] init]; });
  if (![path isKindOfClass:NSString.class] || !path.length) { return nil; }
  UIImage *image = [cache objectForKey:path];
  if (!image && (image = [UIImage imageWithContentsOfFile:path])) {
    [cache setObject:image forKey:path];
  }
  return image;
}

static BOOL blenderOptionsHavePreviews(NSArray *options)
{
  for (NSDictionary *option in options) {
    if ([option[@"preview"] length]) { return YES; }
  }
  return NO;
}

/* Grid of previews, like Blender's template_icon_view popup. */
@interface GHOSTIOSPreviewGrid : UICollectionViewController
@property(nonatomic, copy) NSArray *options;
@property(nonatomic, copy) NSString *value;
@property(nonatomic, copy) void (^choose)(NSDictionary *option);
@end

@implementation GHOSTIOSPreviewGrid
- (instancetype)init
{
  NSCollectionLayoutSize *size = [NSCollectionLayoutSize
      sizeWithWidthDimension:[NSCollectionLayoutDimension absoluteDimension:88]
             heightDimension:[NSCollectionLayoutDimension absoluteDimension:112]];
  NSCollectionLayoutItem *item = [NSCollectionLayoutItem itemWithLayoutSize:size];
  NSCollectionLayoutGroup *row = [NSCollectionLayoutGroup horizontalGroupWithLayoutSize:
      [NSCollectionLayoutSize sizeWithWidthDimension:[NSCollectionLayoutDimension fractionalWidthDimension:1]
                                     heightDimension:[NSCollectionLayoutDimension absoluteDimension:112]]
      repeatingSubitem:item count:4];
  row.interItemSpacing = [NSCollectionLayoutSpacing flexibleSpacing:8];
  NSCollectionLayoutSection *section = [NSCollectionLayoutSection sectionWithGroup:row];
  section.interGroupSpacing = 8;
  section.contentInsets = NSDirectionalEdgeInsetsMake(12, 16, 16, 16);
  if ((self = [super initWithCollectionViewLayout:[[[UICollectionViewCompositionalLayout alloc]
                                                     initWithSection:section] autorelease]])) {
    [self.collectionView registerClass:UICollectionViewCell.class forCellWithReuseIdentifier:@"preview"];
    self.collectionView.backgroundColor = UIColor.clearColor;
    self.collectionView.accessibilityIdentifier = @"blender.preview.grid";
  }
  return self;
}
- (void)dealloc
{
  [_options release];
  [_value release];
  [_choose release];
  [super dealloc];
}
- (NSInteger)collectionView:(UICollectionView *)view numberOfItemsInSection:(NSInteger)section
{
  return self.options.count;
}
- (UICollectionViewCell *)collectionView:(UICollectionView *)view cellForItemAtIndexPath:(NSIndexPath *)path
{
  UICollectionViewCell *cell = [view dequeueReusableCellWithReuseIdentifier:@"preview" forIndexPath:path];
  NSDictionary *option = self.options[path.item];
  const BOOL current = [option[@"value"] isEqual:self.value];
  UIListContentConfiguration *content = [UIListContentConfiguration cellConfiguration];
  content.image = blenderPreviewImage(option[@"preview"]);
  content.imageProperties.maximumSize = CGSizeMake(80, 80);
  content.imageProperties.reservedLayoutSize = CGSizeMake(80, 80);
  content.imageProperties.cornerRadius = 10;
  content.text = option[@"label"];
  content.textProperties.font = [UIFont preferredFontForTextStyle:UIFontTextStyleCaption2];
  content.textProperties.alignment = UIListContentTextAlignmentCenter;
  content.textProperties.numberOfLines = 1;
  content.textProperties.color = current ? cell.tintColor : UIColor.secondaryLabelColor;
  content.axesPreservingSuperviewLayoutMargins = 0;
  content.directionalLayoutMargins = NSDirectionalEdgeInsetsMake(4, 4, 4, 4);
  content.imageToTextPadding = 4;
  cell.contentConfiguration = content;
  UIBackgroundConfiguration *background = [UIBackgroundConfiguration clearConfiguration];
  background.cornerRadius = 12;
  background.strokeColor = current ? cell.tintColor : UIColor.clearColor;
  background.strokeWidth = 2;
  cell.backgroundConfiguration = background;
  cell.accessibilityLabel = option[@"label"];
  cell.accessibilityTraits = UIAccessibilityTraitButton | (current ? UIAccessibilityTraitSelected : 0);
  return cell;
}
- (void)collectionView:(UICollectionView *)view didSelectItemAtIndexPath:(NSIndexPath *)path
{
  NSDictionary *option = self.options[path.item];
  self.value = option[@"value"];
  [view reloadData];
  if (self.choose) { self.choose(option); }
  if (self.navigationController.viewControllers.firstObject != self) {
    [self.navigationController popViewControllerAnimated:YES];
  }
  else {
    [self dismissViewControllerAnimated:YES completion:nil];
  }
}
@end

/* The current preview and its name; tapping opens the grid. */
@interface GHOSTIOSPreviewButton : UIButton
@property(nonatomic, retain) NSDictionary *descriptor;
@property(nonatomic, copy) NSString *value;
- (void)showValue:(NSString *)value options:(NSArray *)options;
@end

@implementation GHOSTIOSPreviewButton
- (void)dealloc
{
  [_descriptor release];
  [_value release];
  [super dealloc];
}
- (void)showValue:(NSString *)value options:(NSArray *)options
{
  self.value = value;
  NSDictionary *current = nil;
  for (NSDictionary *option in options) {
    if ([option[@"value"] isEqual:value]) { current = option; }
  }
  UIButtonConfiguration *config = self.configuration ?: [UIButtonConfiguration plainButtonConfiguration];
  UIImage *preview = blenderPreviewImage(current[@"preview"]);
  if (preview) {
    UIGraphicsImageRenderer *renderer = [[[UIGraphicsImageRenderer alloc] initWithSize:CGSizeMake(56, 56)] autorelease];
    preview = [renderer imageWithActions:^(UIGraphicsImageRendererContext *context) {
      [[UIBezierPath bezierPathWithRoundedRect:CGRectMake(0, 0, 56, 56) cornerRadius:8] addClip];
      [preview drawInRect:CGRectMake(0, 0, 56, 56)];
    }];
  }
  config.image = [preview imageWithRenderingMode:UIImageRenderingModeAlwaysOriginal];
  config.imagePadding = 12;
  config.title = current[@"label"] ?: value;
  config.baseForegroundColor = UIColor.labelColor;
  config.contentInsets = NSDirectionalEdgeInsetsMake(4, 0, 4, 0);
  self.configuration = config;
  self.contentHorizontalAlignment = UIControlContentHorizontalAlignmentLeading;
  self.accessibilityValue = current[@"label"];
}
@end

@interface GHOSTIOSScenePanels () <UITableViewDataSource, UITableViewDelegate, UICollectionViewDelegate,
                                   UISearchBarDelegate, UITextFieldDelegate,
                                   UIColorPickerViewControllerDelegate, UIPopoverPresentationControllerDelegate,
                                   UIContextMenuInteractionDelegate, UIGestureRecognizerDelegate>
@end

/* Horizontal strip whose edges fade out where more content is scrolled out of view. */
@interface GHOSTIOSFadingScrollView : UIScrollView
@end

@implementation GHOSTIOSFadingScrollView

- (void)layoutSubviews
{
  [super layoutSubviews];
  const CGFloat width = self.bounds.size.width;
  const CGFloat hidden_leading = self.contentOffset.x + self.adjustedContentInset.left;
  const CGFloat hidden_trailing = self.contentSize.width + self.adjustedContentInset.right -
                                  self.contentOffset.x - width;
  if (width <= 0 || (hidden_leading <= 0.5 && hidden_trailing <= 0.5)) {
    self.layer.mask = nil;
    return;
  }
  CAGradientLayer *mask = [self.layer.mask isKindOfClass:CAGradientLayer.class] ?
                              (CAGradientLayer *)self.layer.mask : nil;
  if (!mask) {
    mask = [CAGradientLayer layer];
    mask.startPoint = CGPointMake(0, 0.5);
    mask.endPoint = CGPointMake(1, 0.5);
    self.layer.mask = mask;
  }
  /* Fade in proportion to how much is hidden, so the edge clears as the end is reached. */
  const CGFloat fade = MIN(32.0, width / 4);
  const CGFloat leading = MIN(fade, MAX(0, hidden_leading)) / width;
  const CGFloat trailing = MIN(fade, MAX(0, hidden_trailing)) / width;
  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  mask.frame = self.bounds;
  id opaque = (id)UIColor.blackColor.CGColor, clear = (id)UIColor.clearColor.CGColor;
  mask.colors = @[leading > 0 ? clear : opaque, opaque, opaque, trailing > 0 ? clear : opaque];
  mask.locations = @[@0, @(leading), @(1 - trailing), @1];
  [CATransaction commit];
}

@end

static const NSInteger blenderDisclosureChevronTag = 0x6368;

/* Disclosure chevron. Only a fixed-frame glyph inside an untransformed, constrained
 * container rotates: layout passes over a transformed view resize it and stretch the glyph. */
static UIView *blenderDisclosureGlyph(UIColor *color, BOOL expanded)
{
  const CGFloat size = 14;
  UIView *container = [[[UIView alloc] init] autorelease];
  container.userInteractionEnabled = NO;
  container.translatesAutoresizingMaskIntoConstraints = NO;
  UIImageView *chevron = [[[UIImageView alloc] initWithImage:[UIImage systemImageNamed:@"chevron.right"
      withConfiguration:[UIImageSymbolConfiguration configurationWithPointSize:11 weight:UIImageSymbolWeightSemibold]]] autorelease];
  chevron.frame = CGRectMake(0, 0, size, size);
  chevron.autoresizingMask = UIViewAutoresizingNone;
  chevron.contentMode = UIViewContentModeCenter;
  chevron.tintColor = color;
  chevron.tag = blenderDisclosureChevronTag;
  chevron.transform = CGAffineTransformMakeRotation(expanded ? M_PI_2 : 0);
  [container addSubview:chevron];
  [NSLayoutConstraint activateConstraints:@[
    [container.widthAnchor constraintEqualToConstant:size],
    [container.heightAnchor constraintEqualToConstant:size],
  ]];
  return container;
}

static void blenderRotateDisclosure(UIView *owner, BOOL expanded)
{
  UIView *chevron = [owner viewWithTag:blenderDisclosureChevronTag];
  [UIView animateWithDuration:UIAccessibilityIsReduceMotionEnabled() ? 0 : 0.25
      delay:0 options:UIViewAnimationOptionBeginFromCurrentState | UIViewAnimationOptionAllowUserInteraction
      animations:^{ chevron.transform = CGAffineTransformMakeRotation(expanded ? M_PI_2 : 0); }
      completion:nil];
}

/* Panel disclosure header. */
static UIButton *blenderDisclosureButton(NSString *title, UIFont *font, UIColor *color, CGFloat indent, BOOL expanded)
{
  UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
  UIButtonConfiguration *configuration = [UIButtonConfiguration plainButtonConfiguration];
  configuration.title = title;
  configuration.baseForegroundColor = color;
  if (font) {
    configuration.titleTextAttributesTransformer = ^NSDictionary *(NSDictionary *attributes) {
      NSMutableDictionary *result = [[attributes mutableCopy] autorelease];
      result[NSFontAttributeName] = font;
      return result;
    };
  }
  configuration.contentInsets = NSDirectionalEdgeInsetsMake(8, indent + 20, 8, 12);
  button.configuration = configuration;
  button.contentHorizontalAlignment = UIControlContentHorizontalAlignmentLeading;
  button.accessibilityValue = expanded ? @"Expanded" : @"Collapsed";
  UIView *container = blenderDisclosureGlyph(color, expanded);
  [button addSubview:container];
  [NSLayoutConstraint activateConstraints:@[
    [container.leadingAnchor constraintEqualToAnchor:button.leadingAnchor constant:indent + 1],
    [container.centerYAnchor constraintEqualToAnchor:button.centerYAnchor],
  ]];
  return button;
}

static void blenderSetDisclosureExpanded(UIButton *button, BOOL expanded)
{
  button.accessibilityValue = expanded ? @"Expanded" : @"Collapsed";
  blenderRotateDisclosure(button, expanded);
}

@implementation GHOSTIOSScenePanels {
  UIView *_overlay;
  UIImage *(^_icon)(NSString *);
  UIBarButtonItem *_inspectorButton, *_hierarchyButton;
  UIVisualEffectView *_inspector, *_hierarchy;
  UILabel *_inspectorTitle, *_status;
  UIScrollView *_inspectorScroll;
  UITableView *_form;
  NSMutableArray *_formSections;
  /* Stable identity per row, parallel to _formSections, so updates animate by meaning. */
  NSMutableArray *_formRowKeys;
  /* Section id -> persistent header container, refilled in place when controls change. */
  NSMutableDictionary *_headerContainers;
  NSMutableArray *_headerControls;
  NSArray *_sectionTitles;
  /* Sections for the active item of a stack, such as the active modifier. */
  NSSet *_activeSections;
  NSSet *_headerlessSections;
  NSArray *_sectionIdentifiers, *_tabs;
  NSString *_selectedTab;
  NSMutableDictionary *_expandedPanels;
  /* Per tab: sub-panel id -> @YES/@NO once the user toggles it. */
  NSMutableDictionary *_subpanelStates;
  UIScrollView *_tabsScroll;
  UIStackView *_tabsStack;
  UICollectionView *_tree;
  UICollectionViewDiffableDataSource<NSString *, NSString *> *_treeDataSource;
  NSDictionary *_nodesByKey;
  UILabel *_hierarchyStatus;
  UIButton *_hierarchyAdd;
  BOOL _multipleSelection;
  UISearchBar *_search;
  NSMutableSet *_collapsed;
  NSArray *_nodes, *_schema;
  NSDictionary *_snapshot;
  NSString *_objectID;
  NSMutableDictionary *_controls;
  NSMutableDictionary *_sliders;
  NSMutableDictionary *_lockIndicators;
  NSMutableDictionary *_groupHeadings;
  NSMutableDictionary *_sectionViewCache;
  NSMutableDictionary *_disclosureButtons;
  uint64_t _sequence;
  BOOL _showInspector, _showHierarchy;
  CGRect _layoutRect;
  UIColorPickerViewController *_colorPicker;
  GHOSTIOSColorButton *_colorButton;
  BOOL _colorPending;
  GHOSTIOSPropertyField *_scrubField;
  NSString *_colorInteraction;
  CFTimeInterval _colorPreviewTime;
  BOOL _shadingOnly;
  GHOSTIOSScenePanels *_shadingPanels;
  UINavigationController *_shadingController;
  BOOL _toolInline;
  NSLayoutConstraint *_toolStripWidth;
  CGFloat _toolStripContentWidth;
  GHOSTIOSScenePanels *_toolPanels, *_toolPopoverPanels;
  UIBarButtonItemGroup *_toolGroup;
  UINavigationController *_toolPopoverController;
}

@synthesize menuProvider = _menuProvider;

- (instancetype)initWithOverlay:(UIView *)overlay icon:(UIImage *(^)(NSString *))icon
{
  if ((self = [super init])) {
    _overlay = overlay;
    _icon = [icon copy];
    _collapsed = [[NSMutableSet alloc] init];
    _controls = [[NSMutableDictionary alloc] init];
    _sliders = [[NSMutableDictionary alloc] init];
    _lockIndicators = [[NSMutableDictionary alloc] init];
    _groupHeadings = [[NSMutableDictionary alloc] init];
    _sectionViewCache = [[NSMutableDictionary alloc] init];
    _disclosureButtons = [[NSMutableDictionary alloc] init];
    _expandedPanels = [[NSMutableDictionary alloc] init];
    _subpanelStates = [[NSMutableDictionary alloc] init];
    /* Blender's own editor-type icons, as in the area header's editor menu. */
    _inspectorButton = [[UIBarButtonItem alloc]
        initWithImage:_icon(@"PROPERTIES") ?: [UIImage systemImageNamed:@"sidebar.right"]
        style:UIBarButtonItemStylePlain target:self action:@selector(toggleInspector:)];
    _hierarchyButton = [[UIBarButtonItem alloc]
        initWithImage:_icon(@"OUTLINER") ?: [UIImage systemImageNamed:@"list.bullet.indent"]
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
    _tabsScroll = [[[GHOSTIOSFadingScrollView alloc] init] autorelease];
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
    _formRowKeys = [[NSMutableArray alloc] init];
    _headerContainers = [[NSMutableDictionary alloc] init];
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
    __block GHOSTIOSScenePanels *owner = self;
    UICollectionLayoutListConfiguration *list = [[[UICollectionLayoutListConfiguration alloc]
        initWithAppearance:UICollectionLayoutListAppearanceSidebar] autorelease];
    list.backgroundColor = UIColor.clearColor;
    list.trailingSwipeActionsConfigurationProvider = ^UISwipeActionsConfiguration *(NSIndexPath *path) {
      return [owner hierarchySwipeActionsAtIndexPath:path leading:NO];
    };
    list.leadingSwipeActionsConfigurationProvider = ^UISwipeActionsConfiguration *(NSIndexPath *path) {
      return [owner hierarchySwipeActionsAtIndexPath:path leading:YES];
    };
    _tree = [[[UICollectionView alloc] initWithFrame:CGRectZero
        collectionViewLayout:[UICollectionViewCompositionalLayout layoutWithListConfiguration:list]] autorelease];
    _tree.backgroundColor = UIColor.clearColor;
    _tree.delegate = self;
    _tree.allowsMultipleSelection = YES;
    _tree.keyboardDismissMode = UIScrollViewKeyboardDismissModeOnDrag;
    _tree.accessibilityIdentifier = @"blender.hierarchy.outline";
    [_tree registerClass:UICollectionViewListCell.class forCellWithReuseIdentifier:@"node"];
    _treeDataSource = [[UICollectionViewDiffableDataSource alloc] initWithCollectionView:_tree
        cellProvider:^UICollectionViewCell *(UICollectionView *view, NSIndexPath *path, NSString *key) {
      UICollectionViewListCell *cell = [view dequeueReusableCellWithReuseIdentifier:@"node" forIndexPath:path];
      [owner configureHierarchyCell:cell key:key];
      return cell;
    }];
    UICollectionViewDiffableDataSourceSectionSnapshotHandlers *handlers =
        [[[UICollectionViewDiffableDataSourceSectionSnapshotHandlers alloc] init] autorelease];
    handlers.willExpandItemHandler = ^(NSString *key) {
      if (!owner->_search.text.length) { [owner->_collapsed removeObject:key]; }
      GHOSTIOSScenePanels *panels = owner;
      dispatch_async(dispatch_get_main_queue(), ^{ [panels synchronizeHierarchySelection]; });
    };
    handlers.willCollapseItemHandler = ^(NSString *key) {
      if (!owner->_search.text.length) { [owner->_collapsed addObject:key]; }
    };
    _treeDataSource.sectionSnapshotHandlers = handlers;
    [_hierarchy.contentView addSubview:_tree];
    _hierarchyStatus = [self label:@"" style:UIFontTextStyleFootnote];
    _hierarchyStatus.textColor = UIColor.systemRedColor;
    _hierarchyStatus.hidden = YES;
    [_hierarchy.contentView addSubview:_hierarchyStatus];
  }
  return self;
}

- (void)dealloc
{
  _shadingController.presentationController.delegate = nil;
  [_toolPanels release];
  _toolPopoverController.presentationController.delegate = nil;
  [_toolPopoverController dismissViewControllerAnimated:NO completion:nil];
  [_toolPopoverController release];
  [_toolPopoverPanels release];
  [_toolGroup release];
  [_toolStripWidth release];
  _shadingController.popoverPresentationController.delegate = nil;
  [_shadingPanels release];
  [_shadingController release];
  [_scrubField release];
  [_colorInteraction release];
  _tree.delegate = nil;
  _tree.dataSource = nil;
  [_treeDataSource release];
  [_nodesByKey release];
  [_inspector removeFromSuperview];
  [_hierarchy removeFromSuperview];
  [_icon release];
  [_inspectorButton release];
  [_hierarchyButton release];
  [_inspector release];
  [_hierarchy release];
  [_collapsed release];
  [_controls release];
  [_sliders release];
  [_lockIndicators release];
  [_groupHeadings release];
  [_sectionViewCache release];
  [_disclosureButtons release];
  [_formSections release];
  [_formRowKeys release];
  [_headerContainers release];
  [_headerControls release];
  [_menuProvider release];
  [_sectionTitles release];
  [_activeSections release];
  [_headerlessSections release];
  [_sectionIdentifiers release];
  [_tabs release];
  [_selectedTab release];
  [_expandedPanels release];
  [_subpanelStates release];
  [_nodes release];
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

- (void)setMenuProvider:(UIMenu *(^)(NSDictionary *))provider
{
  if (_menuProvider == provider) { return; }
  [_menuProvider release];
  _menuProvider = [provider copy];
  UIMenu *objects = provider ? provider(@{@"label": @"Add Object",
      @"menu": @"VIEW3D_MT_add", @"icon": @"OBJECT_DATA"}) : nil;
  UIAction *collection = (UIAction *)_hierarchyAdd.menu.children.firstObject;
  _hierarchyAdd.menu = [UIMenu menuWithChildren:objects ? @[collection, objects] : @[collection]];
}

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
  UIView *titleTrailing = close;
  if (selector == @selector(toggleHierarchy:)) {
    _hierarchyAdd = [UIButton buttonWithType:UIButtonTypeSystem];
    [_hierarchyAdd setImage:[UIImage systemImageNamed:@"plus"] forState:UIControlStateNormal];
    _hierarchyAdd.accessibilityLabel = @"Add Collection or Object";
    _hierarchyAdd.accessibilityIdentifier = @"blender.hierarchy.add";
    _hierarchyAdd.showsMenuAsPrimaryAction = YES;
    __block GHOSTIOSScenePanels *owner = self;
    UIAction *collection = [UIAction actionWithTitle:@"New Collection"
        image:_icon(@"OUTLINER_COLLECTION") identifier:@"blender.hierarchy.newCollection"
        handler:^(__kindof UIAction *action) { [owner promptNewCollection]; }];
    _hierarchyAdd.menu = [UIMenu menuWithChildren:@[collection]];
    _hierarchyAdd.translatesAutoresizingMaskIntoConstraints = NO;
    [panel.contentView addSubview:_hierarchyAdd];
    [NSLayoutConstraint activateConstraints:@[
      [_hierarchyAdd.trailingAnchor constraintEqualToAnchor:close.leadingAnchor],
      [_hierarchyAdd.centerYAnchor constraintEqualToAnchor:close.centerYAnchor],
      [_hierarchyAdd.widthAnchor constraintEqualToConstant:44],
      [_hierarchyAdd.heightAnchor constraintEqualToConstant:44],
    ]];
    titleTrailing = _hierarchyAdd;
  }
  [NSLayoutConstraint activateConstraints:@[
    [label.leadingAnchor constraintEqualToAnchor:panel.contentView.leadingAnchor constant:16],
    [label.centerYAnchor constraintEqualToAnchor:close.centerYAnchor],
    [label.trailingAnchor constraintLessThanOrEqualToAnchor:titleTrailing.leadingAnchor constant:-8],
    [close.topAnchor constraintEqualToAnchor:panel.contentView.topAnchor constant:5],
    [close.trailingAnchor constraintEqualToAnchor:panel.contentView.trailingAnchor constant:-5],
    [close.widthAnchor constraintEqualToConstant:44], [close.heightAnchor constraintEqualToConstant:44],
  ]];
  [_overlay addSubview:panel];
  return panel;
}

- (void)animatePanel:(UIView *)panel visible:(BOOL)visible
{
  CGAffineTransform hiddenTransform = UIAccessibilityIsReduceMotionEnabled() ? CGAffineTransformIdentity :
      CGAffineTransformConcat(CGAffineTransformMakeTranslation(panel == _hierarchy ? -14 : 14, 0),
                              CGAffineTransformMakeScale(0.98, 0.98));
  panel.userInteractionEnabled = visible;
  if (visible) {
    [_overlay bringSubviewToFront:panel];
    if (panel.hidden) {
      panel.alpha = 0;
      panel.transform = hiddenTransform;
    }
    panel.hidden = NO;
  }
  [UIView animateWithDuration:UIAccessibilityIsReduceMotionEnabled() ? 0 : 0.28
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
}

- (void)applyVisibility
{
  if (!_showInspector) {
    for (GHOSTIOSPropertySlider *slider in _sliders.allValues) {
      if (slider.editing) { [self finishSlider:slider]; }
    }
    [self cancelNumberScrub];
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
  [_form endEditing:YES];
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
    [_form endEditing:YES];
  }
  [self applyVisibility];
}

- (void)dismiss
{
  [self dismissShading];
  [_form endEditing:YES];
  [_hierarchy endEditing:YES];
  _showInspector = _showHierarchy = NO;
  [self applyVisibility];
}

- (UIBarButtonItemGroup *)updateToolSettings:(NSDictionary *)settings acknowledgement:(NSNumber *)ack
{
  if (!_toolPanels) {
    _toolPanels = [[GHOSTIOSScenePanels alloc] initWithOverlay:_overlay icon:_icon];
    _toolPanels->_toolInline = YES;
    _toolPanels->_showInspector = YES;
    _toolPanels->_selectedTab = [@"TOOL" copy];
    _toolPanels.menuProvider = self.menuProvider;
  }
  if (![_toolPanels->_objectID isEqual:settings[@"id"]]) { [self hideToolSettings]; }
  NSDictionary *snapshot = @{@"inspector": settings ?: @{}, @"ack": ack ?: @0};
  [_toolPanels updateSnapshot:snapshot];
  [_toolPanels updateToolStripWidth];
  [_toolPanels->_toolPopoverPanels updateSnapshot:snapshot];
  return _toolPanels->_toolGroup;
}

- (void)updateToolStripWidth
{
  /* Leave room for the bar's leading and trailing items; the strip scrolls for the rest. */
  const CGFloat available = MAX(160.0, _overlay.bounds.size.width - 620.0);
  const CGFloat width = MIN(_toolStripContentWidth, available);
  if (_toolStripWidth && fabs(_toolStripWidth.constant - width) > 0.5) {
    _toolStripWidth.constant = width;
  }
}

- (void)hideToolSettings
{
  GHOSTIOSScenePanels *panels = _toolInline ? self : _toolPanels;
  if (!panels) { return; }
  for (UIControl *control in panels->_controls.allValues) { [control endEditing:YES]; }
  [panels cancelNumberScrub];
  [panels closeColorPicker];
  [panels dismissToolSettings];
}

- (void)presentToolSettings:(UIBarButtonItem *)button
{
  UIViewController *presenter = _overlay.window.rootViewController;
  if (!presenter || presenter.presentedViewController) { return; }
  if (!_toolPopoverController) {
    UIViewController *content = [[[UIViewController alloc] init] autorelease];
    content.title = @"Tool Settings";
    /* Let the popover's own glass show through, matching the inspector and rails. */
    content.view.backgroundColor = UIColor.clearColor;
    _toolPopoverPanels = [[GHOSTIOSScenePanels alloc] initWithOverlay:content.view icon:_icon];
    _toolPopoverPanels->_selectedTab = [@"TOOL" copy];
    _toolPopoverPanels.menuProvider = self.menuProvider;
    _toolPopoverPanels->_showInspector = YES;
    [_toolPopoverPanels->_inspector removeFromSuperview];
    UITableView *form = _toolPopoverPanels->_form;
    [form removeFromSuperview];
    [content.view addSubview:form];
    [NSLayoutConstraint activateConstraints:@[
      [form.leadingAnchor constraintEqualToAnchor:content.view.leadingAnchor],
      [form.trailingAnchor constraintEqualToAnchor:content.view.trailingAnchor],
      [form.topAnchor constraintEqualToAnchor:content.view.safeAreaLayoutGuide.topAnchor],
      [form.bottomAnchor constraintEqualToAnchor:content.view.bottomAnchor],
    ]];
    content.navigationItem.rightBarButtonItem = [[[UIBarButtonItem alloc]
        initWithBarButtonSystemItem:UIBarButtonSystemItemDone target:self action:@selector(dismissToolSettings)] autorelease];
    _toolPopoverController = [[UINavigationController alloc] initWithRootViewController:content];
  }
  [_toolPopoverPanels updateSnapshot:_snapshot];
  /* Fit the popover to the few settings a tool has rather than a fixed sheet. */
  UITableView *form = _toolPopoverPanels->_form;
  form.frame = CGRectMake(0, 0, 320, 1000);
  [form layoutIfNeeded];
  _toolPopoverController.preferredContentSize = CGSizeMake(320, MIN(420.0, MAX(88.0, form.contentSize.height)));
  _toolPopoverController.modalPresentationStyle = UIModalPresentationPopover;
  _toolPopoverController.popoverPresentationController.barButtonItem = button;
  _toolPopoverController.presentationController.delegate = self;
  [presenter presentViewController:_toolPopoverController animated:YES completion:nil];
}

- (void)dismissToolSettings
{
  if (!_toolPopoverPanels) { return; }
  [_toolPopoverPanels->_form endEditing:YES];
  [_toolPopoverPanels cancelNumberScrub];
  [_toolPopoverPanels closeColorPicker];
  [_toolPopoverController dismissViewControllerAnimated:YES completion:nil];
}

- (void)presentShadingFrom:(UIBarButtonItem *)button controller:(UIViewController *)controller
{
  if (_shadingController.presentingViewController || controller.presentedViewController) { return; }
  if (!_shadingController) {
    UIViewController *content = [[[UIViewController alloc] init] autorelease];
    content.title = @"Viewport Shading";
    /* Let the popover's own glass show through, matching the inspector and rails. */
    content.view.backgroundColor = UIColor.clearColor;
    _shadingPanels = [[GHOSTIOSScenePanels alloc] initWithOverlay:content.view icon:_icon];
    _shadingPanels->_shadingOnly = YES;
    _shadingPanels->_selectedTab = [@"SHADING" copy];
    [_shadingPanels->_inspector removeFromSuperview];
    UITableView *form = _shadingPanels->_form;
    UIActivityIndicatorView *loading = [[[UIActivityIndicatorView alloc]
        initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium] autorelease];
    [loading startAnimating];
    form.backgroundView = loading;
    [form removeFromSuperview];
    [content.view addSubview:form];
    [NSLayoutConstraint activateConstraints:@[
      [form.leadingAnchor constraintEqualToAnchor:content.view.leadingAnchor],
      [form.trailingAnchor constraintEqualToAnchor:content.view.trailingAnchor],
      [form.topAnchor constraintEqualToAnchor:content.view.safeAreaLayoutGuide.topAnchor],
      [form.bottomAnchor constraintEqualToAnchor:content.view.bottomAnchor],
    ]];
    content.navigationItem.rightBarButtonItem = [[[UIBarButtonItem alloc]
        initWithBarButtonSystemItem:UIBarButtonSystemItemDone target:self action:@selector(dismissShading)] autorelease];
    _shadingController = [[UINavigationController alloc] initWithRootViewController:content];
    _shadingController.preferredContentSize = CGSizeMake(400, 640);
  }
  _shadingPanels->_showInspector = YES;
  [_shadingPanels requestProperties];
  WM_apple_ios_ui_setting_enqueue("native_shading_visible", "1");
  _shadingController.modalPresentationStyle = UIModalPresentationPopover;
  _shadingController.popoverPresentationController.barButtonItem = button;
  _shadingController.popoverPresentationController.delegate = self;
  _shadingController.presentationController.delegate = self;
  [controller presentViewController:_shadingController animated:YES completion:nil];
}

- (void)dismissShading
{
  if (!_shadingPanels || !_shadingPanels->_showInspector) { return; }
  for (GHOSTIOSPropertySlider *slider in _shadingPanels->_sliders.allValues) {
    if (slider.editing) { [_shadingPanels finishSlider:slider]; }
  }
  [_shadingPanels cancelNumberScrub];
  [_shadingPanels closeColorPicker];
  [_shadingPanels->_form endEditing:YES];
  _shadingPanels->_showInspector = NO;
  WM_apple_ios_ui_setting_enqueue("native_shading_visible", "0");
  [_shadingController dismissViewControllerAnimated:YES completion:nil];
}

- (void)layoutInRect:(CGRect)rect
{
  if (_shadingOnly || _toolInline || [_selectedTab isEqual:@"TOOL"]) { return; }
  _layoutRect = rect;
  if (rect.size.width < 700 && _showInspector && _showHierarchy) {
    _showHierarchy = NO;
    [self applyVisibility];
    return;
  }
  const CGFloat width = MIN(336, MAX(0, rect.size.width - 24));
  const CGFloat height = MIN(620, MAX(0, rect.size.height - 24));
  /* Frame is undefined while transformed; layout must not disturb the presentation animation. */
  _hierarchy.bounds = _inspector.bounds = CGRectMake(0, 0, width, height);
  _hierarchy.center = CGPointMake(rect.origin.x + 12 + width / 2, rect.origin.y + 12 + height / 2);
  _inspector.center = CGPointMake(CGRectGetMaxX(rect) - 12 - width / 2, rect.origin.y + 12 + height / 2);
  _search.frame = CGRectMake(4, 52, width - 8, 52);
  CGFloat statusHeight = _hierarchyStatus.hidden ? 0 : 60;
  _tree.frame = CGRectMake(0, 106, width, MAX(0, height - 106 - statusHeight));
  _hierarchyStatus.frame = CGRectMake(16, height - statusHeight, width - 32, statusHeight);
}

- (void)send:(NSDictionary *)request
{
  static uint64_t sequence = 0;
  NSMutableDictionary *payload = [[request mutableCopy] autorelease];
  payload[@"sequence"] = @(_sequence = ++sequence);
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

- (void)previewProperty:(NSDictionary *)property object:(NSString *)objectID value:(id)value
            interaction:(NSString *)interaction phase:(NSString *)phase
{
  NSMutableDictionary *request = [[property mutableCopy] autorelease];
  request[@"action"] = @"property";
  request[@"id"] = objectID;
  request[@"value"] = value;
  request[@"interaction"] = interaction;
  request[@"phase"] = phase;
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

- (NSDictionary *)shapeForProperty:(NSDictionary *)property
{
  NSMutableDictionary *shape = [[property mutableCopy] autorelease];
  for (NSString *key in @[@"value", @"exposure", @"editable"]) {
    [shape removeObjectForKey:key];
  }
  if (shape[@"animation"]) {
    shape[@"animation"] = @YES;
  }
  for (NSString *key in @[@"lock", @"group_menu"]) {
    if (shape[key]) {
      shape[key] = [self shapeForProperty:shape[key]];
    }
  }
  if (shape[@"options"]) {
    NSMutableArray *options = [NSMutableArray array];
    for (NSDictionary *option in shape[@"options"]) {
      NSMutableDictionary *item = [[option mutableCopy] autorelease];
      [item removeObjectForKey:@"checked"];
      [options addObject:item];
    }
    shape[@"options"] = options;
  }
  return shape;
}

- (NSArray *)shapeForSection:(NSDictionary *)section
{
  NSMutableArray *shape = [NSMutableArray arrayWithObjects:section[@"id"] ?: section[@"title"], section[@"title"],
                           section[@"active"] ?: @0, nil];
  for (NSDictionary *property in section[@"properties"]) {
    [shape addObject:[self shapeForProperty:property]];
  }
  return shape;
}

- (void)rebuildInspector:(NSDictionary *)inspector preservingSections:(BOOL)preserve
{
  if (!preserve) {
    for (GHOSTIOSPropertySlider *slider in _sliders.allValues) {
      if (slider.editing) { [self finishSlider:slider]; }
    }
    [self closeColorPicker];
    [_sectionViewCache removeAllObjects];
    [_disclosureButtons removeAllObjects];
  }
  NSArray *oldIdentifiers = [[_sectionIdentifiers copy] autorelease];
  NSArray *oldRows = [[_formSections copy] autorelease];
  NSArray *oldRowKeys = [[_formRowKeys copy] autorelease];
  NSMutableDictionary *nextCache = [NSMutableDictionary dictionary];
  [_headerControls release];
  _headerControls = [[NSMutableArray alloc] init];
  [_formSections removeAllObjects];
  [_formRowKeys removeAllObjects];
  NSMutableArray *titles = [NSMutableArray array];
  NSMutableArray *identifiers = [NSMutableArray array];
  NSMutableSet *actives = [NSMutableSet set];
  NSMutableSet *headerless = [NSMutableSet set];
  [_controls removeAllObjects];
  [_sliders removeAllObjects];
  [_lockIndicators removeAllObjects];
  [_groupHeadings removeAllObjects];
  if (!_status) {
    _status = [self label:@"" style:UIFontTextStyleFootnote];
    _status.textColor = UIColor.systemRedColor;
    _status.hidden = YES;
    _status.frame = CGRectMake(0, 0, _form.bounds.size.width, 44);
    _status.textAlignment = NSTextAlignmentCenter;
    _form.tableFooterView = _status;
  }
  if (!inspector) {
    _form.backgroundView = [self label:@"No active object" style:UIFontTextStyleBody];
    ((UILabel *)_form.backgroundView).textAlignment = NSTextAlignmentCenter;
    [_sectionTitles release];
    _sectionTitles = nil;
    [_headerContainers removeAllObjects];
    [_form reloadData];
    return;
  }
  _form.backgroundView = nil;
  __block GHOSTIOSScenePanels *owner = self;
  NSString *objectID = inspector[@"id"];
  for (NSDictionary *section in inspector[@"sections"]) {
    [titles addObject:section[@"title"]];
    NSString *sectionID = section[@"id"] ?: section[@"title"];
    [identifiers addObject:sectionID];
    if ([section[@"active"] boolValue]) { [actives addObject:sectionID]; }
    if ([section[@"header_hidden"] boolValue]) {
      /* Blender draws these without a header, so they stay open and are not collapsible. */
      [headerless addObject:sectionID];
      [_expandedPanels[_selectedTab ?: @"OBJECT"] addObject:sectionID];
    }
    NSArray *shape = [self shapeForSection:section];
    NSDictionary *cached = _sectionViewCache[sectionID];
    NSArray *maps = @[_controls, _sliders, _lockIndicators, _groupHeadings];
    NSArray *mapKeys = @[@"controls", @"sliders", @"locks", @"headings"];
    if ([cached[@"shape"] isEqual:shape]) {
      [_formSections addObject:cached[@"rows"]];
      [_formRowKeys addObject:cached[@"keys"]];
      [_headerControls addObject:cached[@"header"]];
      for (NSUInteger i = 0; i < maps.count; i++) {
        [maps[i] addEntriesFromDictionary:cached[mapKeys[i]]];
      }
      nextCache[sectionID] = cached;
      continue;
    }
    for (UIView *row in cached[@"rows"]) { [row endEditing:YES]; }
    [_disclosureButtons removeObjectForKey:sectionID];
    if (_colorButton && [cached[@"controls"] allValues] &&
        [[cached[@"controls"] allValues] containsObject:_colorButton]) {
      [self closeColorPicker];
    }
    NSMutableArray *sectionRows = [NSMutableArray array];
    [_formSections addObject:sectionRows];
    NSMutableArray *sectionKeys = [NSMutableArray array];
    [_formRowKeys addObject:sectionKeys];
    UIStackView *headerControls = [[[UIStackView alloc] init] autorelease];
    headerControls.spacing = 8;
    [_headerControls addObject:headerControls];
    NSMutableDictionary *vectors = [NSMutableDictionary dictionary];
    NSString *innerSection = nil;
    UIStackView *innerHeading = nil;
    for (NSDictionary *property in section[@"properties"]) {
      NSString *subpanel = property[@"subpanel"] ?: property[@"section"];
      if (property[@"section"] && ![innerSection isEqual:subpanel]) {
        innerSection = subpanel;
        UIView *heading;
        if ([property[@"type"] isEqual:@"SUBPANEL"]) {
          const BOOL open = [property[@"open"] boolValue];
          UIButton *disclosure = blenderDisclosureButton(property[@"section"],
              [UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline], UIColor.secondaryLabelColor,
              12 * MAX(0, [property[@"depth"] integerValue] - 1), open);
          disclosure.accessibilityIdentifier = [@"blender.subpanel." stringByAppendingString:subpanel];
          [disclosure addAction:[UIAction actionWithHandler:^(__kindof UIAction *action) {
            [owner->_form endEditing:YES];
            [owner cancelNumberScrub];
            NSString *tab = owner->_selectedTab ?: @"OBJECT";
            NSMutableDictionary *states = owner->_subpanelStates[tab];
            if (!states) {
              states = [NSMutableDictionary dictionary];
              owner->_subpanelStates[tab] = states;
            }
            const BOOL opening = ![(states[subpanel] ?: property[@"open"]) boolValue];
            states[subpanel] = @(opening);
            blenderSetDisclosureExpanded((UIButton *)action.sender, opening);
            [owner requestProperties];
          }] forControlEvents:UIControlEventTouchUpInside];
          heading = disclosure;
        }
        else {
          UILabel *label = [self label:property[@"section"] style:UIFontTextStyleSubheadline];
          label.textColor = UIColor.secondaryLabelColor;
          heading = label;
        }
        [heading.heightAnchor constraintGreaterThanOrEqualToConstant:36].active = YES;
        innerHeading = [[[UIStackView alloc] initWithArrangedSubviews:@[heading]] autorelease];
        innerHeading.alignment = UIStackViewAlignmentCenter;
        innerHeading.spacing = 8;
        [heading setContentCompressionResistancePriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];
        [sectionRows addObject:innerHeading];
        [sectionKeys addObject:[@"heading:" stringByAppendingString:subpanel ?: @""]];
      }
      if ([property[@"type"] isEqual:@"SUBPANEL"]) {
        continue;
      }
      NSString *group = property[@"group"];
      const BOOL vector = group.length > 0;
      UIStackView *vectorRow = nil;
      if (vector && !_toolInline) {
        NSString *identity = property[@"group_key"] ?: [NSString stringWithFormat:@"%@.%@", property[@"owner"], property[@"property"]];
        NSString *groupKey = [NSString stringWithFormat:@"%@.%@.%@", property[@"panel"] ?: @"", innerSection ?: @"", identity];
        vectorRow = vectors[groupKey];
        if (!vectorRow) {
          vectorRow = [[[UIStackView alloc] init] autorelease];
          vectorRow.spacing = 4;
          vectorRow.distribution = UIStackViewDistributionFillEqually;
          vectors[groupKey] = vectorRow;
          UILabel *title = [self label:group style:UIFontTextStyleCaption1];
          title.textColor = UIColor.secondaryLabelColor;
          if (property[@"group_menu"]) {
            NSString *key = [self keyForProperty:property];
            title.userInteractionEnabled = YES;
            title.accessibilityIdentifier = [@"blender.group." stringByAppendingString:key];
            title.accessibilityTraits |= UIAccessibilityTraitButton;
            title.accessibilityHint = @"Touch and hold to change the mode.";
            [title.heightAnchor constraintGreaterThanOrEqualToConstant:28].active = YES;
            [title addInteraction:[[[UIContextMenuInteraction alloc] initWithDelegate:self] autorelease]];
            _groupHeadings[key] = title;
          }
          UIStackView *vectorGroup = [[[UIStackView alloc] initWithArrangedSubviews:@[title, vectorRow]] autorelease];
          vectorGroup.axis = UILayoutConstraintAxisVertical;
          vectorGroup.spacing = 0;
          [sectionRows addObject:vectorGroup];
          [sectionKeys addObject:[@"vector:" stringByAppendingString:groupKey]];
        }
      }
      UILabel *label = [self label:property[@"label"] style:UIFontTextStyleBody];
      UIControl *control;
      NSString *kind = property[@"type"];
      if ([kind isEqual:@"OPERATOR"] && [property[@"checkbox"] boolValue]) {
        /* Blender draws some toggle operators as checkboxes; run the operator from a switch. */
        UISwitch *toggle = [[[UISwitch alloc] init] autorelease];
        toggle.on = [property[@"value"] boolValue];
        [toggle addAction:[UIAction actionWithHandler:^(__kindof UIAction *action) {
          NSMutableDictionary *request = [[property mutableCopy] autorelease];
          request[@"id"] = objectID;
          [owner send:request];
        }] forControlEvents:UIControlEventValueChanged];
        control = toggle;
      }
      else if ([kind isEqual:@"OPERATOR"] || [kind isEqual:@"MENU"]) {
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
            NSMutableDictionary *request = [[property mutableCopy] autorelease];
            request[@"id"] = objectID;
            [owner send:request];
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
        const BOOL toolRow = [_selectedTab isEqual:@"TOOL"] && [property[@"expand"] boolValue] &&
                             [property[@"options"] count] > 1 && [property[@"options"] count] <= 6;
        if (_shadingOnly && [property[@"property"] isEqual:@"type"]) {
          NSMutableArray *items = [NSMutableArray array];
          for (NSDictionary *option in property[@"options"]) {
            [items addObject:_icon(option[@"icon"]) ?: option[@"label"]];
          }
          UISegmentedControl *segments = [[[UISegmentedControl alloc] initWithItems:items] autorelease];
          [segments addAction:[UIAction actionWithHandler:^(__kindof UIAction *action) {
            NSInteger index = ((UISegmentedControl *)action.sender).selectedSegmentIndex;
            if (index >= 0 && index < [property[@"options"] count]) {
              [owner editProperty:property object:objectID value:property[@"options"][index][@"value"]];
            }
          }] forControlEvents:UIControlEventValueChanged];
          control = segments;
          label.hidden = YES;
        }
        else if (blenderOptionsHavePreviews(property[@"options"])) {
          /* Blender shows these as a large preview (template_icon_view), not a list of names. */
          GHOSTIOSPreviewButton *button = [GHOSTIOSPreviewButton buttonWithType:UIButtonTypeSystem];
          button.descriptor = property;
          button.accessibilityHint = @"Shows all choices as previews.";
          [button addAction:[UIAction actionWithHandler:^(__kindof UIAction *action) {
            [owner presentPreviewGrid:(GHOSTIOSPreviewButton *)action.sender object:objectID];
          }] forControlEvents:UIControlEventTouchUpInside];
          control = button;
          label.hidden = YES;
        }
        else if (toolRow) {
          /* Blender's icon-only rows (e.g. selection mode) become a glass button showing the
           * current choice's icon, with a standard single-selection menu. */
          UIButtonConfiguration *config = [UIButtonConfiguration glassButtonConfiguration];
          config.indicator = UIButtonConfigurationIndicatorPopup;
          config.contentInsets = NSDirectionalEdgeInsetsMake(2, 8, 2, 8);
          UIButton *button = [UIButton buttonWithConfiguration:config primaryAction:nil];
          button.showsMenuAsPrimaryAction = YES;
          button.accessibilityLabel = property[@"toolbar_label"] ?: property[@"label"];
          control = button;
        }
        else {
          UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
          button.showsMenuAsPrimaryAction = YES;
          control = button;
        }
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
          field.font = [[UIFontMetrics metricsForTextStyle:UIFontTextStyleBody]
              scaledFontForFont:[UIFont monospacedDigitSystemFontOfSize:15 weight:UIFontWeightRegular]];
          GHOSTIOSNumberFieldAppearance(field, false);
          UIToolbar *accessory = [[[UIToolbar alloc] initWithFrame:CGRectMake(0, 0, 320, 44)] autorelease];
          UIStepper *stepper = [[[UIStepper alloc] init] autorelease];
          stepper.accessibilityIdentifier = [self keyForProperty:property];
          stepper.accessibilityLabel = [NSString stringWithFormat:@"Adjust %@ %@", section[@"title"], blenderNativePropertyLabel(property)];
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
          if (!property[@"slider"] && ([kind isEqual:@"FLOAT"] || [kind isEqual:@"INT"])) {
            UIPanGestureRecognizer *pan = [[[UIPanGestureRecognizer alloc]
                initWithTarget:self action:@selector(scrubNumber:)] autorelease];
            pan.name = @"blender.number.scrub";
            GHOSTIOSNumberFieldInstallHover(field);
            pan.maximumNumberOfTouches = 1;
            pan.allowedTouchTypes = @[@(UITouchTypeDirect), @(UITouchTypeStylus)];
            pan.delegate = self;
            [field addGestureRecognizer:pan];
            [_form.panGestureRecognizer requireGestureRecognizerToFail:pan];
          }
        }
        [field addTarget:self action:@selector(commitField:) forControlEvents:UIControlEventEditingDidEnd];
        control = field;
      }
      control.enabled = [property[@"editable"] boolValue];
      control.accessibilityLabel = [NSString stringWithFormat:@"%@ %@", section[@"title"], blenderNativePropertyLabel(property)];
      NSString *key = [self keyForProperty:property];
      control.accessibilityIdentifier = [@"blender.property." stringByAppendingString:key];
      _controls[key] = control;
      GHOSTIOSPropertySlider *slider = nil;
      if (property[@"slider"] && [control isKindOfClass:GHOSTIOSPropertyField.class]) {
        slider = [[[GHOSTIOSPropertySlider alloc] init] autorelease];
        slider.descriptor = property;
        slider.objectID = objectID;
        slider.minimumValue = 0;
        slider.maximumValue = 1;
        slider.continuous = YES;
        slider.enabled = control.enabled;
        slider.accessibilityLabel = control.accessibilityLabel;
        slider.accessibilityIdentifier = [@"blender.slider." stringByAppendingString:key];
        [slider addTarget:self action:@selector(beginSlider:) forControlEvents:UIControlEventTouchDown];
        [slider addTarget:self action:@selector(changeSlider:) forControlEvents:UIControlEventValueChanged];
        [slider addTarget:self action:@selector(finishSlider:) forControlEvents:UIControlEventTouchUpInside | UIControlEventTouchUpOutside];
        [slider addTarget:self action:@selector(cancelSlider:) forControlEvents:UIControlEventTouchCancel];
        [slider.heightAnchor constraintGreaterThanOrEqualToConstant:32].active = YES;
        _sliders[key] = slider;
      }
      if (_toolInline) {
        UIControl *inlineControl = slider ?: control;
        const BOOL toggle = [inlineControl isKindOfClass:UISwitch.class];
        label.text = property[@"toolbar_label"] ?: label.text;
        label.font = [UIFont preferredFontForTextStyle:UIFontTextStyleCaption2];
        label.textColor = UIColor.secondaryLabelColor;
        label.numberOfLines = 1;
        label.textAlignment = NSTextAlignmentCenter;
        label.adjustsFontSizeToFitWidth = YES;
        label.minimumScaleFactor = 0.8;
        UIView *columnControl = inlineControl;
        if (toggle) {
          /* Caption above a compact switch, like the other inline columns. */
          inlineControl.transform = CGAffineTransformMakeScale(0.7, 0.7);
          UIView *holder = [[[UIView alloc] init] autorelease];
          inlineControl.translatesAutoresizingMaskIntoConstraints = NO;
          [holder addSubview:inlineControl];
          [NSLayoutConstraint activateConstraints:@[
            [holder.widthAnchor constraintEqualToConstant:40],
            [holder.heightAnchor constraintEqualToConstant:24],
            [inlineControl.centerXAnchor constraintEqualToAnchor:holder.centerXAnchor],
            [inlineControl.centerYAnchor constraintEqualToAnchor:holder.centerYAnchor],
          ]];
          columnControl = holder;
        }
        UIStackView *column = [[[UIStackView alloc] initWithArrangedSubviews:@[label, columnControl]] autorelease];
        column.axis = UILayoutConstraintAxisVertical;
        column.alignment = UIStackViewAlignmentCenter;
        column.spacing = 0;
        const BOOL iconMenu = [inlineControl isKindOfClass:UIButton.class] &&
                              ((UIButton *)inlineControl).configuration.indicator == UIButtonConfigurationIndicatorPopup;
        CGFloat width = [kind isEqual:@"COLOR"] ? 52 : toggle ? 72 : iconMenu ? 72 :
                        [kind isEqual:@"ENUM"] ? 108 : [kind isEqual:@"OPERATOR"] ? 96 : 104;
        [column.widthAnchor constraintEqualToConstant:width].active = YES;
        [column.heightAnchor constraintEqualToConstant:40].active = YES;
        [label.widthAnchor constraintLessThanOrEqualToConstant:width - 4].active = YES;
        if (!toggle) {
          [inlineControl.widthAnchor constraintEqualToConstant:[kind isEqual:@"COLOR"] ? 36 : width - 8].active = YES;
          [inlineControl.heightAnchor constraintEqualToConstant:26].active = YES;
        }
        [sectionRows addObject:column];
        [sectionKeys addObject:[@"property:" stringByAppendingString:[self keyForProperty:property]]];
        continue;
      }
      if ([property[@"header"] boolValue] && [kind isEqual:@"OPERATOR"] && ![property[@"checkbox"] boolValue]) {
        /* Header operators such as Remove are compact icon buttons beside the header switches. */
        UIButtonConfiguration *config = [UIButtonConfiguration plainButtonConfiguration];
        config.image = _icon(property[@"icon"]);
        config.title = config.image ? nil : property[@"label"];
        config.baseForegroundColor = UIColor.secondaryLabelColor;
        config.contentInsets = NSDirectionalEdgeInsetsMake(4, 4, 4, 4);
        ((UIButton *)control).configuration = config;
        if (config.image) {
          [control.widthAnchor constraintEqualToConstant:36].active = YES;
        }
        [control.heightAnchor constraintEqualToConstant:36].active = YES;
        [control setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
        [control setContentCompressionResistancePriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
        [(property[@"section"] ? innerHeading : headerControls) addArrangedSubview:control];
        continue;
      }
      if ([property[@"header"] boolValue] && [control isKindOfClass:UISwitch.class]) {
        if (property[@"animation"]) {
          [control addInteraction:[[[UIContextMenuInteraction alloc] initWithDelegate:self] autorelease]];
          control.accessibilityHint = @"Touch and hold for property actions.";
        }
        [control setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
        [control setContentCompressionResistancePriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
        [(property[@"section"] ? innerHeading : headerControls) addArrangedSubview:control];
        continue;
      }
      UIStackView *row = [[[UIStackView alloc] initWithArrangedSubviews:@[label, control]] autorelease];
      row.spacing = vector ? 4 : 10;
      row.axis = UILayoutConstraintAxisHorizontal;
      row.alignment = UIStackViewAlignmentCenter;
      if (property[@"lock"]) {
        UIImage *image = _icon(@"LOCKED") ?: [UIImage systemImageNamed:@"lock.fill"];
        UIImageView *indicator = [[[UIImageView alloc] initWithImage:image] autorelease];
        indicator.contentMode = UIViewContentModeScaleAspectFit;
        indicator.tintColor = UIColor.secondaryLabelColor;
        indicator.isAccessibilityElement = NO;
        [indicator.widthAnchor constraintEqualToConstant:12].active = YES;
        [indicator.heightAnchor constraintEqualToConstant:12].active = YES;
        [row insertArrangedSubview:indicator atIndex:1];
        _lockIndicators[key] = indicator;
      }
      if (property[@"lock"] || property[@"animation"]) {
        row.accessibilityIdentifier = key;
        [row addInteraction:[[[UIContextMenuInteraction alloc] initWithDelegate:self] autorelease]];
        control.accessibilityHint = @"Touch and hold for property actions.";
      }
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
        else if ([control isKindOfClass:GHOSTIOSPreviewButton.class]) {
          [control.widthAnchor constraintEqualToAnchor:row.widthAnchor].active = YES;
          [control.heightAnchor constraintGreaterThanOrEqualToConstant:64].active = YES;
        }
        else if (!vector && ![kind isEqual:@"OPERATOR"] && ![kind isEqual:@"MENU"] &&
                 ![control isKindOfClass:UISegmentedControl.class]) {
          [control.widthAnchor constraintEqualToAnchor:row.widthAnchor multiplier:0.53].active = YES;
        }
        [control.heightAnchor constraintGreaterThanOrEqualToConstant:slider ? 32 : 40].active = YES;
      }
      [row.heightAnchor constraintGreaterThanOrEqualToConstant:slider ? 32 : 44].active = YES;
      UIStackView *content = row;
      if (slider) {
        content = [[[UIStackView alloc] initWithArrangedSubviews:@[row, slider]] autorelease];
        content.axis = UILayoutConstraintAxisVertical;
      }
      if (vectorRow) {
        [vectorRow addArrangedSubview:content];
      }
      else {
        [sectionRows addObject:content];
        [sectionKeys addObject:[@"property:" stringByAppendingString:[self keyForProperty:property]]];
      }
    }
    NSCountedSet *seen = [NSCountedSet set];
    for (NSUInteger i = 0; i < sectionKeys.count; i++) {
      [seen addObject:sectionKeys[i]];
      if ([seen countForObject:sectionKeys[i]] > 1) {
        sectionKeys[i] = [NSString stringWithFormat:@"%@#%lu", sectionKeys[i],
                          (unsigned long)[seen countForObject:sectionKeys[i]]];
      }
    }
    NSMutableDictionary *record = [NSMutableDictionary dictionaryWithDictionary:
        @{@"shape": shape, @"rows": sectionRows, @"keys": sectionKeys, @"header": headerControls}];
    for (NSUInteger i = 0; i < maps.count; i++) {
      NSMutableDictionary *entries = [NSMutableDictionary dictionary];
      for (NSDictionary *property in section[@"properties"]) {
        NSString *key = [self keyForProperty:property];
        if (maps[i][key]) {
          entries[key] = maps[i][key];
        }
      }
      record[mapKeys[i]] = entries;
    }
    nextCache[sectionID] = record;
  }
  [_sectionViewCache setDictionary:nextCache];
  if (_scrubField && _controls[[self keyForProperty:_scrubField.descriptor]] != _scrubField) {
    [self cancelNumberScrub];
  }
  [_sectionTitles release];
  _sectionTitles = [titles copy];
  [_activeSections release];
  _activeSections = [actives copy];
  [_headerlessSections release];
  _headerlessSections = [headerless copy];
  [_sectionIdentifiers release];
  _sectionIdentifiers = [identifiers copy];
  for (NSString *identifier in [[_disclosureButtons.allKeys copy] autorelease]) {
    if (![_sectionIdentifiers containsObject:identifier]) { [_disclosureButtons removeObjectForKey:identifier]; }
  }
  if (_toolInline) {
    /* All controls share one strip that scrolls sideways when the bar is too narrow, so
     * every setting stays reachable inline. */
    UIStackView *strip = [[[UIStackView alloc] init] autorelease];
    strip.axis = UILayoutConstraintAxisHorizontal;
    strip.alignment = UIStackViewAlignmentCenter;
    strip.spacing = 6;
    strip.translatesAutoresizingMaskIntoConstraints = NO;
    for (NSArray *rows in _formSections) {
      for (UIView *row in rows) {
        [strip addArrangedSubview:row];
      }
    }
    NSMutableArray *items = [NSMutableArray array];
    if (strip.arrangedSubviews.count) {
      UIScrollView *scroll = [[[UIScrollView alloc] init] autorelease];
      scroll.accessibilityIdentifier = @"blender.tool-settings.strip";
      scroll.showsHorizontalScrollIndicator = NO;
      scroll.delaysContentTouches = NO;
      scroll.clipsToBounds = YES;
      [scroll addSubview:strip];
      const CGFloat content = [strip systemLayoutSizeFittingSize:UILayoutFittingCompressedSize].width;
      [_toolStripWidth release];
      _toolStripWidth = [[scroll.widthAnchor constraintEqualToConstant:content] retain];
      _toolStripContentWidth = content;
      [NSLayoutConstraint activateConstraints:@[
        [strip.leadingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.leadingAnchor],
        [strip.trailingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.trailingAnchor],
        [strip.topAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.topAnchor],
        [strip.bottomAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.bottomAnchor],
        [strip.heightAnchor constraintEqualToAnchor:scroll.frameLayoutGuide.heightAnchor],
        [scroll.heightAnchor constraintEqualToConstant:40],
        _toolStripWidth,
      ]];
      [self updateToolStripWidth];
      [items addObject:[[[UIBarButtonItem alloc] initWithCustomView:scroll] autorelease]];
    }
    UIBarButtonItem *compact = [[[UIBarButtonItem alloc] initWithImage:
        _icon(@"TOOL_SETTINGS") ?: [UIImage systemImageNamed:@"slider.horizontal.3"]
        style:UIBarButtonItemStylePlain target:self action:@selector(presentToolSettings:)] autorelease];
    compact.accessibilityLabel = @"Tool Settings";
    /* Keep one group installed in the bar: replacing it on every rebuild makes the bar
     * re-measure from scratch and fall back to the compact item. */
    if (!_toolGroup) {
      _toolGroup = [[UIBarButtonItemGroup fixedGroupWithRepresentativeItem:compact items:items] retain];
    }
    else {
      _toolGroup.barButtonItems = items;
    }
    _toolGroup.hidden = items.count == 0;
  }
  else if (preserve) {
    [self updateFormFromSections:oldIdentifiers rows:oldRows keys:oldRowKeys];
  }
  else {
    [_headerContainers removeAllObjects];
    [_form reloadData];
  }
}

/* Animate from the previous form to the rebuilt one by row identity: only rows that
 * appear or disappear animate, persisting rows take their new controls in place. A
 * positional reload would cross-fade unrelated rows, e.g. below an opened sub-panel. */
- (void)updateFormFromSections:(NSArray *)oldIDs rows:(NSArray *)oldRows keys:(NSArray *)oldKeys
{
  NSSet *expanded = _expandedPanels[_selectedTab ?: @"OBJECT"];
  NSArray *(^visible)(NSString *, NSArray *) = ^NSArray *(NSString *sectionID, NSArray *keys) {
    return expanded && ![expanded containsObject:sectionID] ? @[] : (keys ?: @[]);
  };
  NSMutableArray *commonOld = [NSMutableArray array], *commonNew = [NSMutableArray array];
  for (NSString *identifier in oldIDs) {
    if ([_sectionIdentifiers containsObject:identifier]) { [commonOld addObject:identifier]; }
  }
  for (NSString *identifier in _sectionIdentifiers) {
    if ([oldIDs containsObject:identifier]) { [commonNew addObject:identifier]; }
  }
  if (oldIDs.count != oldRows.count || oldIDs.count != oldKeys.count || ![commonOld isEqual:commonNew] ||
      [_form hasUncommittedUpdates]) {
    [_headerContainers removeAllObjects];
    [_form reloadData];
    return;
  }
  NSMutableIndexSet *deletedSections = [NSMutableIndexSet indexSet];
  NSMutableIndexSet *insertedSections = [NSMutableIndexSet indexSet];
  NSMutableArray *deletedRows = [NSMutableArray array], *insertedRows = [NSMutableArray array];
  NSMutableArray *moves = [NSMutableArray array];
  for (NSUInteger section = 0; section < oldIDs.count; section++) {
    if (![_sectionIdentifiers containsObject:oldIDs[section]]) { [deletedSections addIndex:section]; }
  }
  for (NSUInteger section = 0; section < _sectionIdentifiers.count; section++) {
    NSString *identifier = _sectionIdentifiers[section];
    const NSUInteger oldSection = [oldIDs indexOfObject:identifier];
    if (oldSection == NSNotFound) {
      [insertedSections addIndex:section];
      [_headerContainers removeObjectForKey:identifier];
      continue;
    }
    NSArray *before = visible(identifier, oldKeys[oldSection]);
    NSArray *after = visible(identifier, _formRowKeys[section]);
    for (NSUInteger row = 0; row < before.count; row++) {
      const NSUInteger next = [after indexOfObject:before[row]];
      NSIndexPath *from = [NSIndexPath indexPathForRow:row inSection:oldSection];
      if (next == NSNotFound) {
        [deletedRows addObject:from];
        continue;
      }
      UIView *previous = oldRows[oldSection][row];
      UIView *current = _formSections[section][next];
      if (previous != current) {
        UITableViewCell *cell = [_form cellForRowAtIndexPath:from];
        if (cell) { [self installRow:current inCell:cell section:section]; }
      }
      if (row != next) {
        [moves addObject:@[from, [NSIndexPath indexPathForRow:next inSection:section]]];
      }
    }
    for (NSUInteger row = 0; row < after.count; row++) {
      if (![before containsObject:after[row]]) {
        [insertedRows addObject:[NSIndexPath indexPathForRow:row inSection:section]];
      }
    }
    UIView *container = _headerContainers[identifier];
    if (container) { [self fillHeader:container section:section]; }
  }
  for (NSString *identifier in [[_headerContainers.allKeys copy] autorelease]) {
    if (![_sectionIdentifiers containsObject:identifier]) { [_headerContainers removeObjectForKey:identifier]; }
  }
  if (!deletedSections.count && !insertedSections.count && !deletedRows.count && !insertedRows.count &&
      !moves.count) {
    return;
  }
  const UITableViewRowAnimation animation = UIAccessibilityIsReduceMotionEnabled() ?
      UITableViewRowAnimationNone : UITableViewRowAnimationFade;
  [_form performBatchUpdates:^{
    [self->_form deleteSections:deletedSections withRowAnimation:animation];
    [self->_form insertSections:insertedSections withRowAnimation:animation];
    [self->_form deleteRowsAtIndexPaths:deletedRows withRowAnimation:animation];
    [self->_form insertRowsAtIndexPaths:insertedRows withRowAnimation:animation];
    for (NSArray *move in moves) {
      [self->_form moveRowAtIndexPath:move[0] toIndexPath:move[1]];
    }
  } completion:nil];
}

- (void)presentPreviewGrid:(GHOSTIOSPreviewButton *)button object:(NSString *)objectID
{
  NSDictionary *property = button.descriptor;
  GHOSTIOSPreviewGrid *grid = [[[GHOSTIOSPreviewGrid alloc] init] autorelease];
  grid.title = property[@"label"];
  grid.options = property[@"options"];
  grid.value = button.value;
  __block GHOSTIOSScenePanels *owner = self;
  grid.choose = ^(NSDictionary *option) {
    [button showValue:option[@"value"] options:property[@"options"]];
    [owner editProperty:property object:objectID value:option[@"value"]];
  };
  /* Inside the shading popover, push like a settings page; elsewhere use a popover. */
  UIResponder *responder = button;
  while (responder && ![responder isKindOfClass:UIViewController.class]) { responder = responder.nextResponder; }
  UINavigationController *navigation = ((UIViewController *)responder).navigationController;
  if (navigation) {
    [navigation pushViewController:grid animated:YES];
    return;
  }
  UIViewController *presenter = _overlay.window.rootViewController;
  while (presenter.presentedViewController) { presenter = presenter.presentedViewController; }
  UINavigationController *wrapper = [[[UINavigationController alloc] initWithRootViewController:grid] autorelease];
  wrapper.modalPresentationStyle = UIModalPresentationPopover;
  wrapper.preferredContentSize = CGSizeMake(420, 480);
  wrapper.popoverPresentationController.sourceView = button;
  wrapper.popoverPresentationController.sourceRect = button.bounds;
  [presenter presentViewController:wrapper animated:YES completion:nil];
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
  [_form endEditing:YES];
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
    if (_colorInteraction) {
      [self previewProperty:_colorButton.descriptor object:_colorButton.objectID value:components
               interaction:_colorInteraction phase:@"end"];
      [_colorInteraction release]; _colorInteraction = nil;
    }
    else { [self editProperty:_colorButton.descriptor object:_colorButton.objectID value:components]; }
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
  else {
    if (!_colorInteraction) {
      _colorInteraction = [NSUUID.UUID.UUIDString copy];
      _colorPreviewTime = 0;
    }
    if (CACurrentMediaTime() - _colorPreviewTime >= 1.0 / 60.0) {
      NSArray *components = blenderNativeColorComponents(color, _colorButton.well.supportsAlpha);
      if (components) {
        [self previewProperty:_colorButton.descriptor object:_colorButton.objectID value:components
                 interaction:_colorInteraction phase:@"update"];
        _colorPreviewTime = CACurrentMediaTime();
      }
    }
  }
}

- (void)colorPickerViewControllerDidFinish:(UIColorPickerViewController *)picker
{
  [self closeColorPicker];
}

- (void)presentationControllerWillDismiss:(UIPresentationController *)presentationController
{
  /* Resign the keyboard while the field is still in the popover's window. Otherwise UIKit
   * re-attaches its input accessory after the popover is gone and throws on the constraint. */
  [presentationController.presentedViewController.view endEditing:YES];
}

- (void)presentationControllerDidDismiss:(UIPresentationController *)presentationController
{
  if (presentationController.presentedViewController == _toolPopoverController) {
    [_toolPopoverPanels cancelNumberScrub];
    [_toolPopoverPanels closeColorPicker];
    return;
  }
  if (presentationController.presentedViewController == _shadingController) {
    [self dismissShading];
    return;
  }
  [self closeColorPicker];
}

- (void)popoverPresentationControllerDidDismissPopover:(UIPopoverPresentationController *)popover
{
  if (popover.presentedViewController == _shadingController) {
    [self dismissShading];
    return;
  }
  [self closeColorPicker];
}

- (void)requestProperties
{
  if ([_selectedTab isEqual:@"TOOL"]) { return; }
  NSMutableDictionary *state = [NSMutableDictionary dictionaryWithObject:_selectedTab ?: @"OBJECT" forKey:@"tab"];
  NSSet *expanded = _expandedPanels[_selectedTab ?: @"OBJECT"];
  if (expanded) {
    state[@"expanded"] = expanded.allObjects;
  }
  NSDictionary *subpanels = _subpanelStates[_selectedTab ?: @"OBJECT"];
  if (subpanels.count) {
    state[@"subpanels"] = subpanels;
  }
  NSData *data = [NSJSONSerialization dataWithJSONObject:state options:0 error:nil];
  NSString *json = [[[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] autorelease];
  WM_apple_ios_ui_setting_enqueue(_shadingOnly ? "native_shading_state" : "native_properties_state", json.UTF8String);
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
      /* Blender colors object, modifier and data icons in the Properties tabs. */
      configuration.image = GHOSTIOSBlenderIcon(tab[@"icon"], YES) ?: _icon(tab[@"icon"]);
      configuration.preferredSymbolConfigurationForImage = [UIImageSymbolConfiguration configurationWithPointSize:22];
      configuration.contentInsets = NSDirectionalEdgeInsetsMake(10, 10, 10, 10);
      button.configuration = configuration;
      button.accessibilityIdentifier = tab[@"id"];
      button.accessibilityLabel = tab[@"title"];
      [button addInteraction:[[[UIToolTipInteraction alloc] initWithDefaultToolTip:tab[@"title"]] autorelease]];
      [button addAction:[UIAction actionWithHandler:^(__kindof UIAction *action) {
        [owner->_form endEditing:YES];
        [owner cancelNumberScrub];
        [owner->_selectedTab release];
        owner->_selectedTab = [tab[@"id"] copy];
        [owner requestProperties];
        for (UIButton *item in owner->_tabsStack.arrangedSubviews) {
          item.selected = [item.accessibilityIdentifier isEqual:owner->_selectedTab];
        }
      }] forControlEvents:UIControlEventTouchUpInside];
      button.configurationUpdateHandler = ^(UIButton *item) {
        UIButtonConfiguration *config = item.configuration;
        config.baseForegroundColor = item.selected ? UIColor.whiteColor : UIColor.secondaryLabelColor;
        config.background.backgroundColor = item.selected ? UIColor.systemBlueColor : UIColor.clearColor;
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

- (void)finishNumber:(id)sender { [_overlay endEditing:YES]; }

- (NSDictionary *)propertyForKey:(NSString *)key
{
  for (NSDictionary *section in _snapshot[@"inspector"][@"sections"]) {
    for (NSDictionary *property in section[@"properties"]) {
      if ([[self keyForProperty:property] isEqual:key]) {
        return property;
      }
    }
  }
  return nil;
}

- (UIAction *)lockActionForProperty:(NSDictionary *)property
{
  NSDictionary *lock = property[@"lock"];
  if (!lock) {
    return nil;
  }
  BOOL locked = [lock[@"value"] boolValue];
  NSString *key = [self keyForProperty:property];
  NSString *objectID = _objectID;
  __block GHOSTIOSScenePanels *owner = self;
  UIAction *action = [UIAction actionWithTitle:locked ? @"Unlock" : @"Lock"
      image:_icon(locked ? @"UNLOCKED" : @"LOCKED") ?: [UIImage systemImageNamed:locked ? @"lock.open" : @"lock"]
      identifier:@"blender.property.lock" handler:^(__kindof UIAction *selected) {
    NSDictionary *current = [owner propertyForKey:key];
    if (![owner->_objectID isEqual:objectID] ||
        ![current[@"lock"][@"key"] isEqual:lock[@"key"]] ||
        ![current[@"lock"][@"editable"] boolValue]) {
      return;
    }
    [owner->_form endEditing:YES];
    [owner editProperty:current[@"lock"] object:objectID value:@(!locked)];
  }];
  if (![lock[@"editable"] boolValue]) {
    action.attributes = UIMenuElementAttributesDisabled;
  }
  return action;
}

- (NSArray<UIMenuElement *> *)menuActionsForProperty:(NSDictionary *)property
{
  NSMutableArray *items = [NSMutableArray array];
  UIAction *lock = [self lockActionForProperty:property];
  if (lock) {
    [items addObject:lock];
  }
  NSString *key = [self keyForProperty:property];
  NSString *objectID = _objectID;
  __block GHOSTIOSScenePanels *owner = self;
  for (NSDictionary *entry in property[@"animation"]) {
    NSString *identifier = entry[@"id"];
    UIAction *action = [UIAction actionWithTitle:entry[@"title"] image:_icon(entry[@"icon"])
        identifier:[@"blender.property.animation." stringByAppendingString:identifier]
        handler:^(__kindof UIAction *selected) {
      NSDictionary *current = [owner propertyForKey:key];
      if (![owner->_objectID isEqual:objectID]) {
        return;
      }
      for (NSDictionary *available in current[@"animation"]) {
        if ([available[@"id"] isEqual:identifier]) {
          [owner->_form endEditing:YES];
          NSMutableDictionary *request = [[current mutableCopy] autorelease];
          request[@"action"] = @"animation";
          request[@"animation_action"] = identifier;
          request[@"id"] = objectID;
          [owner send:request];
          break;
        }
      }
    }];
    [items addObject:action];
  }
  return items;
}

- (NSArray<UIMenuElement *> *)groupMenuActionsForProperty:(NSDictionary *)property
{
  NSDictionary *mode = property[@"group_menu"];
  NSMutableArray *actions = [NSMutableArray array];
  NSString *key = [self keyForProperty:property];
  NSString *objectID = _objectID;
  __block GHOSTIOSScenePanels *owner = self;
  for (NSDictionary *option in mode[@"options"]) {
    UIAction *action = [UIAction actionWithTitle:option[@"label"] image:_icon(option[@"icon"])
        identifier:option[@"value"] handler:^(__kindof UIAction *selected) {
      NSDictionary *current = [owner propertyForKey:key][@"group_menu"];
      if (![owner->_objectID isEqual:objectID] || ![current[@"key"] isEqual:mode[@"key"]] ||
          ![current[@"editable"] boolValue]) {
        return;
      }
      for (NSDictionary *available in current[@"options"]) {
        if ([available[@"value"] isEqual:option[@"value"]]) {
          if (![current[@"value"] isEqual:option[@"value"]]) {
            [owner->_form endEditing:YES];
            [owner editProperty:current object:objectID value:option[@"value"]];
          }
          break;
        }
      }
    }];
    action.state = [mode[@"value"] isEqual:option[@"value"]] ? UIMenuElementStateOn : UIMenuElementStateOff;
    if (![mode[@"editable"] boolValue]) {
      action.attributes = UIMenuElementAttributesDisabled;
    }
    [actions addObject:action];
  }
  return actions;
}

- (UIContextMenuConfiguration *)contextMenuInteraction:(UIContextMenuInteraction *)interaction
                       configurationForMenuAtLocation:(CGPoint)location
{
  NSString *key = interaction.view.accessibilityIdentifier;
  BOOL group = [key hasPrefix:@"blender.group."];
  if (group) {
    key = [key substringFromIndex:@"blender.group.".length];
  }
  else if ([key hasPrefix:@"blender.property."]) {
    key = [key substringFromIndex:@"blender.property.".length];
  }
  NSDictionary *property = [self propertyForKey:key];
  if (group ? !property[@"group_menu"] : (!property[@"lock"] && ![property[@"animation"] count])) {
    return nil;
  }
  NSString *objectID = _objectID;
  __block GHOSTIOSScenePanels *owner = self;
  return [UIContextMenuConfiguration configurationWithIdentifier:key previewProvider:nil
      actionProvider:^UIMenu *(NSArray<UIMenuElement *> *suggested) {
    if (![owner->_objectID isEqual:objectID]) {
      return nil;
    }
    NSDictionary *current = [owner propertyForKey:key];
    NSArray *actions = group ? [owner groupMenuActionsForProperty:current] : [owner menuActionsForProperty:current];
    NSString *title = group ? current[@"group_menu"][@"label"] :
        current[@"group"] ? [NSString stringWithFormat:@"%@ %@", current[@"group"], current[@"label"]] : current[@"label"];
    return actions.count ? [UIMenu menuWithTitle:title children:actions] : nil;
  }];
}

- (UIMenu *)textField:(UITextField *)textField
    editMenuForCharactersInRanges:(NSArray<NSValue *> *)ranges
                suggestedActions:(NSArray<UIMenuElement *> *)suggestedActions
{
  NSDictionary *property = [self propertyForKey:[self keyForProperty:((GHOSTIOSPropertyField *)textField).descriptor]];
  NSArray *actions = [self menuActionsForProperty:property];
  if (!actions.count) {
    return [UIMenu menuWithChildren:suggestedActions];
  }
  NSMutableArray *items = [NSMutableArray arrayWithArray:actions];
  [items addObjectsFromArray:suggestedActions];
  return [UIMenu menuWithChildren:items];
}

- (void)displaySliderValue:(GHOSTIOSPropertySlider *)slider value:(double)value
{
  GHOSTIOSPropertyField *field = _controls[[self keyForProperty:slider.descriptor]];
  NSNumberFormatter *formatter = [[[NSNumberFormatter alloc] init] autorelease];
  formatter.numberStyle = NSNumberFormatterDecimalStyle;
  formatter.usesGroupingSeparator = NO;
  formatter.usesSignificantDigits = YES;
  formatter.maximumSignificantDigits = [slider.descriptor[@"type"] isEqual:@"INT"] ? 10 : 6;
  field.text = [formatter stringFromNumber:@(value * ([slider.descriptor[@"angle"] boolValue] ? 180.0 / M_PI : 1))];
  field.originalText = field.text;
  slider.accessibilityValue = field.text;
}

- (void)beginSlider:(GHOSTIOSPropertySlider *)slider
{
  [_form endEditing:YES];
  slider.editing = YES;
  slider.changed = NO;
  slider.interactionID = NSUUID.UUID.UUIDString;
  slider.startValue = slider.propertyValue;
  slider.lastPreview = 0;
}

- (void)changeSlider:(GHOSTIOSPropertySlider *)slider
{
  if (_sliders[[self keyForProperty:slider.descriptor]] != slider || !slider.enabled) {
    return;
  }
  double value = slider.propertyValue;
  slider.changed = YES;
  [slider setPropertyValue:value];
  [self displaySliderValue:slider value:value];
  if (slider.editing && CACurrentMediaTime() - slider.lastPreview >= 1.0 / 60.0) {
    [self previewProperty:slider.descriptor object:slider.objectID value:@(value)
             interaction:slider.interactionID phase:@"update"];
    slider.lastPreview = CACurrentMediaTime();
  }
  if (!slider.editing) {
    /* Accessibility adjustments are complete edits without touch-down/up events. */
    [self finishSlider:slider];
  }
}

- (void)finishSlider:(GHOSTIOSPropertySlider *)slider
{
  slider.editing = NO;
  if (_sliders[[self keyForProperty:slider.descriptor]] != slider || !slider.enabled || !slider.changed) {
    slider.interactionID = nil;
    return;
  }
  slider.changed = NO;
  double value = slider.propertyValue;
  if (slider.interactionID) {
    [self previewProperty:slider.descriptor object:slider.objectID value:@(value)
             interaction:slider.interactionID phase:value == slider.startValue ? @"cancel" : @"end"];
    slider.interactionID = nil;
  }
  else { [self editProperty:slider.descriptor object:slider.objectID value:@(value)]; }
  NSMutableDictionary *updated = [[slider.descriptor mutableCopy] autorelease];
  updated[@"value"] = @(value);
  slider.descriptor = updated;
  ((GHOSTIOSPropertyField *)_controls[[self keyForProperty:updated]]).descriptor = updated;
}

- (void)cancelSlider:(GHOSTIOSPropertySlider *)slider
{
  if (slider.interactionID) {
    [self previewProperty:slider.descriptor object:slider.objectID value:@(slider.startValue)
             interaction:slider.interactionID phase:@"cancel"];
    slider.interactionID = nil;
    [slider setPropertyValue:slider.startValue];
    [self displaySliderValue:slider value:slider.startValue];
    slider.editing = NO;
    slider.changed = NO;
    return;
  }
  slider.editing = NO;
  slider.changed = NO;
  if (_sliders[[self keyForProperty:slider.descriptor]] != slider) {
    return;
  }
  NSDictionary *current = slider.descriptor;
  for (NSDictionary *section in _snapshot[@"inspector"][@"sections"]) {
    for (NSDictionary *property in section[@"properties"]) {
      if ([[self keyForProperty:property] isEqual:[self keyForProperty:slider.descriptor]]) {
        current = property;
      }
    }
  }
  slider.descriptor = current;
  ((GHOSTIOSPropertyField *)_controls[[self keyForProperty:current]]).descriptor = current;
  [slider setPropertyValue:[current[@"value"] doubleValue]];
  [self displaySliderValue:slider value:[current[@"value"] doubleValue]];
}

- (void)stepNumber:(UIStepper *)stepper
{
  UITextField *field = _controls[stepper.accessibilityIdentifier];
  NSNumberFormatter *formatter = [[[NSNumberFormatter alloc] init] autorelease];
  formatter.numberStyle = NSNumberFormatterDecimalStyle;
  formatter.usesGroupingSeparator = NO;
  formatter.maximumFractionDigits = 6;
  field.text = [formatter stringFromNumber:@(stepper.value)];
  [self commitField:(GHOSTIOSPropertyField *)field];
}

- (BOOL)gestureRecognizerShouldBegin:(UIGestureRecognizer *)recognizer
{
  if (![recognizer.name isEqual:@"blender.number.scrub"]) { return YES; }
  GHOSTIOSPropertyField *field = (GHOSTIOSPropertyField *)recognizer.view;
  CGPoint velocity = [(UIPanGestureRecognizer *)recognizer velocityInView:field];
  return field.enabled && !field.isFirstResponder && !field.descriptor[@"slider"] &&
      _controls[[self keyForProperty:field.descriptor]] == field &&
      [field.objectID isEqual:_objectID] && fabs(velocity.x) > fabs(velocity.y) * 1.2;
}

- (void)displayNumberScrub:(GHOSTIOSPropertyField *)field
{
  NSNumberFormatter *formatter = [[[NSNumberFormatter alloc] init] autorelease];
  formatter.numberStyle = NSNumberFormatterDecimalStyle;
  formatter.usesGroupingSeparator = NO;
  formatter.usesSignificantDigits = YES;
  formatter.maximumSignificantDigits = [field.descriptor[@"type"] isEqual:@"INT"] ? 10 : 9;
  double value = field.scrubValue;
  if ([field.descriptor[@"type"] isEqual:@"INT"]) { value = round(value); }
  field.text = [formatter stringFromNumber:@(value * ([field.descriptor[@"angle"] boolValue] ? 180.0 / M_PI : 1))];
  field.originalText = field.text;
}

- (void)beginNumberScrub:(GHOSTIOSPropertyField *)field
{
  if (!field.enabled || field.isFirstResponder || field.descriptor[@"slider"] ||
      !([field.descriptor[@"type"] isEqual:@"FLOAT"] || [field.descriptor[@"type"] isEqual:@"INT"]) ||
      _controls[[self keyForProperty:field.descriptor]] != field || ![field.objectID isEqual:_objectID]) {
    return;
  }
  [self cancelNumberScrub];
  [_form endEditing:YES];
  _scrubField = [field retain];
  field.scrubbing = YES;
  field.scrubStart = field.scrubValue = [field.descriptor[@"value"] doubleValue];
  field.scrubTranslation = 0;
  field.interactionID = NSUUID.UUID.UUIDString;
  field.lastPreview = 0;
  GHOSTIOSNumberFieldAppearance(field, true);
}

- (void)changeNumberScrub:(GHOSTIOSPropertyField *)field translation:(CGFloat)translation
{
  [self changeNumberScrub:field translation:translation vertical:0];
}

- (void)changeNumberScrub:(GHOSTIOSPropertyField *)field translation:(CGFloat)translation
                 vertical:(CGFloat)vertical
{
  if (field != _scrubField || !field.scrubbing || !std::isfinite(translation) ||
      !std::isfinite(vertical)) { return; }
  if (!field.enabled || _controls[[self keyForProperty:field.descriptor]] != field ||
      ![field.objectID isEqual:_objectID]) {
    [self cancelNumberScrub];
    return;
  }
  double step = field.descriptor[@"dragStep"] ? [field.descriptor[@"dragStep"] doubleValue] :
      [field.descriptor[@"type"] isEqual:@"INT"] ? 0.5 : 0.01;
  if ([field.descriptor[@"angle"] boolValue]) { step *= GHOSTIOSAngleScrubScale * M_PI / 180.0; }
  /* Integrate horizontal movement so changing precision never reinterprets earlier motion. */
  double value = field.scrubValue +
      (translation - field.scrubTranslation) * step * GHOSTIOSNumberSensitivity(vertical);
  double minimum = field.descriptor[@"min"] ? [field.descriptor[@"min"] doubleValue] : -DBL_MAX;
  double maximum = field.descriptor[@"max"] ? [field.descriptor[@"max"] doubleValue] : DBL_MAX;
  field.scrubValue = fmin(maximum, fmax(minimum, value));
  field.scrubTranslation = translation;
  [self displayNumberScrub:field];
  GHOSTIOSNumberScrubIndicatorUpdate(field, vertical);
  if (CACurrentMediaTime() - field.lastPreview >= 1.0 / 60.0) {
    const double preview = [field.descriptor[@"type"] isEqual:@"INT"] ? round(field.scrubValue) : field.scrubValue;
    [self previewProperty:field.descriptor object:field.objectID value:@(preview)
             interaction:field.interactionID phase:@"update"];
    field.lastPreview = CACurrentMediaTime();
  }
}

- (void)finishNumberScrub:(GHOSTIOSPropertyField *)field
{
  if (field != _scrubField) { return; }
  BOOL valid = field.enabled && _controls[[self keyForProperty:field.descriptor]] == field &&
      [field.objectID isEqual:_objectID];
  double value = [field.descriptor[@"type"] isEqual:@"INT"] ? round(field.scrubValue) : field.scrubValue;
  field.scrubbing = NO;
  GHOSTIOSNumberFieldAppearance(field, false);
  if (valid) {
    [self previewProperty:field.descriptor object:field.objectID value:@(value)
             interaction:field.interactionID phase:value != field.scrubStart ? @"end" : @"cancel"];
    NSMutableDictionary *updated = [[field.descriptor mutableCopy] autorelease];
    updated[@"value"] = @(value);
    field.descriptor = updated;
  }
  field.interactionID = nil;
  [_scrubField release];
  _scrubField = nil;
}

- (void)cancelNumberScrub
{
  if (!_scrubField) { return; }
  if (_scrubField.interactionID) {
    [self previewProperty:_scrubField.descriptor object:_scrubField.objectID value:@(_scrubField.scrubStart)
             interaction:_scrubField.interactionID phase:@"cancel"];
    _scrubField.interactionID = nil;
  }
  _scrubField.scrubValue = _scrubField.scrubStart;
  [self displayNumberScrub:_scrubField];
  _scrubField.scrubbing = NO;
  GHOSTIOSNumberFieldAppearance(_scrubField, false);
  [_scrubField release];
  _scrubField = nil;
}

- (void)scrubNumber:(UIPanGestureRecognizer *)pan
{
  GHOSTIOSPropertyField *field = (GHOSTIOSPropertyField *)pan.view;
  if (pan.state == UIGestureRecognizerStateBegan) {
    [self beginNumberScrub:field];
    [pan setTranslation:CGPointZero inView:_overlay];
  }
  else if (pan.state == UIGestureRecognizerStateChanged || pan.state == UIGestureRecognizerStateEnded) {
    CGPoint translation = [pan translationInView:_overlay];
    [self changeNumberScrub:field translation:translation.x vertical:translation.y];
    if (pan.state == UIGestureRecognizerStateEnded) { [self finishNumberScrub:field]; }
  }
  else if (pan.state == UIGestureRecognizerStateCancelled || pan.state == UIGestureRecognizerStateFailed) {
    if (field == _scrubField) { [self cancelNumberScrub]; }
  }
}

- (void)commitField:(GHOSTIOSPropertyField *)field
{
  if (field.scrubbing) { return; }
  if (![field.descriptor[@"type"] isEqual:@"STRING"]) { GHOSTIOSNumberFieldAppearance(field, false); }
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
    [_sliders[[self keyForProperty:field.descriptor]] setPropertyValue:[value doubleValue]];
    NSMutableDictionary *updated = [[field.descriptor mutableCopy] autorelease];
    updated[@"value"] = value;
    field.descriptor = updated;
    field.originalText = field.text;
  }
}

- (BOOL)textFieldShouldReturn:(UITextField *)field
{
  [field resignFirstResponder];
  return YES;
}

- (void)textFieldDidBeginEditing:(UITextField *)field
{
  if (![[(GHOSTIOSPropertyField *)field descriptor][@"type"] isEqual:@"STRING"]) {
    GHOSTIOSNumberFieldAppearance(field, false);
  }
  if ([field.inputAccessoryView isKindOfClass:UIToolbar.class]) {
    UIToolbar *bar = (UIToolbar *)field.inputAccessoryView;
    UIStepper *stepper = (UIStepper *)bar.items.firstObject.customView;
    NSNumberFormatter *formatter = [[[NSNumberFormatter alloc] init] autorelease];
    formatter.numberStyle = NSNumberFormatterDecimalStyle;
    stepper.value = [[formatter numberFromString:field.text] doubleValue];
  }
  dispatch_async(dispatch_get_main_queue(), ^{
    if (self->_toolInline) { return; }
    [self->_inspectorScroll scrollRectToVisible:[field convertRect:field.bounds toView:self->_inspectorScroll]
                                     animated:YES];
  });
}

- (NSDictionary *)snapshotKeepingCollapsedContents:(NSDictionary *)snapshot
{
  NSDictionary *inspector = snapshot[@"inspector"];
  NSDictionary *previous = _snapshot[@"inspector"];
  if (!inspector || ![inspector[@"id"] isEqual:previous[@"id"]]) {
    return snapshot;
  }
  NSMutableArray *sections = [NSMutableArray array];
  for (NSDictionary *section in inspector[@"sections"]) {
    NSDictionary *old = nil;
    if (section[@"open"] && ![section[@"open"] boolValue]) {
      for (NSDictionary *candidate in previous[@"sections"]) {
        if ([candidate[@"id"] isEqual:section[@"id"]]) { old = candidate; break; }
      }
    }
    if (old) {
      NSMutableArray *properties = [NSMutableArray array];
      for (NSDictionary *property in old[@"properties"]) {
        if (![property[@"header"] boolValue] || property[@"section"]) {
          [properties addObject:property];
        }
      }
      [properties insertObjects:section[@"properties"] atIndexes:
          [NSIndexSet indexSetWithIndexesInRange:NSMakeRange(0, [section[@"properties"] count])]];
      NSMutableDictionary *merged = [[section mutableCopy] autorelease];
      merged[@"properties"] = properties;
      [sections addObject:merged];
    }
    else {
      [sections addObject:section];
    }
  }
  NSMutableDictionary *mergedInspector = [[inspector mutableCopy] autorelease];
  mergedInspector[@"sections"] = sections;
  NSMutableDictionary *merged = [[snapshot mutableCopy] autorelease];
  merged[@"inspector"] = mergedInspector;
  return merged;
}

- (void)updateSnapshot:(NSDictionary *)snapshot
{
  if (_shadingPanels && [snapshot[@"shading"] isKindOfClass:NSDictionary.class]) {
    [_shadingPanels updateSnapshot:@{@"ack": snapshot[@"ack"] ?: @0,
        @"error": snapshot[@"error"] ?: @"", @"inspector": snapshot[@"shading"]}];
  }
  if (![snapshot isKindOfClass:NSDictionary.class] || [snapshot[@"ack"] unsignedLongLongValue] < _sequence) {
    return;
  }
  snapshot = [self snapshotKeepingCollapsedContents:snapshot];
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
  else if ([nodes isKindOfClass:NSArray.class]) {
    [self synchronizeHierarchySelection];
  }
  NSString *error = snapshot[@"error"] ?: @"";
  _hierarchyStatus.text = error;
  _hierarchyStatus.hidden = !error.length;
  [self layoutInRect:_layoutRect];
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
    [schema addObject:[self shapeForSection:section]];
  }
  if (![_objectID isEqual:inspector[@"id"]] || ![_schema isEqual:schema]) {
    BOOL preserve = [_objectID isEqual:inspector[@"id"]];
    /* Commit to the original ID before replacing the controls on a selection change. */
    if (!preserve) { [_form endEditing:YES]; }
    [_objectID release];
    _objectID = [inspector[@"id"] copy];
    [_schema release];
    _schema = [schema copy];
    [self rebuildInspector:inspector preservingSections:preserve];
  }
  _inspectorTitle.text = inspector[@"name"] ?: @"Properties";
  _status.text = snapshot[@"error"] ?: @"";
  _status.hidden = _status.text.length == 0;
  __block GHOSTIOSScenePanels *owner = self;
  for (NSDictionary *section in inspector[@"sections"]) {
    for (NSDictionary *property in section[@"properties"]) {
      UIControl *control = _controls[[self keyForProperty:property]];
      GHOSTIOSPropertySlider *slider = _sliders[[self keyForProperty:property]];
      id value = property[@"value"];
      control.enabled = [property[@"editable"] boolValue];
      slider.enabled = control.enabled;
      UILabel *heading = _groupHeadings[[self keyForProperty:property]];
      if (heading) {
        for (NSDictionary *option in property[@"group_menu"][@"options"]) {
          if ([option[@"value"] isEqual:property[@"group_menu"][@"value"]]) {
            heading.accessibilityValue = option[@"label"];
            break;
          }
        }
      }
      if (property[@"lock"]) {
        BOOL locked = [property[@"lock"][@"value"] boolValue];
        ((UIImageView *)_lockIndicators[[self keyForProperty:property]]).alpha = locked ? 1 : 0;
        control.accessibilityLabel = [NSString stringWithFormat:@"%@ %@%@", section[@"title"], blenderNativePropertyLabel(property), locked ? @", Locked" : @""];
      }
      if (slider && !slider.editing && !control.isFirstResponder) {
        slider.descriptor = property;
        [slider setPropertyValue:[value doubleValue]];
      }
      if ([control isKindOfClass:GHOSTIOSColorButton.class]) {
        GHOSTIOSColorButton *button = (GHOSTIOSColorButton *)control;
        if (button != _colorButton) {
          button.descriptor = property;
          button.well.selectedColor = blenderNativeColor(value);
        }
      }
      else if ([control isKindOfClass:GHOSTIOSPropertyField.class]) {
        GHOSTIOSPropertyField *field = (GHOSTIOSPropertyField *)control;
        if (![property[@"type"] isEqual:@"STRING"]) {
          GHOSTIOSNumberFieldAppearance(field, field.scrubbing);
        }
        if (field.scrubbing && !field.enabled) {
          [self cancelNumberScrub];
        }
        if (!field.isFirstResponder && !slider.editing && !field.scrubbing) {
          field.descriptor = property;
          if ([value isKindOfClass:NSNumber.class]) {
            double number = [value doubleValue] * ([property[@"angle"] boolValue] ? 180.0 / M_PI : 1.0);
            NSNumberFormatter *formatter = [[[NSNumberFormatter alloc] init] autorelease];
            formatter.numberStyle = NSNumberFormatterDecimalStyle;
            formatter.usesGroupingSeparator = NO;
            formatter.usesSignificantDigits = YES;
            formatter.maximumSignificantDigits = slider ? ([property[@"type"] isEqual:@"INT"] ? 10 : 6) : 9;
            field.text = [formatter stringFromNumber:@(number)];
          }
          else {
            field.text = value;
          }
          field.originalText = field.text;
          slider.accessibilityValue = field.text;
        }
      }
      else if ([control isKindOfClass:UISwitch.class]) {
        ((UISwitch *)control).on = [value boolValue];
      }
      else if ([property[@"type"] isEqual:@"ENUM"]) {
        if ([control isKindOfClass:UISegmentedControl.class]) {
          UISegmentedControl *segments = (UISegmentedControl *)control;
          NSInteger selected = UISegmentedControlNoSegment;
          for (NSUInteger i = 0; i < [property[@"options"] count]; i++) {
            if ([property[@"options"][i][@"value"] isEqual:value]) {
              selected = i;
              segments.accessibilityValue = property[@"options"][i][@"label"];
            }
          }
          /* Reassigning the same segment, or clearing it first, restarts the selection
           * and cuts off UIKit's sliding transition when Blender acknowledges the edit. */
          if (segments.selectedSegmentIndex != selected) { segments.selectedSegmentIndex = selected; }
          continue;
        }
        if ([control isKindOfClass:GHOSTIOSPreviewButton.class]) {
          GHOSTIOSPreviewButton *preview = (GHOSTIOSPreviewButton *)control;
          preview.descriptor = property;
          if (![preview.value isEqual:value] || !preview.configuration.title) {
            [preview showValue:value options:property[@"options"]];
          }
          continue;
        }
        UIButton *button = (UIButton *)control;
        NSArray *options = property[@"options"];
        BOOL unchanged = [control.accessibilityValue isEqual:value] && button.menu.children.count == options.count;
        for (NSUInteger i = 0; unchanged && i < options.count; i++) {
          NSDictionary *option = options[i];
          BOOL checked = option[@"checked"] ? [option[@"checked"] boolValue] : [value isEqual:option[@"value"]];
          unchanged = (((UIAction *)button.menu.children[i]).state == UIMenuElementStateOn) == checked;
        }
        if (unchanged) { continue; }
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
            if (button.configuration.indicator == UIButtonConfigurationIndicatorPopup) {
              UIButtonConfiguration *config = button.configuration;
              config.image = _icon(option[@"icon"]);
              config.title = config.image ? nil : option[@"label"];
              button.configuration = config;
            }
            else {
              [button setTitle:option[@"label"] forState:UIControlStateNormal];
            }
          }
          [actions addObject:action];
        }
        button.menu = [UIMenu menuWithTitle:property[@"toolbar_label"] ?: property[@"label"] image:nil identifier:nil
                                    options:UIMenuOptionsSingleSelection children:actions];
        button.titleLabel.lineBreakMode = NSLineBreakByTruncatingTail;
      }
    }
  }
}

- (void)rebuildRows
{
  NSString *query = _search.text ?: @"";
  NSDictionary *previous = [[_nodesByKey retain] autorelease];
  NSMutableDictionary *nodes = [NSMutableDictionary dictionary];
  for (NSDictionary *node in _nodes) { nodes[node[@"key"]] = node; }
  [_nodesByKey release];
  _nodesByKey = [nodes copy];
  [_collapsed intersectSet:[NSSet setWithArray:nodes.allKeys]];
  NSMutableSet *matches = [NSMutableSet set];
  if (query.length) {
    for (NSDictionary *node in _nodes) {
      if ([node[@"name"] localizedStandardContainsString:query]) {
        NSString *key = node[@"key"];
        while (key.length) {
          [matches addObject:key];
          key = nodes[key][@"parent"];
        }
      }
    }
  }
  NSDiffableDataSourceSectionSnapshot<NSString *> *outline =
      [[[NSDiffableDataSourceSectionSnapshot alloc] init] autorelease];
  NSMutableSet *included = [NSMutableSet set];
  NSMutableArray *expanded = [NSMutableArray array];
  for (NSDictionary *node in _nodes) {
    NSString *key = node[@"key"];
    if (query.length && ![matches containsObject:key]) { continue; }
    NSString *parent = [included containsObject:node[@"parent"]] ? node[@"parent"] : nil;
    [outline appendItems:@[key] intoParentItem:parent];
    [included addObject:key];
    if (query.length || ![_collapsed containsObject:key]) {
      [expanded addObject:key];
    }
  }
  [outline expandItems:expanded];
  NSDiffableDataSourceSectionSnapshot *current = [_treeDataSource snapshotForSection:@"scene"];
  BOOL topologyChanged = ![current.items isEqual:outline.items] ||
                         ![current.expandedItems isEqual:outline.expandedItems];
  for (NSString *key in outline.items) {
    if (topologyChanged) { break; }
    NSString *oldParent = [current parentOfChildItem:key] ?: @"";
    NSString *newParent = [outline parentOfChildItem:key] ?: @"";
    topologyChanged = ![oldParent isEqual:newParent];
  }
  if (topologyChanged || !_treeDataSource.snapshot.numberOfSections) {
    [_treeDataSource applySnapshot:outline toSection:@"scene"
        animatingDifferences:previous != nil && !UIAccessibilityIsReduceMotionEnabled()
        completion:^{ [self synchronizeHierarchySelection]; }];
  }
  /* Value changes must not rebuild the outline or interrupt disclosure animations. */
  for (NSIndexPath *path in _tree.indexPathsForVisibleItems) {
    NSString *key = [_treeDataSource itemIdentifierForIndexPath:path];
    if (nodes[key] && (topologyChanged || ![previous[key] isEqual:nodes[key]])) {
      [self configureHierarchyCell:(UICollectionViewListCell *)[_tree cellForItemAtIndexPath:path] key:key];
    }
  }
  [self synchronizeHierarchySelection];
}

- (NSArray *)hierarchyRows
{
  NSMutableArray *rows = [NSMutableArray array];
  for (NSString *key in [_treeDataSource snapshotForSection:@"scene"].visibleItems) {
    if (_nodesByKey[key]) { [rows addObject:_nodesByKey[key]]; }
  }
  return rows;
}

- (void)synchronizeHierarchySelection
{
  for (NSIndexPath *path in _tree.indexPathsForSelectedItems) {
    NSDictionary *node = _nodesByKey[[_treeDataSource itemIdentifierForIndexPath:path]];
    if (![node[@"selected"] boolValue]) { [_tree deselectItemAtIndexPath:path animated:NO]; }
  }
  for (NSDictionary *node in [self hierarchyRows]) {
    if ([node[@"selected"] boolValue]) {
      NSIndexPath *path = [_treeDataSource indexPathForItemIdentifier:node[@"key"]];
      if (path) { [_tree selectItemAtIndexPath:path animated:NO scrollPosition:UICollectionViewScrollPositionNone]; }
    }
  }
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
  return 0;
}

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView
{
  return tableView == _form ? _formSections.count : 0;
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
  if ([_headerlessSections containsObject:identifier]) {
    return nil;
  }
  UIView *container = _headerContainers[identifier];
  if (!container) {
    container = [[[UIView alloc] init] autorelease];
    _headerContainers[identifier] = container;
  }
  [self fillHeader:container section:section];
  return container;
}

- (void)fillHeader:(UIView *)container section:(NSInteger)section
{
  for (UIView *view in [[container.subviews copy] autorelease]) { [view removeFromSuperview]; }
  UIView *content = [self headerContentForSection:section];
  content.translatesAutoresizingMaskIntoConstraints = NO;
  [container addSubview:content];
  [NSLayoutConstraint activateConstraints:@[
    [content.leadingAnchor constraintEqualToAnchor:container.leadingAnchor],
    [content.trailingAnchor constraintEqualToAnchor:container.trailingAnchor],
    [content.topAnchor constraintEqualToAnchor:container.topAnchor],
    [content.bottomAnchor constraintEqualToAnchor:container.bottomAnchor],
  ]];
}

- (UIView *)headerContentForSection:(NSInteger)section
{
  NSString *identifier = _sectionIdentifiers[section];
  BOOL expanded = !_selectedTab || [_expandedPanels[_selectedTab] containsObject:identifier];
  const BOOL active = [_activeSections containsObject:identifier];
  UIButton *header = blenderDisclosureButton(_sectionTitles[section], nil,
      active ? _form.tintColor : UIColor.labelColor, 12, expanded);
  if (active) {
    header.accessibilityValue = [header.accessibilityValue stringByAppendingString:@", Active"];
  }
  _disclosureButtons[identifier] = header;
  __block GHOSTIOSScenePanels *owner = self;
  [header addAction:[UIAction actionWithHandler:^(__kindof UIAction *action) {
    [owner->_form endEditing:YES];
    [owner cancelNumberScrub];
    NSMutableSet *open = owner->_expandedPanels[owner->_selectedTab ?: @"OBJECT"];
    NSInteger currentSection = [owner->_sectionIdentifiers indexOfObject:identifier];
    if (currentSection == NSNotFound || !open) { return; }
    BOOL closing = [open containsObject:identifier];
    NSInteger count = [owner->_formSections[currentSection] count];
    NSMutableArray *paths = [NSMutableArray array];
    for (NSInteger row = 0; row < count; row++) {
      [paths addObject:[NSIndexPath indexPathForRow:row inSection:currentSection]];
    }
    blenderSetDisclosureExpanded(owner->_disclosureButtons[identifier], !closing);
    [owner->_form performBatchUpdates:^{
      if (closing) { [open removeObject:identifier]; }
      else { [open addObject:identifier]; }
      if (paths.count) {
        UITableViewRowAnimation animation = UIAccessibilityIsReduceMotionEnabled() ? UITableViewRowAnimationNone : UITableViewRowAnimationFade;
        if (closing) { [owner->_form deleteRowsAtIndexPaths:paths withRowAnimation:animation]; }
        else { [owner->_form insertRowsAtIndexPaths:paths withRowAnimation:animation]; }
      }
    } completion:nil];
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
  if (tableView != _form) {
    return 0;
  }
  return [_headerlessSections containsObject:_sectionIdentifiers[section]] ? 8 : 44;
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath
{
  if (tableView == _form) {
    UITableViewCell *cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil] autorelease];
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    [self installRow:_formSections[indexPath.section][indexPath.row] inCell:cell section:indexPath.section];
    return cell;
  }
  return nil;
}

- (void)installRow:(UIView *)row inCell:(UITableViewCell *)cell section:(NSInteger)section
{
  for (UIView *view in [[cell.contentView.subviews copy] autorelease]) { [view removeFromSuperview]; }
  /* The shading mode picker is already a pill; a grouped card around it nests the shape. */
  const BOOL bare = _shadingOnly && [_formSections[section] count] == 1 &&
                    [row isKindOfClass:UIStackView.class] &&
                    [[(UIStackView *)row arrangedSubviews] indexOfObjectPassingTest:
                        ^BOOL(UIView *view, NSUInteger, BOOL *) {
                          return [view isKindOfClass:UISegmentedControl.class];
                        }] != NSNotFound;
  cell.backgroundColor = bare ? UIColor.clearColor : UIColor.secondarySystemGroupedBackgroundColor;
  const CGFloat inset = bare ? 0 : 12;
  [row removeFromSuperview];
  row.translatesAutoresizingMaskIntoConstraints = NO;
  [cell.contentView addSubview:row];
  [NSLayoutConstraint activateConstraints:@[
    [row.leadingAnchor constraintEqualToAnchor:cell.contentView.leadingAnchor constant:inset],
    [row.trailingAnchor constraintEqualToAnchor:cell.contentView.trailingAnchor constant:-inset],
    [row.topAnchor constraintEqualToAnchor:cell.contentView.topAnchor constant:2],
    [row.bottomAnchor constraintEqualToAnchor:cell.contentView.bottomAnchor constant:-2],
  ]];
}

- (void)toggleHierarchyItem:(NSString *)key
{
  NSDiffableDataSourceSectionSnapshot *outline = [[[_treeDataSource snapshotForSection:@"scene"] copy] autorelease];
  const BOOL expand = ![outline isExpanded:key];
  if (expand) { [outline expandItems:@[key]]; }
  else { [outline collapseItems:@[key]]; }
  if (!_search.text.length) {
    if (expand) { [_collapsed removeObject:key]; }
    else { [_collapsed addObject:key]; }
  }
  [_treeDataSource applySnapshot:outline toSection:@"scene"
      animatingDifferences:!UIAccessibilityIsReduceMotionEnabled()
      completion:^{ [self synchronizeHierarchySelection]; }];
  NSIndexPath *path = [_treeDataSource indexPathForItemIdentifier:key];
  UICollectionViewListCell *cell = path ? (UICollectionViewListCell *)[_tree cellForItemAtIndexPath:path] : nil;
  for (UICellAccessory *accessory in cell.accessories) {
    if ([accessory isKindOfClass:UICellAccessoryCustomView.class] &&
        ((UICellAccessoryCustomView *)accessory).placement == UICellAccessoryPlacementLeading) {
      UIView *slotView = ((UICellAccessoryCustomView *)accessory).customView;
      slotView.subviews.firstObject.accessibilityLabel = expand ? @"Collapse" : @"Expand";
      blenderRotateDisclosure(slotView, expand);
    }
  }
}

- (void)configureHierarchyCell:(UICollectionViewListCell *)cell key:(NSString *)key
{
  NSDictionary *node = _nodesByKey[key];
  BOOL collection = [node[@"kind"] isEqual:@"collection"];
  UIListContentConfiguration *content = [cell defaultContentConfiguration];
  content.text = node[@"name"];
  content.textProperties.numberOfLines = 2;
  if ([node[@"active"] boolValue]) {
    content.textProperties.font = [UIFont fontWithDescriptor:
        [content.textProperties.font.fontDescriptor fontDescriptorWithSymbolicTraits:UIFontDescriptorTraitBold]
        size:0];
  }
  if (![node[@"enabled"] boolValue]) { content.textProperties.color = UIColor.secondaryLabelColor; }
  /* As in Blender's Outliner, the hierarchy shows category icons in their theme colors. */
  content.image = GHOSTIOSBlenderIcon(node[@"icon"], YES) ?: _icon(node[@"icon"]) ?:
                  [UIImage systemImageNamed:collection ? @"folder" : @"cube"];
  content.imageProperties.maximumSize = CGSizeMake(22, 22);
  cell.contentConfiguration = content;
  cell.configurationUpdateHandler = ^(UICollectionViewCell *updated, UICellConfigurationState *state) {
    UIListContentConfiguration *configuration = [content updatedConfigurationForState:state];
    UIBackgroundConfiguration *background = [[UIBackgroundConfiguration listSidebarCellConfiguration]
        updatedConfigurationForState:state];
    if (state.selected) {
      background.backgroundColor = UIColor.systemBlueColor;
      configuration.textProperties.color = UIColor.whiteColor;
      configuration.imageProperties.tintColor = UIColor.whiteColor;
    }
    updated.contentConfiguration = configuration;
    updated.backgroundConfiguration = background;
  };
  [cell setNeedsUpdateConfiguration];
  cell.accessibilityIdentifier = [@"blender.hierarchy." stringByAppendingString:node[@"id"]];
  cell.accessibilityValue = [node[@"active"] boolValue] ? @"Active" : @"";
  __block GHOSTIOSScenePanels *owner = self;
  NSMutableArray *accessories = [NSMutableArray array];
  /* UIKit's outline disclosure is always trailing; an outliner reads its disclosure on the
   * left, like Blender and Finder. Leaves keep an empty slot so names stay aligned. */
  NSDiffableDataSourceSectionSnapshot *outline = [_treeDataSource snapshotForSection:@"scene"];
  /* The sidebar list ignores indentationLevel, so the leading slot carries the depth
   * indent itself, with the chevron at its trailing edge just before the row's icon. */
  cell.indentationLevel = 0;
  const CGFloat indent = 16 * MAX(0, [node[@"depth"] integerValue]);
  UIView *slotView = [[[UIView alloc] initWithFrame:CGRectMake(0, 0, indent + 20, 28)] autorelease];
  UIButton *disclosure = [UIButton buttonWithType:UIButtonTypeSystem];
  disclosure.translatesAutoresizingMaskIntoConstraints = NO;
  [slotView addSubview:disclosure];
  [NSLayoutConstraint activateConstraints:@[
    [slotView.widthAnchor constraintEqualToConstant:indent + 20],
    [slotView.heightAnchor constraintEqualToConstant:28],
    [disclosure.widthAnchor constraintEqualToConstant:20],
    [disclosure.heightAnchor constraintEqualToConstant:28],
    [disclosure.trailingAnchor constraintEqualToAnchor:slotView.trailingAnchor],
    [disclosure.centerYAnchor constraintEqualToAnchor:slotView.centerYAnchor],
  ]];
  if ([outline snapshotOfParentItem:key includingParentItem:NO].items.count) {
    const BOOL expanded = [outline isExpanded:key];
    UIView *glyph = blenderDisclosureGlyph(UIColor.secondaryLabelColor, expanded);
    [disclosure addSubview:glyph];
    [NSLayoutConstraint activateConstraints:@[
      [glyph.centerXAnchor constraintEqualToAnchor:disclosure.centerXAnchor],
      [glyph.centerYAnchor constraintEqualToAnchor:disclosure.centerYAnchor],
    ]];
    disclosure.accessibilityLabel = expanded ? @"Collapse" : @"Expand";
    disclosure.accessibilityIdentifier = [@"blender.hierarchy.disclosure." stringByAppendingString:node[@"id"]];
    [disclosure addAction:[UIAction actionWithHandler:^(__kindof UIAction *action) {
      [owner toggleHierarchyItem:key];
    }] forControlEvents:UIControlEventTouchUpInside];
  }
  else {
    disclosure.userInteractionEnabled = NO;
    disclosure.isAccessibilityElement = NO;
  }
  /* A fixed size keeps every row at one depth identical, so chevrons and names line up. */
  UICellAccessoryCustomView *slot = [[[UICellAccessoryCustomView alloc] initWithCustomView:slotView
      placement:UICellAccessoryPlacementLeading] autorelease];
  slot.maintainsFixedSize = YES;
  slot.reservedLayoutWidth = indent + 20;
  [accessories addObject:slot];
  if (!collection && [node[@"hidden"] boolValue]) {
    UIButton *accessory = [UIButton buttonWithType:UIButtonTypeSystem];
    accessory.tintColor = UIColor.secondaryLabelColor;
    accessory.frame = CGRectMake(0, 0, 44, 44);
    [accessory setImage:[UIImage systemImageNamed:@"eye.slash"] forState:UIControlStateNormal];
    accessory.accessibilityLabel = @"Show in viewport";
    [accessory addAction:[UIAction actionWithHandler:^(__kindof UIAction *action) {
      [owner send:@{@"action": @"visibility", @"id": node[@"id"], @"value": @NO}];
    }] forControlEvents:UIControlEventTouchUpInside];
    [accessories addObject:[[[UICellAccessoryCustomView alloc] initWithCustomView:accessory
        placement:UICellAccessoryPlacementTrailing] autorelease]];
  }
  cell.accessories = accessories;
}

- (void)promptNewCollection
{
  UIViewController *presenter = _overlay.window.rootViewController;
  while (presenter.presentedViewController) { presenter = presenter.presentedViewController; }
  if (!presenter || presenter.isBeingDismissed || [presenter isKindOfClass:UIAlertController.class]) {
    return;
  }
  UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"New Collection"
      message:nil preferredStyle:UIAlertControllerStyleAlert];
  [alert addTextFieldWithConfigurationHandler:^(UITextField *field) {
    field.text = @"Collection";
    field.clearButtonMode = UITextFieldViewModeWhileEditing;
    field.autocapitalizationType = UITextAutocapitalizationTypeNone;
    field.accessibilityLabel = @"Collection name";
  }];
  __block GHOSTIOSScenePanels *owner = self;
  __block UIAlertController *prompt = alert;
  [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
  UIAlertAction *create = [UIAlertAction actionWithTitle:@"Create" style:UIAlertActionStyleDefault
      handler:^(UIAlertAction *action) {
        [owner send:@{@"action": @"collection_new", @"value": prompt.textFields.firstObject.text ?: @""}];
      }];
  [alert addAction:create];
  [alert.textFields.firstObject addAction:[UIAction actionWithHandler:^(__kindof UIAction *event) {
    create.enabled = [((UITextField *)event.sender).text
        stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet].length > 0;
  }] forControlEvents:UIControlEventEditingChanged];
  [presenter presentViewController:alert animated:YES completion:nil];
}

- (BOOL)sendHierarchyAction:(NSString *)action node:(NSDictionary *)node value:(id)value
{
  NSDictionary *current = _nodesByKey[node[@"key"]];
  if (![current[@"id"] isEqual:node[@"id"]] || ![current[@"actions"] containsObject:action]) {
    return NO;
  }
  NSMutableDictionary *request = [NSMutableDictionary dictionaryWithDictionary:
      @{@"action": action, @"id": current[@"id"]}];
  if (value) { request[@"value"] = value; }
  [self send:request];
  return YES;
}

- (void)promptHierarchyAction:(NSString *)action node:(NSDictionary *)node completion:(void (^)(BOOL))complete
{
  NSDictionary *current = _nodesByKey[node[@"key"]];
  if (![current[@"actions"] containsObject:action]) { complete(NO); return; }
  UIViewController *presenter = _overlay.window.rootViewController;
  while (presenter.presentedViewController) { presenter = presenter.presentedViewController; }
  if (!presenter || presenter.isBeingDismissed || [presenter isKindOfClass:UIAlertController.class]) {
    complete(NO);
    return;
  }
  BOOL deleting = [action isEqual:@"delete"];
  UIAlertController *alert = [UIAlertController alertControllerWithTitle:
      deleting ? [NSString stringWithFormat:@"Delete \"%@\"?", current[@"name"]] : @"Rename Object"
      message:deleting ? @"The object will be removed from this scene. You can undo this change." : nil
      preferredStyle:UIAlertControllerStyleAlert];
  if (!deleting) {
    [alert addTextFieldWithConfigurationHandler:^(UITextField *field) {
      field.text = current[@"name"];
      field.clearButtonMode = UITextFieldViewModeWhileEditing;
      field.autocapitalizationType = UITextAutocapitalizationTypeNone;
      field.autocorrectionType = UITextAutocorrectionTypeNo;
      field.accessibilityLabel = @"Object name";
    }];
  }
  __block GHOSTIOSScenePanels *owner = self;
  __block UIAlertController *prompt = alert;
  [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel
      handler:^(UIAlertAction *chosen) { complete(NO); }]];
  UIAlertAction *confirm = [UIAlertAction actionWithTitle:deleting ? @"Delete" : @"Rename"
      style:deleting ? UIAlertActionStyleDestructive : UIAlertActionStyleDefault handler:^(UIAlertAction *chosen) {
        complete([owner sendHierarchyAction:action node:node value:deleting ? nil : prompt.textFields.firstObject.text]);
      }];
  [alert addAction:confirm];
  if (!deleting) {
    [alert.textFields.firstObject addAction:[UIAction actionWithHandler:^(__kindof UIAction *event) {
      NSString *name = ((UITextField *)event.sender).text;
      confirm.enabled = [name stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet].length > 0;
    }] forControlEvents:UIControlEventEditingChanged];
  }
  [presenter presentViewController:alert animated:YES completion:nil];
}

- (UISwipeActionsConfiguration *)hierarchySwipeActionsAtIndexPath:(NSIndexPath *)indexPath leading:(BOOL)leading
{
  NSDictionary *node = _nodesByKey[[_treeDataSource itemIdentifierForIndexPath:indexPath]];
  if (![node[@"kind"] isEqual:@"object"]) { return nil; }
  NSMutableArray *items = [NSMutableArray array];
  __block GHOSTIOSScenePanels *owner = self;
  for (NSString *identifier in leading ? @[@"duplicate", @"rename"] : @[@"delete", @"visibility"]) {
    if (![node[@"actions"] containsObject:identifier]) { continue; }
    BOOL deleting = [identifier isEqual:@"delete"];
    BOOL duplicate = [identifier isEqual:@"duplicate"];
    BOOL visibility = [identifier isEqual:@"visibility"];
    BOOL hidden = [node[@"hidden"] boolValue];
    NSString *title = deleting ? @"Delete" : duplicate ? @"Duplicate" : visibility ? (hidden ? @"Show" : @"Hide") : @"Rename";
    UIContextualAction *item = [UIContextualAction contextualActionWithStyle:
        deleting ? UIContextualActionStyleDestructive : UIContextualActionStyleNormal title:title
        handler:^(UIContextualAction *action, UIView *view, void (^complete)(BOOL)) {
          if (deleting || [identifier isEqual:@"rename"]) {
            dispatch_async(dispatch_get_main_queue(), ^{
              [owner promptHierarchyAction:identifier node:node completion:complete];
            });
          }
          else {
            NSDictionary *current = owner->_nodesByKey[node[@"key"]];
            complete([owner sendHierarchyAction:identifier node:node
                value:visibility ? @(![current[@"hidden"] boolValue]) : nil]);
          }
        }];
    NSString *icon = deleting ? @"TRASH" : duplicate ? @"DUPLICATE" : visibility ? (hidden ? @"HIDE_OFF" : @"HIDE_ON") : @"GREASEPENCIL";
    NSString *symbol = deleting ? @"trash" : duplicate ? @"plus.square.on.square" : visibility ? (hidden ? @"eye" : @"eye.slash") : @"pencil";
    item.image = _icon(icon) ?: [UIImage systemImageNamed:symbol];
    if (!deleting) { item.backgroundColor = duplicate ? UIColor.systemBlueColor : UIColor.systemGrayColor; }
    [items addObject:item];
  }
  if (!items.count) { return nil; }
  UISwipeActionsConfiguration *actions = [UISwipeActionsConfiguration configurationWithActions:items];
  actions.performsFirstActionWithFullSwipe = NO;
  return actions;
}

- (BOOL)collectionView:(UICollectionView *)collectionView shouldSelectItemAtIndexPath:(NSIndexPath *)indexPath
{
  NSDictionary *node = _nodesByKey[[_treeDataSource itemIdentifierForIndexPath:indexPath]];
  return [node[@"kind"] isEqual:@"object"] && [node[@"enabled"] boolValue];
}

- (void)collectionView:(UICollectionView *)collectionView didSelectItemAtIndexPath:(NSIndexPath *)indexPath
{
  if (![self collectionView:collectionView shouldSelectItemAtIndexPath:indexPath]) { return; }
  NSDictionary *node = _nodesByKey[[_treeDataSource itemIdentifierForIndexPath:indexPath]];
  if (!_multipleSelection) {
    for (NSIndexPath *other in collectionView.indexPathsForSelectedItems) {
      if (![other isEqual:indexPath]) { [collectionView deselectItemAtIndexPath:other animated:YES]; }
    }
  }
  [self send:@{@"action": @"select", @"id": node[@"id"], @"extend": @(_multipleSelection), @"value": @YES}];
}

- (BOOL)collectionView:(UICollectionView *)collectionView shouldDeselectItemAtIndexPath:(NSIndexPath *)indexPath
{
  /* An ordinary tap on an already-selected object still makes it the active object. */
  if (!_multipleSelection) { [self collectionView:collectionView didSelectItemAtIndexPath:indexPath]; }
  return _multipleSelection;
}

- (void)collectionView:(UICollectionView *)collectionView didDeselectItemAtIndexPath:(NSIndexPath *)indexPath
{
  NSDictionary *node = _nodesByKey[[_treeDataSource itemIdentifierForIndexPath:indexPath]];
  if (_multipleSelection && [node[@"kind"] isEqual:@"object"]) {
    [self send:@{@"action": @"select", @"id": node[@"id"], @"extend": @YES, @"value": @NO}];
  }
}

- (BOOL)collectionView:(UICollectionView *)collectionView shouldBeginMultipleSelectionInteractionAtIndexPath:(NSIndexPath *)indexPath
{
  return [self collectionView:collectionView shouldSelectItemAtIndexPath:indexPath];
}

- (void)collectionView:(UICollectionView *)collectionView didBeginMultipleSelectionInteractionAtIndexPath:(NSIndexPath *)indexPath
{
  _multipleSelection = YES;
}

- (void)collectionViewDidEndMultipleSelectionInteraction:(UICollectionView *)collectionView
{
  _multipleSelection = NO;
}
@end
