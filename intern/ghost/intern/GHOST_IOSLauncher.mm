/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#import "GHOST_IOSLauncher.hh"
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import "GHOST_IOSDocument.hh"

/* The views drawing Suzanne and the logo are Swift, in the app itself, see
 * `GHOST_IOSLauncher.swift`. They are found by name, as this library is built without Swift. */
@protocol GHOSTIOSLauncherModelView
- (instancetype)initWithModelURL:(NSURL *)url;
- (void)placeModelWithCenter:(CGPoint)center size:(CGFloat)size;
@end

@protocol GHOSTIOSLauncherLogoView
- (instancetype)initWithArtworkURL:(NSURL *)url;
@property(nonatomic, readonly) CGFloat aspectRatio;
@end

static UIColor *launcherColor(uint32_t rgb, CGFloat alpha)
{
  return [UIColor colorWithRed:((rgb >> 16) & 0xff) / 255.0
                         green:((rgb >> 8) & 0xff) / 255.0
                          blue:(rgb & 0xff) / 255.0
                         alpha:alpha];
}

static UIColor *launcherDynamicColor(UIColor *light, UIColor *dark)
{
  return [UIColor colorWithDynamicProvider:^UIColor *(UITraitCollection *traits) {
    return traits.userInterfaceStyle == UIUserInterfaceStyleDark ? dark : light;
  }];
}

/** Blender's brand behind the launcher: charcoal, with the logo's orange and blue as soft light. */
@interface GHOSTIOSLauncherBackground : UIView
@end

@implementation GHOSTIOSLauncherBackground {
  CAGradientLayer *_base, *_orange, *_blue;
}

- (instancetype)initWithFrame:(CGRect)frame
{
  if ((self = [super initWithFrame:frame])) {
    _base = [CAGradientLayer layer];
    _orange = [CAGradientLayer layer];
    _blue = [CAGradientLayer layer];
    _orange.type = _blue.type = kCAGradientLayerRadial;
    for (CALayer *layer in @[_base, _blue, _orange]) {
      [self.layer addSublayer:layer];
    }
    [self registerForTraitChanges:@[UITraitUserInterfaceStyle.class]
                       withAction:@selector(updateColors)];
    [self updateColors];
  }
  return self;
}

- (void)updateColors
{
  UITraitCollection *traits = self.traitCollection;
  CGColorRef (^resolve)(UIColor *) = ^CGColorRef(UIColor *color) {
    return [color resolvedColorWithTraitCollection:traits].CGColor;
  };
  _base.colors = @[
    (id)resolve(launcherDynamicColor(launcherColor(0xFBF8F4, 1), launcherColor(0x1D1E22, 1))),
    (id)resolve(launcherDynamicColor(launcherColor(0xF1ECE6, 1), launcherColor(0x121316, 1))),
  ];
  UIColor *orange = launcherDynamicColor(launcherColor(0xE87D0D, 0.22), launcherColor(0xE87D0D, 0.38));
  UIColor *blue = launcherDynamicColor(launcherColor(0x265787, 0.16), launcherColor(0x265787, 0.55));
  _orange.colors = @[(id)resolve(orange), (id)resolve([orange colorWithAlphaComponent:0])];
  _blue.colors = @[(id)resolve(blue), (id)resolve([blue colorWithAlphaComponent:0])];
}

- (void)layoutSubviews
{
  [super layoutSubviews];
  const CGRect bounds = self.bounds;
  const CGFloat size = MAX(bounds.size.width, bounds.size.height) * 0.9;
  _base.frame = bounds;
  /* The orange glows behind the title, the blue in the top left corner. */
  _orange.frame = CGRectMake(bounds.size.width * 0.62 - size / 2, bounds.size.height * 0.22 - size / 2,
                             size, size);
  _blue.frame = CGRectMake(-size * 0.35 / 0.9, -size * 0.45 / 0.9, size, size);
  for (CAGradientLayer *layer in @[_orange, _blue]) {
    layer.startPoint = CGPointMake(0.5, 0.5);
    layer.endPoint = CGPointMake(1, 1);
  }
}

@end

/** Suzanne, huge on the right and slanted, behind the launcher's title and browser. */
@interface GHOSTIOSLauncherModelStage : UIView
@end

@implementation GHOSTIOSLauncherModelStage {
  UIView<GHOSTIOSLauncherModelView> *_model;
}

- (instancetype)initWithModel:(UIView<GHOSTIOSLauncherModelView> *)model
{
  if ((self = [super initWithFrame:CGRectZero])) {
    _model = [model retain];
    model.frame = self.bounds;
    model.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    [self addSubview:model];
  }
  return self;
}

- (void)dealloc
{
  [_model release];
  [super dealloc];
}

- (void)layoutSubviews
{
  [super layoutSubviews];
  const CGSize size = self.bounds.size;
  [_model placeModelWithCenter:CGPointMake(size.width * 0.86, size.height * 0.46)
                          size:MAX(size.width, size.height) * 1.05];
}

/* Touches around Suzanne reach the launcher. */
- (UIView *)hitTest:(CGPoint)point withEvent:(UIEvent *)event
{
  UIView *hit = [super hitTest:point withEvent:event];
  return hit == self ? nil : hit;
}

@end

/** The title the launcher draws, left blank for Blender's logo to take its place. */
static NSString *const launcher_blank_title = @" ";

static UILabel *launcherTitleLabel(UIView *view)
{
  if ([view isKindOfClass:UILabel.class] &&
      [((UILabel *)view).text isEqualToString:launcher_blank_title] &&
      ((UILabel *)view).font.pointSize > 24)
  {
    return (UILabel *)view;
  }
  for (UIView *child in view.subviews) {
    if (UILabel *label = launcherTitleLabel(child)) {
      return label;
    }
  }
  return nil;
}

/** Blender's logo where the launcher would draw its title. */
@interface GHOSTIOSLauncherLogoStage : UIView
@end

@implementation GHOSTIOSLauncherLogoStage {
  UIView<GHOSTIOSLauncherLogoView> *_logo;
}

- (instancetype)initWithLogo:(UIView<GHOSTIOSLauncherLogoView> *)logo
{
  if ((self = [super initWithFrame:CGRectZero])) {
    _logo = [logo retain];
    [self addSubview:logo];
  }
  return self;
}

- (void)dealloc
{
  [_logo release];
  [super dealloc];
}

- (void)layoutSubviews
{
  [super layoutSubviews];
  const CGSize size = self.bounds.size;
  /* The logo's letters sit on the title's baseline, a little smaller than its letters; the mark
   * rises above them and hangs a little left of the text below. */
  const CGFloat letters = 0.44, baseline = 0.87, hang = 0.035;
  UILabel *title = self.window ? launcherTitleLabel(self.window) : nil;
  CGFloat height, x, baseline_y;
  if (title) {
    const CGRect frame = [self convertRect:title.bounds fromView:title];
    height = title.font.capHeight / letters;
    x = CGRectGetMinX(frame) - height * self->_logo.aspectRatio * hang;
    baseline_y = CGRectGetMinY(frame) + title.font.ascender;
  }
  else {
    /* Where the launcher puts its title for this size. */
    height = size.height * 0.08;
    x = size.width * 0.125;
    baseline_y = size.height * 0.222;
  }
  _logo.frame = CGRectMake(x, baseline_y - height * baseline, height * _logo.aspectRatio, height);
}

- (void)didMoveToWindow
{
  [super didMoveToWindow];
  if (self.window == nil) {
    return;
  }
  /* The launcher may lay out its title after its accessories: follow it for a moment. */
  for (int i = 1; i <= 10; i++) {
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, int64_t(i * 0.1 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
                     [self setNeedsLayout];
                   });
  }
}

- (UIView *)hitTest:(CGPoint)point withEvent:(UIEvent *)event
{
  return nil;
}

@end

static void launcherStyle(UIDocumentViewControllerLaunchOptions *launch)
{
  UIBackgroundConfiguration *background = [UIBackgroundConfiguration clearConfiguration];
  background.customView = [[[GHOSTIOSLauncherBackground alloc] initWithFrame:CGRectZero] autorelease];
  launch.background = background;
  launch.browserViewController.view.tintColor = launcherColor(0xE87D0D, 1);

  NSBundle *bundle = NSBundle.mainBundle;
  NSURL *model_url = [bundle URLForResource:@"Suzanne" withExtension:@"usdz"];
  NSURL *logo_url = [bundle URLForResource:@"blender_logo_mono" withExtension:@"svg"];
  Class model_class = NSClassFromString(@"GHOSTIOSLauncherModelView");
  Class logo_class = NSClassFromString(@"GHOSTIOSLauncherLogoView");
  if (model_url && model_class) {
    UIView<GHOSTIOSLauncherModelView> *model = [[[model_class alloc] initWithModelURL:model_url]
        autorelease];
    launch.backgroundAccessoryView = [[[GHOSTIOSLauncherModelStage alloc] initWithModel:model]
        autorelease];
  }
  if (logo_url && logo_class) {
    UIView<GHOSTIOSLauncherLogoView> *logo = [[[logo_class alloc] initWithArtworkURL:logo_url]
        autorelease];
    launch.foregroundAccessoryView = [[[GHOSTIOSLauncherLogoStage alloc] initWithLogo:logo]
        autorelease];
    /* The logo is the title; VoiceOver reads it as "Blender". */
    launch.title = launcher_blank_title;
  }
}

/* -------------------------------------------------------------------- */
/** \name Templates
 * \{ */

static NSArray<NSDictionary *> *launcherTemplates()
{
  return GHOST_ios_document_templates();
}

/** A real preview of the starting scene, with a native selection outline. */
@interface GHOSTIOSTemplateCell : UICollectionViewCell
@property(nonatomic, readonly) UIImageView *symbol;
@property(nonatomic, readonly) UILabel *name;
@property(nonatomic, readonly) UIButton *credit;
@property(nonatomic, copy) NSString *templateIdentifier;
@end

@implementation GHOSTIOSTemplateCell

- (instancetype)initWithFrame:(CGRect)frame
{
  if ((self = [super initWithFrame:frame])) {
    _symbol = [[UIImageView alloc] init];
    _symbol.contentMode = UIViewContentModeScaleAspectFit;
    _symbol.layer.cornerRadius = 6;
    _symbol.clipsToBounds = YES;
    _name = [[UILabel alloc] init];
    _name.font = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
    _name.adjustsFontForContentSizeCategory = YES;
    _name.textAlignment = NSTextAlignmentCenter;
    _name.numberOfLines = 0;
    _credit = [[UIButton buttonWithType:UIButtonTypeSystem] retain];
    _credit.titleLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleCaption1];
    _credit.titleLabel.adjustsFontForContentSizeCategory = YES;
    _credit.titleLabel.numberOfLines = 0;
    _credit.titleLabel.textAlignment = NSTextAlignmentCenter;
    _credit.accessibilityIdentifier = @"blender.template.attribution";
    UIStackView *stack = [[[UIStackView alloc] initWithArrangedSubviews:@[_symbol, _name, _credit]] autorelease];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 12;
    stack.alignment = UIStackViewAlignmentFill;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [self.contentView addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[
      [stack.leadingAnchor constraintEqualToAnchor:self.contentView.leadingAnchor constant:10],
      [stack.trailingAnchor constraintEqualToAnchor:self.contentView.trailingAnchor constant:-10],
      [stack.topAnchor constraintEqualToAnchor:self.contentView.topAnchor constant:10],
      [stack.bottomAnchor constraintEqualToAnchor:self.contentView.bottomAnchor constant:-10],
      [_symbol.heightAnchor constraintEqualToAnchor:_symbol.widthAnchor multiplier:420.0 / 640.0],
    ]];
    self.isAccessibilityElement = YES;
    self.accessibilityTraits = UIAccessibilityTraitButton;
    [self addInteraction:[[[UIPointerInteraction alloc] initWithDelegate:nil] autorelease]];
  }
  return self;
}

- (void)dealloc
{
  [_symbol release];
  [_name release];
  [_credit release];
  [_templateIdentifier release];
  [super dealloc];
}

- (void)updateConfigurationUsingState:(UICellConfigurationState *)state
{
  UIBackgroundConfiguration *background = [UIBackgroundConfiguration listCellConfiguration];
  background.cornerRadius = 8;
  background.strokeColor = state.isSelected ? self.tintColor : UIColor.clearColor;
  background.strokeWidth = state.isSelected ? 2 : 0;
  background.backgroundColor = state.isHighlighted || state.isSelected ?
                                   UIColor.tertiarySystemFillColor :
                                   UIColor.secondarySystemBackgroundColor;
  self.backgroundConfiguration = background;
  _name.textColor = state.isSelected ? self.tintColor : UIColor.labelColor;
  self.accessibilityTraits = UIAccessibilityTraitButton | (state.isSelected ? UIAccessibilityTraitSelected : 0);
}

- (void)tintColorDidChange
{
  [super tintColorDidChange];
  [self setNeedsUpdateConfiguration];
}

@end

/** Attribution stays available without leaving the native chooser or opening a document. */
@interface GHOSTIOSTemplateCredits : UIViewController
@property(nonatomic, copy) NSDictionary *credit;
@end

@implementation GHOSTIOSTemplateCredits
- (void)dealloc
{
  [_credit release];
  [super dealloc];
}
- (void)viewDidLoad
{
  [super viewDidLoad];
  self.title = @"Asset Credits";
  self.view.backgroundColor = UIColor.systemBackgroundColor;
  UITextView *text = [[[UITextView alloc] init] autorelease];
  text.editable = NO;
  text.adjustsFontForContentSizeCategory = YES;
  text.backgroundColor = UIColor.clearColor;
  text.textContainerInset = UIEdgeInsetsMake(20, 20, 20, 20);
  NSString *body = [NSString stringWithFormat:@"%@\n%@\n\n%@\n\n%@\n\n%@\n\nAdaptation does not imply endorsement by the original creators.",
      _credit[@"title"], _credit[@"author"], _credit[@"licenseURL"],
      _credit[@"sourceURL"], _credit[@"changes"]];
  NSMutableAttributedString *content = [[[NSMutableAttributedString alloc] initWithString:body
      attributes:@{NSFontAttributeName: [UIFont preferredFontForTextStyle:UIFontTextStyleBody],
                   NSForegroundColorAttributeName: UIColor.labelColor}] autorelease];
  for (NSString *key in @[@"licenseURL", @"sourceURL"]) {
    NSString *link = _credit[key];
    NSURL *url = [NSURL URLWithString:link];
    if ([url.scheme isEqual:@"https"]) {
      [content addAttribute:NSLinkAttributeName value:url range:[body rangeOfString:link]];
    }
  }
  text.attributedText = content;
  text.translatesAutoresizingMaskIntoConstraints = NO;
  [self.view addSubview:text];
  [NSLayoutConstraint activateConstraints:@[
    [text.leadingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.leadingAnchor],
    [text.trailingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.trailingAnchor],
    [text.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor],
    [text.bottomAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.bottomAnchor],
  ]];
}
@end

/* The template chooser in SwiftUI (`intern/swift/TemplatePicker.swift`). */
@interface UIViewController (GHOSTIOSSwiftTemplatePicker)
- (instancetype)initWithTemplates:(NSArray<NSDictionary *> * (^)(void))templates
                           manage:(void (^)(NSDictionary *entry, NSString *action, NSString *title,
                                            void (^done)(BOOL success)))manage
                       completion:(void (^)(NSString *app_template))completion;
@end

/** A grid of Blender's templates, to start a new document from one. */
@interface GHOSTIOSTemplatePicker : UICollectionViewController <UIAdaptivePresentationControllerDelegate, UISearchResultsUpdating>
@property(nonatomic, copy) void (^completion)(NSString *app_template);
@end

@implementation GHOSTIOSTemplatePicker {
  NSArray<NSDictionary *> *_templates;
  NSArray<NSArray *> *_sections;
  NSArray<NSString *> *_sectionTitles;
  UISearchController *_search;
  BOOL _finished;
}

- (instancetype)init
{
  UICollectionViewCompositionalLayout *layout = [[[UICollectionViewCompositionalLayout alloc]
      initWithSectionProvider:^NSCollectionLayoutSection *(NSInteger,
                                                            id<NSCollectionLayoutEnvironment> environment) {
        const CGFloat width = environment.container.effectiveContentSize.width;
        const BOOL largeText = UIContentSizeCategoryIsAccessibilityCategory(environment.traitCollection.preferredContentSizeCategory);
        const NSInteger columns = largeText || width < 360 ? 1 : width > 700 ? 3 : 2;
        NSCollectionLayoutSize *item_size = [NSCollectionLayoutSize
            sizeWithWidthDimension:[NSCollectionLayoutDimension fractionalWidthDimension:1.0 / columns]
                   heightDimension:[NSCollectionLayoutDimension estimatedDimension:220]];
        NSCollectionLayoutItem *item = [NSCollectionLayoutItem itemWithLayoutSize:item_size];
        item.contentInsets = NSDirectionalEdgeInsetsMake(8, 8, 8, 8);
        NSCollectionLayoutSize *group_size = [NSCollectionLayoutSize
            sizeWithWidthDimension:[NSCollectionLayoutDimension fractionalWidthDimension:1.0]
                   heightDimension:[NSCollectionLayoutDimension estimatedDimension:220]];
        NSCollectionLayoutGroup *group = [NSCollectionLayoutGroup
            horizontalGroupWithLayoutSize:group_size
                                 repeatingSubitem:item
                                            count:columns];
        NSCollectionLayoutSection *section = [NSCollectionLayoutSection sectionWithGroup:group];
        section.interGroupSpacing = 16;
        NSCollectionLayoutBoundarySupplementaryItem *header = [NSCollectionLayoutBoundarySupplementaryItem
            boundarySupplementaryItemWithLayoutSize:[NSCollectionLayoutSize
                sizeWithWidthDimension:[NSCollectionLayoutDimension fractionalWidthDimension:1]
                heightDimension:[NSCollectionLayoutDimension estimatedDimension:44]]
            elementKind:UICollectionElementKindSectionHeader alignment:NSRectAlignmentTop];
        section.boundarySupplementaryItems = @[header];
        section.contentInsets = NSDirectionalEdgeInsetsMake(12, 12, 12, 12);
        return section;
      }] autorelease];
  if ((self = [super initWithCollectionViewLayout:layout])) {
    [self reloadTemplates];
    self.title = @"Choose a Template";
    self.preferredContentSize = CGSizeMake(780, 640);
  }
  return self;
}

- (void)dealloc
{
  [_templates release];
  [_sections release];
  [_sectionTitles release];
  [_search release];
  [_completion release];
  [super dealloc];
}

- (void)viewDidLoad
{
  [super viewDidLoad];
  [self.collectionView registerClass:GHOSTIOSTemplateCell.class forCellWithReuseIdentifier:@"template"];
  [self.collectionView registerClass:UICollectionReusableView.class
      forSupplementaryViewOfKind:UICollectionElementKindSectionHeader withReuseIdentifier:@"section"];
  self.collectionView.backgroundColor = UIColor.systemBackgroundColor;
  self.collectionView.selectionFollowsFocus = NO;
  _search = [[UISearchController alloc] initWithSearchResultsController:nil];
  _search.obscuresBackgroundDuringPresentation = NO;
  _search.hidesNavigationBarDuringPresentation = NO;
  _search.searchResultsUpdater = self;
  _search.searchBar.placeholder = @"Search Templates";
  self.navigationItem.searchController = _search;
  self.navigationItem.hidesSearchBarWhenScrolling = NO;
  self.definesPresentationContext = YES;
  __block GHOSTIOSTemplatePicker *picker = self;
  self.navigationItem.leftBarButtonItem = [[[UIBarButtonItem alloc]
      initWithBarButtonSystemItem:UIBarButtonSystemItemCancel
                    primaryAction:[UIAction actionWithHandler:^(__kindof UIAction *) {
                      [picker finish:nil];
                    }]] autorelease];
}

- (void)reloadTemplates
{
  [_templates release];
  _templates = [launcherTemplates() retain];
  [self filterTemplates];
}

- (void)filterTemplates
{
  NSString *query = [_search.searchBar.text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
  NSMutableArray *bundled = [NSMutableArray array], *personal = [NSMutableArray array];
  for (NSDictionary *entry in _templates) {
    if (query.length && [entry[@"title"] rangeOfString:query options:NSCaseInsensitiveSearch | NSDiacriticInsensitiveSearch].location == NSNotFound) { continue; }
    NSMutableArray *group = [entry[@"personal"] boolValue] ? personal : bundled;
    [group addObject:entry];
  }
  NSMutableArray *sections = [NSMutableArray array], *titles = [NSMutableArray array];
  if (bundled.count) { [sections addObject:bundled]; [titles addObject:@"Blender"]; }
  if (personal.count) { [sections addObject:personal]; [titles addObject:@"My Templates"]; }
  [_sections release];
  _sections = [sections copy];
  [_sectionTitles release];
  _sectionTitles = [titles copy];
  if (self.isViewLoaded) {
    [self.collectionView reloadData];
    self.contentUnavailableConfiguration = sections.count ? nil : [UIContentUnavailableConfiguration searchConfiguration];
  }
}

- (void)updateSearchResultsForSearchController:(UISearchController *)searchController
{
  [self filterTemplates];
}

- (NSInteger)numberOfSectionsInCollectionView:(UICollectionView *)collectionView
{
  return _sections.count;
}

- (UICollectionReusableView *)collectionView:(UICollectionView *)collectionView
    viewForSupplementaryElementOfKind:(NSString *)kind atIndexPath:(NSIndexPath *)indexPath
{
  UICollectionReusableView *view = [collectionView dequeueReusableSupplementaryViewOfKind:kind
      withReuseIdentifier:@"section" forIndexPath:indexPath];
  UILabel *title = (UILabel *)[view viewWithTag:1];
  if (!title) {
    title = [[[UILabel alloc] init] autorelease];
    title.tag = 1;
    title.font = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
    title.adjustsFontForContentSizeCategory = YES;
    title.numberOfLines = 0;
    title.translatesAutoresizingMaskIntoConstraints = NO;
    [view addSubview:title];
    [NSLayoutConstraint activateConstraints:@[
      [title.leadingAnchor constraintEqualToAnchor:view.leadingAnchor constant:8],
      [title.trailingAnchor constraintEqualToAnchor:view.trailingAnchor constant:-8],
      [title.topAnchor constraintEqualToAnchor:view.topAnchor constant:8],
      [title.bottomAnchor constraintEqualToAnchor:view.bottomAnchor constant:-8],
    ]];
  }
  title.text = _sectionTitles[indexPath.section];
  return view;
}

- (NSInteger)collectionView:(UICollectionView *)view numberOfItemsInSection:(NSInteger)section
{
  return _sections[section].count;
}

- (UICollectionViewCell *)collectionView:(UICollectionView *)view
                  cellForItemAtIndexPath:(NSIndexPath *)index
{
  GHOSTIOSTemplateCell *cell = [view dequeueReusableCellWithReuseIdentifier:@"template"
                                                                forIndexPath:index];
  NSDictionary *item = _sections[index.section][index.item];
  cell.templateIdentifier = item[@"id"];
  cell.symbol.image = nil;
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    UIImage *preview = [UIImage imageWithContentsOfFile:[item[@"previewURL"] path]];
    [preview prepareForDisplayWithCompletionHandler:^(UIImage *image) {
      dispatch_async(dispatch_get_main_queue(), ^{
        if ([cell.templateIdentifier isEqual:item[@"id"]]) { cell.symbol.image = image; }
      });
    }];
  });
  cell.name.text = item[@"title"];
  NSDictionary *credit = item[@"attribution"];
  cell.credit.hidden = credit == nil;
  cell.isAccessibilityElement = credit == nil;
  cell.name.isAccessibilityElement = credit != nil;
  cell.name.accessibilityTraits = UIAccessibilityTraitButton;
  [cell.credit removeActionForIdentifier:@"credits" forControlEvents:UIControlEventTouchUpInside];
  if (credit) {
    [cell.credit setTitle:[NSString stringWithFormat:@"%@ · %@\n%@ · Adapted",
        credit[@"title"], credit[@"author"], credit[@"license"]] forState:UIControlStateNormal];
    cell.credit.accessibilityLabel = [NSString stringWithFormat:@"%@. Asset credits and license", credit[@"title"]];
    __block GHOSTIOSTemplatePicker *picker = self;
    [cell.credit addAction:[UIAction actionWithTitle:@"Asset Credits" image:nil identifier:@"credits"
        handler:^(__kindof UIAction *) {
      GHOSTIOSTemplateCredits *details = [[[GHOSTIOSTemplateCredits alloc] init] autorelease];
      details.credit = credit;
      [picker.navigationController pushViewController:details animated:YES];
    }] forControlEvents:UIControlEventTouchUpInside];
  }
  cell.accessibilityLabel = item[@"title"];
  cell.accessibilityIdentifier = [@"blender.template." stringByAppendingString:item[@"id"]];
  return cell;
}

- (void)collectionView:(UICollectionView *)view didSelectItemAtIndexPath:(NSIndexPath *)index
{
  [self finish:_sections[index.section][index.item][@"id"]];
}

- (void)manageTemplate:(NSDictionary *)entry action:(NSString *)action title:(NSString *)title
{
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    const BOOL success = GHOST_ios_template_manage(entry[@"id"], action, title);
    dispatch_async(dispatch_get_main_queue(), ^{
      if (success) {
        [self reloadTemplates];
      }
      else {
        UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Could Not Update Template"
            message:@"Check available storage and try again." preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
        [self presentViewController:alert animated:YES completion:nil];
      }
    });
  });
}

- (UIContextMenuConfiguration *)collectionView:(UICollectionView *)collectionView
    contextMenuConfigurationForItemAtIndexPath:(NSIndexPath *)index point:(CGPoint)point
{
  NSDictionary *entry = _sections[index.section][index.item];
  return [UIContextMenuConfiguration configurationWithIdentifier:entry[@"id"] previewProvider:nil
      actionProvider:^UIMenu *(NSArray<UIMenuElement *> *) {
    NSMutableArray *items = [NSMutableArray arrayWithObject:[UIAction actionWithTitle:@"Duplicate"
        image:[UIImage systemImageNamed:@"plus.square.on.square"] identifier:nil handler:^(__kindof UIAction *) {
      [self manageTemplate:entry action:@"duplicate" title:nil];
    }]];
    if ([entry[@"personal"] boolValue]) {
      [items addObject:[UIAction actionWithTitle:@"Rename" image:[UIImage systemImageNamed:@"pencil"]
          identifier:nil handler:^(__kindof UIAction *) {
        UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Rename Template"
            message:nil preferredStyle:UIAlertControllerStyleAlert];
        [alert addTextFieldWithConfigurationHandler:^(UITextField *field) { field.text = entry[@"title"]; }];
        [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
        [alert addAction:[UIAlertAction actionWithTitle:@"Rename" style:UIAlertActionStyleDefault handler:^(UIAlertAction *) {
          NSString *name = [alert.textFields.firstObject.text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
          if (name.length) { [self manageTemplate:entry action:@"rename" title:name]; }
        }]];
        [self presentViewController:alert animated:YES completion:nil];
      }]];
      UIAction *remove = [UIAction actionWithTitle:@"Delete" image:[UIImage systemImageNamed:@"trash"]
          identifier:nil handler:^(__kindof UIAction *) {
        UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Delete Template?"
            message:@"Documents created from this template will not be deleted."
            preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
        [alert addAction:[UIAlertAction actionWithTitle:@"Delete" style:UIAlertActionStyleDestructive handler:^(UIAlertAction *) {
          [self manageTemplate:entry action:@"delete" title:nil];
        }]];
        [self presentViewController:alert animated:YES completion:nil];
      }];
      remove.attributes = UIMenuElementAttributesDestructive;
      [items addObject:remove];
    }
    return [UIMenu menuWithTitle:@"" children:items];
  }];
}

- (void)presentationControllerDidDismiss:(UIPresentationController *)presentationController
{
  [self finish:nil];
}

- (void)finish:(NSString *)app_template
{
  if (_finished) {
    return;
  }
  _finished = YES;
  void (^completion)(NSString *) = [[_completion retain] autorelease];
  if (self.presentingViewController) {
    [self.presentingViewController dismissViewControllerAnimated:YES completion:^{
      if (completion) { completion(app_template); }
    }];
  }
  else if (completion) { completion(app_template); }
}

@end

/** \} */

/* -------------------------------------------------------------------- */
/** \name Launcher
 * \{ */

/** Where a document was open when the app was left, to continue there as Pages does. */
static NSString *const launcher_open_document_key = @"BlenderOpenDocument";
/** Choosing a template before creating a document. */
static UIDocumentCreationIntent const launcher_template_intent = @"template";
static UIDocumentCreationIntent const launcher_general_intent = @"general";

/**
 * The document launcher, in the app's first window, shown at once on launch while Blender starts
 * behind it. It owns the open document; Blender's window shows only while one is open.
 */
@interface GHOSTIOSLauncherController : UIDocumentViewController <UIDocumentBrowserViewControllerDelegate>
- (void)openDocumentAtURL:(NSURL *)url;
- (void)chooseTemplate:(void (^)(NSString *app_template))completion;
- (void)newDocumentFrom:(UIViewController *)presenter;
@end

static GHOSTIOSLauncherController *launcher_controller = nil;
/** A file the system asked to open before the launcher existed. */
static NSURL *launcher_pending_url = nil;

@implementation GHOSTIOSLauncherController {
  /** A document is being opened: Blender loads one file at a time. */
  BOOL _opening;
  BOOL _creating;
  NSString *_creatingTemplate;
  NSURL *_createdURL;
  NSDictionary *_createdPresentation;
  NSURL *_creationSource;
}

- (void)viewDidLoad
{
  [super viewDidLoad];
  /* The document view, which iOS zooms between the browser and the open document: the 3D
   * viewport's grey, so opening and closing never flash white. */
  self.view.backgroundColor = launcherColor(0x3D3D3D, 1);
  UIDocumentViewControllerLaunchOptions *launch = self.launchOptions;
  launch.title = @"Blender";
  if (@available(iOS 27.0, *)) {
    launch.subtitle = @"Create a new scene or open one";
  }
  /* As in Pages: choosing a template first, or starting at once from General. Both create
   * through the browser, which places the document and lists it in Recents. */
  UIAction *choose = [UIDocumentViewControllerLaunchOptions
      createDocumentActionWithIntent:launcher_template_intent];
  choose.title = @"Choose a Template";
  launch.primaryAction = choose;
  UIAction *start = [UIDocumentViewControllerLaunchOptions
      createDocumentActionWithIntent:launcher_general_intent];
  start.title = @"Start Creating";
  launch.secondaryAction = start;
  /* The browser lists, opens and creates Blender files. The launcher's own browser knows no file
   * types: it would list nothing, not even in Recents, and create nothing. */
  /* The type the app declares for `.blend` files, exactly: dragged files carry it, and the
   * browser accepts only drops of its own types. */
  NSArray *document_types = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleDocumentTypes"];
  NSArray *content_types = [document_types.firstObject objectForKey:@"LSItemContentTypes"];
  NSString *blend_identifier = content_types.firstObject;
  UTType *blend_type = (blend_identifier ? [UTType typeWithIdentifier:blend_identifier] : nil) ?:
                       [UTType typeWithFilenameExtension:@"blend" conformingToType:UTTypeData];
  UIDocumentBrowserViewController *browser = [[[UIDocumentBrowserViewController alloc]
      initForOpeningContentTypes:@[blend_type]] autorelease];
  browser.delegate = self;
  browser.allowsDocumentCreation = YES;
  /* A Blender file, not a sheet of paper: square, like its previews. Set before the browser first
   * draws: changing the create tile afterwards loses its "+". */
  browser.localizedCreateDocumentActionTitle = @"Create Blender File";
  browser.defaultDocumentAspectRatio = 1.0;
  /* Picking several is what lets the browser select files, to move or share them, as Files does.
   * Opening takes the first: Blender works on one file at a time. */
  browser.allowsPickingMultipleItems = YES;
  launch.browserViewController = browser;
  launcherStyle(launch);

  /* Save before leaving the foreground, while the GPU can still render the file's preview. */
  [NSNotificationCenter.defaultCenter addObserver:self
                                         selector:@selector(sceneWillDeactivate:)
                                             name:@"BlenderSceneInactive"
                                           object:nil];

  NSURL *url = [[launcher_pending_url retain] autorelease];
  [launcher_pending_url release];
  launcher_pending_url = nil;
  if (url == nil) {
    /* Continue in the document that was open when the app was left. */
    NSData *bookmark = [NSUserDefaults.standardUserDefaults dataForKey:launcher_open_document_key];
    BOOL stale = NO;
    url = bookmark ? [NSURL URLByResolvingBookmarkData:bookmark options:0 relativeToURL:nil
                                   bookmarkDataIsStale:&stale error:nil] :
                     nil;
  }
  if (url) {
    [self openDocumentAtURL:url];
  }
}

- (void)dealloc
{
  [NSNotificationCenter.defaultCenter removeObserver:self];
  [_creatingTemplate release];
  [_createdURL release];
  [_createdPresentation release];
  [_creationSource release];
  [super dealloc];
}

- (void)creationFailed:(UIViewController *)presenter
{
  _creating = NO;
  [_creatingTemplate release];
  _creatingTemplate = nil;
  if (_creationSource) {
    [NSFileManager.defaultManager removeItemAtURL:_creationSource.URLByDeletingLastPathComponent error:nil];
    [_creationSource release];
    _creationSource = nil;
  }
  UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Could Not Create Document"
      message:@"Your current document has not been replaced. Check available storage and try again."
      preferredStyle:UIAlertControllerStyleAlert];
  [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
  [presenter presentViewController:alert animated:YES completion:nil];
}

- (void)recordCreatedTemplateAtURL:(NSURL *)url
{
  if (!_creatingTemplate) { return; }
  [_createdURL release];
  _createdURL = [url copy];
  [_createdPresentation release];
  _createdPresentation = nil;
  for (NSDictionary *entry in launcherTemplates()) {
    if ([entry[@"id"] isEqual:_creatingTemplate.length ? _creatingTemplate : @"general"]) {
      _createdPresentation = [entry[@"presentation"] copy];
      break;
    }
  }
  [_creatingTemplate release];
  _creatingTemplate = nil;
  _creating = NO;
}

- (void)newDocumentFrom:(UIViewController *)presenter
{
  if (_creating || _opening || presenter.presentedViewController) { return; }
  _creating = YES;
  GHOSTIOSTemplatePicker *picker = [[[GHOSTIOSTemplatePicker alloc] init] autorelease];
  picker.completion = ^(NSString *identifier) {
    if (!identifier) { self->_creating = NO; return; }
    [self->_creatingTemplate release];
    self->_creatingTemplate = [identifier copy];
    GHOST_ios_document_create(identifier, ^(NSURL *source) {
      if (!source) { [self creationFailed:presenter]; return; }
      [self.launchOptions.browserViewController importDocumentAtURL:source
          nextToDocumentAtURL:self.document.fileURL mode:UIDocumentBrowserImportModeMove
          completionHandler:^(NSURL *destination, NSError *error) {
        [NSFileManager.defaultManager removeItemAtURL:source.URLByDeletingLastPathComponent error:nil];
        if (!destination || error) { [self creationFailed:presenter]; return; }
        [self recordCreatedTemplateAtURL:destination];
        [self openDocumentAtURL:destination];
      }];
    });
  };
  UINavigationController *sheet = [[[UINavigationController alloc] initWithRootViewController:picker] autorelease];
  sheet.modalPresentationStyle = UIModalPresentationFormSheet;
  sheet.preferredContentSize = picker.preferredContentSize;
  sheet.presentationController.delegate = picker;
  [presenter presentViewController:sheet animated:YES completion:nil];
}

- (void)openDocumentAtURL:(NSURL *)url
{
  UIDocument *previous = self.document;
  const BOOL previous_open = previous && !(previous.documentState & UIDocumentStateClosed);
  /* A second tap while a file opens, or opening the file that is open, changes nothing. */
  if (_opening || (previous_open && [previous.fileURL.URLByStandardizingPath
                                         isEqual:url.URLByStandardizingPath]))
  {
    return;
  }
  _opening = YES;
  /* Should iOS never report the document open, don't refuse files from then on. */
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, int64_t(60 * NSEC_PER_SEC)),
                 dispatch_get_main_queue(), ^{
                   _opening = NO;
                 });
  GHOSTIOSBlendDocument *document = [[[GHOSTIOSBlendDocument alloc] initWithFileURL:url] autorelease];
  document.templateDocument = [_createdURL.URLByStandardizingPath isEqual:url.URLByStandardizingPath];
  if (!previous_open) {
    self.document = document;
    return;
  }
  /* One file at a time: the current one saves as it closes. */
  GHOST_ios_document_window_set_visible(false, true);
  [previous closeWithCompletionHandler:^(BOOL success) {
    if (success) { self.document = document; }
    else {
      self->_opening = NO;
      UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Could Not Save Document"
          message:@"The current document could not be saved. It has not been replaced."
          preferredStyle:UIAlertControllerStyleAlert];
      [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:^(UIAlertAction *) {
        GHOST_ios_document_window_set_visible(true, true);
      }]];
      [self presentViewController:alert animated:YES completion:nil];
    }
  }];
}

/** Present Blender's templates; `completion` gets the chosen one, or nil. */
- (void)chooseTemplate:(void (^)(NSString *app_template))completion
{
  if (Class swift_class = [NSUserDefaults.standardUserDefaults boolForKey:@"BlenderUIKitInterface"] ?
                              nil :
                              NSClassFromString(@"GHOSTIOSSwiftTemplatePicker"))
  {
    UIViewController *picker = [[[swift_class alloc] initWithTemplates:^NSArray<NSDictionary *> * {
      return launcherTemplates();
    } manage:^(NSDictionary *entry, NSString *action, NSString *title, void (^done)(BOOL)) {
      void (^finished)(BOOL) = [[done copy] autorelease];
      dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        const BOOL success = GHOST_ios_template_manage(entry[@"id"], action, title);
        dispatch_async(dispatch_get_main_queue(), ^{ finished(success); });
      });
    } completion:completion] autorelease];
    picker.modalPresentationStyle = UIModalPresentationFormSheet;
    /* Swiping the sheet away cancels, like Cancel. */
    picker.presentationController.delegate = (id<UIAdaptivePresentationControllerDelegate>)picker;
    [self presentViewController:picker animated:YES completion:nil];
    return;
  }
  GHOSTIOSTemplatePicker *picker = [[[GHOSTIOSTemplatePicker alloc] init] autorelease];
  picker.completion = completion;
  UINavigationController *sheet = [[[UINavigationController alloc] initWithRootViewController:picker]
      autorelease];
  sheet.modalPresentationStyle = UIModalPresentationFormSheet;
  sheet.preferredContentSize = picker.preferredContentSize;
  /* Swiping the sheet away cancels, like Cancel. */
  sheet.presentationController.delegate = (id<UIAdaptivePresentationControllerDelegate>)picker;
  [self presentViewController:sheet animated:YES completion:nil];
}

- (void)setDocument:(UIDocument *)document
{
  [super setDocument:document];
  if (document == nil) {
    _opening = NO;
    /* Back in the launcher: the next launch starts there too. */
    [NSUserDefaults.standardUserDefaults removeObjectForKey:launcher_open_document_key];
    GHOST_ios_document_window_set_visible(false, true);
  }
}

- (void)documentDidOpen
{
  [super documentDidOpen];
  _opening = NO;
  NSData *bookmark = [self.document.fileURL bookmarkDataWithOptions:0
                                     includingResourceValuesForKeys:nil
                                                      relativeToURL:nil
                                                              error:nil];
  if (bookmark) {
    [NSUserDefaults.standardUserDefaults setObject:bookmark forKey:launcher_open_document_key];
  }
  /* Blender has loaded the file: show it. */
  GHOST_ios_document_window_set_visible(true, true);
  if ([_createdURL.URLByStandardizingPath isEqual:self.document.fileURL.URLByStandardizingPath]) {
    [NSNotificationCenter.defaultCenter postNotificationName:@"BlenderTemplatePresentation"
        object:nil userInfo:_createdPresentation];
    [_createdURL release];
    _createdURL = nil;
    [_createdPresentation release];
    _createdPresentation = nil;
  }
}


- (void)sceneWillDeactivate:(NSNotification *)notification
{
  if ([self.document isKindOfClass:GHOSTIOSBlendDocument.class]) {
    [(GHOSTIOSBlendDocument *)self.document saveChangesWithCompletionHandler:nil];
  }
}

/* Document browser. */

- (void)documentBrowser:(UIDocumentBrowserViewController *)browser
    didRequestDocumentCreationWithHandler:(void (^)(NSURL *, UIDocumentBrowserImportMode))importHandler
{
  void (^create)(NSString *) = ^(NSString *app_template) {
    if (app_template == nil) {
      importHandler(nil, UIDocumentBrowserImportModeNone);
      return;
    }
    [self->_creatingTemplate release];
    self->_creatingTemplate = [app_template copy];
    GHOST_ios_document_create(app_template, ^(NSURL *url) {
      [self->_creationSource release];
      self->_creationSource = [url copy];
      importHandler(url, url ? UIDocumentBrowserImportModeMove : UIDocumentBrowserImportModeNone);
      if (!url) { [self creationFailed:self]; }
    });
  };
  if ([browser.activeDocumentCreationIntent isEqualToString:launcher_general_intent]) {
    create(@"");
    return;
  }
  [self chooseTemplate:create];
}

- (void)documentBrowser:(UIDocumentBrowserViewController *)browser
    didImportDocumentAtURL:(NSURL *)sourceURL
          toDestinationURL:(NSURL *)destinationURL
{
  if (_creatingTemplate) { [self recordCreatedTemplateAtURL:destinationURL]; }
  if ([_creationSource isEqual:sourceURL]) {
    [NSFileManager.defaultManager removeItemAtURL:sourceURL.URLByDeletingLastPathComponent error:nil];
    [_creationSource release];
    _creationSource = nil;
  }
  [self openDocumentAtURL:destinationURL];
}

- (void)documentBrowser:(UIDocumentBrowserViewController *)browser
    failedToImportDocumentAtURL:(NSURL *)documentURL error:(NSError *)error
{
  [self creationFailed:self];
}

- (void)documentBrowser:(UIDocumentBrowserViewController *)browser
    didPickDocumentsAtURLs:(NSArray<NSURL *> *)documentURLs
{
  if (documentURLs.firstObject) {
    [self openDocumentAtURL:documentURLs.firstObject];
  }
}

@end

UIViewController *GHOST_ios_launcher_controller()
{
  if (launcher_controller == nil) {
    launcher_controller = [[GHOSTIOSLauncherController alloc] initWithDocument:nil];
  }
  return launcher_controller;
}

void GHOST_ios_launcher_new_document(UIViewController *presenter)
{
  GHOST_ios_launcher_controller();
  [launcher_controller newDocumentFrom:presenter];
}

void GHOST_ios_document_open_url(NSURL *url)
{
  if (launcher_controller.isViewLoaded) {
    [launcher_controller openDocumentAtURL:url];
    return;
  }
  [launcher_pending_url release];
  launcher_pending_url = [url retain];
}

UIDocument *GHOST_ios_launcher_document()
{
  return launcher_controller.document;
}

bool GHOST_ios_launcher_can_show_documents()
{
  return launcher_controller.document != nil &&
         launcher_controller.navigationItem.backAction != nil;
}

void GHOST_ios_launcher_show_documents(id sender)
{
  UIDocument *document = launcher_controller.document;
  if ([document isKindOfClass:GHOSTIOSBlendDocument.class] && document.hasUnsavedChanges) {
    /* Save first, while Blender's window is still shown for the preview. */
    [(GHOSTIOSBlendDocument *)document saveChangesWithCompletionHandler:^(BOOL) {
      GHOST_ios_launcher_show_documents(sender);
    }];
    return;
  }
  NSURL *url = [[document.fileURL retain] autorelease];
  /* Swap Blender's window for a picture of it in the document view, which iOS then shrinks into
   * the document's place in the browser, as Pages does. */
  UIView *view = launcher_controller.view;
  if (UIView *snapshot = GHOST_ios_document_window_snapshot()) {
    snapshot.frame = view.bounds;
    snapshot.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    [view addSubview:snapshot];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, int64_t(1.5 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
                     [snapshot removeFromSuperview];
                   });
  }
  GHOST_ios_document_window_set_visible(false, false);
  /* The system's back action closes the document and shows the launcher. */
  [launcher_controller.navigationItem.backAction performWithSender:sender target:nil];
  /* Show where the document is, as Pages does, in case the browser is elsewhere. */
  if (url) {
    [launcher_controller.launchOptions.browserViewController revealDocumentAtURL:url
                                                                 importIfNeeded:NO
                                                                     completion:nil];
  }
  [NSUserDefaults.standardUserDefaults removeObjectForKey:launcher_open_document_key];
}

void GHOST_ios_launcher_set_modified(bool modified)
{
  if ([launcher_controller.document isKindOfClass:GHOSTIOSBlendDocument.class]) {
    [(GHOSTIOSBlendDocument *)launcher_controller.document setBlenderModified:modified];
  }
}

/** \} */
