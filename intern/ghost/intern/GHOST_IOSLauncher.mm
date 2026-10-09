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

/**
 * Blender's application templates that suit the iPad, General first, as File > New lists them.
 * Only General for now: the others bring Blender's desktop interface along.
 */
static const bool launcher_desktop_templates = false;

static NSArray<NSDictionary *> *launcherTemplates()
{
  if (!launcher_desktop_templates) {
    return @[@{@"name": @"", @"title": @"General"}];
  }
  NSMutableArray *templates = [NSMutableArray arrayWithObject:@{@"name": @"", @"title": @"General"}];
  NSString *assets = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"Assets"];
  NSFileManager *files = NSFileManager.defaultManager;
  for (NSString *version in [files contentsOfDirectoryAtPath:assets error:nil]) {
    NSString *folder = [assets stringByAppendingPathComponent:
                                   [version stringByAppendingPathComponent:
                                                @"scripts/startup/bl_app_templates_system"]];
    NSArray *names = [[files contentsOfDirectoryAtPath:folder error:nil]
        sortedArrayUsingSelector:@selector(localizedStandardCompare:)];
    for (NSString *name in names) {
      NSString *startup = [folder stringByAppendingPathComponent:
                                      [name stringByAppendingPathComponent:@"startup.blend"]];
      if ([files fileExistsAtPath:startup]) {
        /* As Blender displays them, "Video_Editing" as "Video Editing". */
        [templates addObject:@{@"name": name,
                               @"title": [name stringByReplacingOccurrencesOfString:@"_" withString:@" "]}];
      }
    }
  }
  return templates;
}

static NSString *launcherTemplateSymbol(NSString *name)
{
  NSDictionary *symbols = @{
    @"": @"cube",
    @"2D_Animation": @"pencil.and.scribble",
    @"Sculpting": @"hand.draw",
    @"Storyboarding": @"rectangle.split.3x1",
    @"VFX": @"sparkles",
    @"Video_Editing": @"film",
  };
  return symbols[name] ?: @"square.dashed";
}

/** A template as a card: its symbol above its name. */
@interface GHOSTIOSTemplateCell : UICollectionViewCell
@property(nonatomic, readonly) UIImageView *symbol;
@property(nonatomic, readonly) UILabel *name;
@end

@implementation GHOSTIOSTemplateCell

- (instancetype)initWithFrame:(CGRect)frame
{
  if ((self = [super initWithFrame:frame])) {
    _symbol = [[UIImageView alloc] init];
    _symbol.contentMode = UIViewContentModeScaleAspectFit;
    _symbol.tintColor = launcherColor(0xE87D0D, 1);
    _symbol.preferredSymbolConfiguration = [UIImageSymbolConfiguration
        configurationWithPointSize:40 weight:UIImageSymbolWeightRegular];
    _name = [[UILabel alloc] init];
    _name.font = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
    _name.adjustsFontForContentSizeCategory = YES;
    _name.textAlignment = NSTextAlignmentCenter;
    UIStackView *stack = [[[UIStackView alloc] initWithArrangedSubviews:@[_symbol, _name]] autorelease];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 12;
    stack.alignment = UIStackViewAlignmentCenter;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [self.contentView addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[
      [stack.centerXAnchor constraintEqualToAnchor:self.contentView.centerXAnchor],
      [stack.centerYAnchor constraintEqualToAnchor:self.contentView.centerYAnchor],
      [stack.widthAnchor constraintLessThanOrEqualToAnchor:self.contentView.widthAnchor constant:-16],
    ]];
    self.isAccessibilityElement = YES;
    self.accessibilityTraits = UIAccessibilityTraitButton;
  }
  return self;
}

- (void)dealloc
{
  [_symbol release];
  [_name release];
  [super dealloc];
}

- (void)updateConfigurationUsingState:(UICellConfigurationState *)state
{
  UIBackgroundConfiguration *background = [UIBackgroundConfiguration listCellConfiguration];
  background.cornerRadius = 20;
  background.backgroundColor = state.isHighlighted || state.isSelected ?
                                   UIColor.tertiarySystemFillColor :
                                   UIColor.secondarySystemBackgroundColor;
  self.backgroundConfiguration = background;
}

@end

/** A grid of Blender's templates, to start a new document from one. */
@interface GHOSTIOSTemplatePicker : UICollectionViewController
@property(nonatomic, copy) void (^completion)(NSString *app_template);
@end

@implementation GHOSTIOSTemplatePicker {
  NSArray<NSDictionary *> *_templates;
  BOOL _finished;
}

- (instancetype)init
{
  UICollectionViewCompositionalLayout *layout = [[[UICollectionViewCompositionalLayout alloc]
      initWithSectionProvider:^NSCollectionLayoutSection *(NSInteger,
                                                            id<NSCollectionLayoutEnvironment> environment) {
        const CGFloat width = environment.container.effectiveContentSize.width;
        const NSInteger columns = width > 500 ? 3 : 2;
        NSCollectionLayoutSize *item_size = [NSCollectionLayoutSize
            sizeWithWidthDimension:[NSCollectionLayoutDimension fractionalWidthDimension:1.0 / columns]
                   heightDimension:[NSCollectionLayoutDimension fractionalHeightDimension:1.0]];
        NSCollectionLayoutItem *item = [NSCollectionLayoutItem itemWithLayoutSize:item_size];
        item.contentInsets = NSDirectionalEdgeInsetsMake(8, 8, 8, 8);
        NSCollectionLayoutSize *group_size = [NSCollectionLayoutSize
            sizeWithWidthDimension:[NSCollectionLayoutDimension fractionalWidthDimension:1.0]
                   heightDimension:[NSCollectionLayoutDimension absoluteDimension:150]];
        NSCollectionLayoutGroup *group = [NSCollectionLayoutGroup
            horizontalGroupWithLayoutSize:group_size
                                 repeatingSubitem:item
                                            count:columns];
        NSCollectionLayoutSection *section = [NSCollectionLayoutSection sectionWithGroup:group];
        section.contentInsets = NSDirectionalEdgeInsetsMake(12, 12, 12, 12);
        return section;
      }] autorelease];
  if ((self = [super initWithCollectionViewLayout:layout])) {
    _templates = [launcherTemplates() retain];
    self.title = @"Choose a Template";
  }
  return self;
}

- (void)dealloc
{
  [_templates release];
  [_completion release];
  [super dealloc];
}

- (void)viewDidLoad
{
  [super viewDidLoad];
  [self.collectionView registerClass:GHOSTIOSTemplateCell.class forCellWithReuseIdentifier:@"template"];
  __block GHOSTIOSTemplatePicker *picker = self;
  self.navigationItem.leftBarButtonItem = [[[UIBarButtonItem alloc]
      initWithBarButtonSystemItem:UIBarButtonSystemItemCancel
                    primaryAction:[UIAction actionWithHandler:^(__kindof UIAction *) {
                      [picker finish:nil];
                    }]] autorelease];
}

- (NSInteger)collectionView:(UICollectionView *)view numberOfItemsInSection:(NSInteger)section
{
  return _templates.count;
}

- (UICollectionViewCell *)collectionView:(UICollectionView *)view
                  cellForItemAtIndexPath:(NSIndexPath *)index
{
  GHOSTIOSTemplateCell *cell = [view dequeueReusableCellWithReuseIdentifier:@"template"
                                                                forIndexPath:index];
  NSDictionary *item = _templates[index.item];
  cell.symbol.image = [UIImage systemImageNamed:launcherTemplateSymbol(item[@"name"])];
  cell.name.text = item[@"title"];
  cell.accessibilityLabel = item[@"title"];
  cell.accessibilityIdentifier = [@"blender.template." stringByAppendingString:item[@"name"]];
  return cell;
}

- (void)collectionView:(UICollectionView *)view didSelectItemAtIndexPath:(NSIndexPath *)index
{
  [self finish:_templates[index.item][@"name"]];
}

- (void)finish:(NSString *)app_template
{
  if (_finished) {
    return;
  }
  _finished = YES;
  void (^completion)(NSString *) = [[_completion retain] autorelease];
  [self.presentingViewController dismissViewControllerAnimated:YES completion:nil];
  if (completion) {
    completion(app_template);
  }
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

/**
 * The document launcher, in the app's first window, shown at once on launch while Blender starts
 * behind it. It owns the open document; Blender's window shows only while one is open.
 */
@interface GHOSTIOSLauncherController : UIDocumentViewController <UIDocumentBrowserViewControllerDelegate>
- (void)openDocumentAtURL:(NSURL *)url;
- (void)chooseTemplate:(void (^)(NSString *app_template))completion;
@end

static GHOSTIOSLauncherController *launcher_controller = nil;
/** A file the system asked to open before the launcher existed. */
static NSURL *launcher_pending_url = nil;

@implementation GHOSTIOSLauncherController {
  /** A document is being opened: Blender loads one file at a time. */
  BOOL _opening;
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
      createDocumentActionWithIntent:UIDocumentCreationIntentDefault];
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
  [super dealloc];
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
  if (!previous_open) {
    self.document = document;
    return;
  }
  /* One file at a time: the current one saves as it closes. */
  GHOST_ios_document_window_set_visible(false, true);
  [previous closeWithCompletionHandler:^(BOOL) {
    self.document = document;
  }];
}

/** Present Blender's templates; `completion` gets the chosen one, or nil. */
- (void)chooseTemplate:(void (^)(NSString *app_template))completion
{
  GHOSTIOSTemplatePicker *picker = [[[GHOSTIOSTemplatePicker alloc] init] autorelease];
  picker.completion = completion;
  UINavigationController *sheet = [[[UINavigationController alloc] initWithRootViewController:picker]
      autorelease];
  sheet.modalPresentationStyle = UIModalPresentationFormSheet;
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
    GHOST_ios_document_create(app_template, ^(NSURL *url) {
      importHandler(url, url ? UIDocumentBrowserImportModeMove : UIDocumentBrowserImportModeNone);
    });
  };
  if (![browser.activeDocumentCreationIntent isEqualToString:launcher_template_intent]) {
    create(@"");
    return;
  }
  [self chooseTemplate:create];
}

- (void)documentBrowser:(UIDocumentBrowserViewController *)browser
    didImportDocumentAtURL:(NSURL *)sourceURL
          toDestinationURL:(NSURL *)destinationURL
{
  [self openDocumentAtURL:destinationURL];
}

- (void)documentBrowser:(UIDocumentBrowserViewController *)browser
    didPickDocumentsAtURLs:(NSArray<NSURL *> *)documentURLs
{
  if (documentURLs.firstObject) {
    [self openDocumentAtURL:documentURLs.firstObject];
  }
}

@end

@interface GHOSTIOSTemplatePicker (Dismissal) <UIAdaptivePresentationControllerDelegate>
@end

@implementation GHOSTIOSTemplatePicker (Dismissal)
- (void)presentationControllerDidDismiss:(UIPresentationController *)controller
{
  [self finish:nil];
}
@end

UIViewController *GHOST_ios_launcher_controller()
{
  if (launcher_controller == nil) {
    launcher_controller = [[GHOSTIOSLauncherController alloc] initWithDocument:nil];
  }
  return launcher_controller;
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
