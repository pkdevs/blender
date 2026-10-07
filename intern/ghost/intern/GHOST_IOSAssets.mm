/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#import "GHOST_IOSAssets.hh"
#import "GHOST_IOSIcons.hh"
#import <QuartzCore/QuartzCore.h>

extern "C" void WM_apple_ios_ui_setting_enqueue(const char *, const char *);
extern "C" void WM_apple_ios_ui_asset_drag(int phase, const char *asset_id, int x, int y);

/* Matches #IOSAssetDragPhase in `wm_apple_ios_ui.cc`. */
enum {
  GHOSTIOSAssetDragBegin = 0,
  GHOSTIOSAssetDragMove = 1,
  GHOSTIOSAssetDragDrop = 2,
  GHOSTIOSAssetDragCancel = 3,
  GHOSTIOSAssetAdd = 4,
};

static const CGFloat kAssetsHeight = 212.0;
static const CGFloat kAssetsCompactHeight = 184.0;
static const CGFloat kAssetsMaxWidth = 1100.0;
static const CGFloat kAssetTile = 96.0;

/* Data-block types Blender publishes, with their Blender icon and a readable plural. */
static NSArray<NSArray<NSString *> *> *blenderAssetTypes()
{
  return @[
    @[@"Object", @"OBJECT_DATA", @"Objects"],
    @[@"Collection", @"OUTLINER_COLLECTION", @"Collections"],
    @[@"Material", @"MATERIAL", @"Materials"],
    @[@"World", @"WORLD", @"Worlds"],
    @[@"Modifier", @"MODIFIER", @"Modifiers"],
  ];
}

static UIImage *blenderAssetTypeIcon(NSString *type)
{
  for (NSArray<NSString *> *entry in blenderAssetTypes()) {
    if ([entry[0] isEqual:type]) {
      return GHOSTIOSBlenderIcon(entry[1], NO);
    }
  }
  return [UIImage systemImageNamed:@"shippingbox"];
}

/* What tapping an asset does, matching where Blender's drop boxes accept it. */
static NSString *blenderAssetAddTitle(NSString *type)
{
  if ([type isEqual:@"Material"]) {
    return @"Apply to Active Object";
  }
  if ([type isEqual:@"Modifier"]) {
    return @"Add to Active Object";
  }
  if ([type isEqual:@"World"]) {
    return @"Use as World";
  }
  return @"Add at 3D Cursor";
}

@interface GHOSTIOSAssetCell : UICollectionViewCell
@property(nonatomic, readonly) UIImageView *thumbnail;
@property(nonatomic, readonly) UILabel *title;
@end

@implementation GHOSTIOSAssetCell {
  UIImageView *_thumbnail;
  UILabel *_title;
}

- (instancetype)initWithFrame:(CGRect)frame
{
  if ((self = [super initWithFrame:frame])) {
    _thumbnail = [[UIImageView alloc] init];
    _thumbnail.translatesAutoresizingMaskIntoConstraints = NO;
    _thumbnail.contentMode = UIViewContentModeScaleAspectFill;
    _thumbnail.clipsToBounds = YES;
    _thumbnail.layer.cornerRadius = 14;
    _thumbnail.layer.cornerCurve = kCACornerCurveContinuous;
    _thumbnail.backgroundColor = UIColor.tertiarySystemFillColor;
    _thumbnail.tintColor = UIColor.secondaryLabelColor;
    _title = [[UILabel alloc] init];
    _title.translatesAutoresizingMaskIntoConstraints = NO;
    _title.font = [UIFont preferredFontForTextStyle:UIFontTextStyleCaption1];
    _title.adjustsFontForContentSizeCategory = YES;
    _title.textAlignment = NSTextAlignmentCenter;
    _title.lineBreakMode = NSLineBreakByTruncatingMiddle;
    [self.contentView addSubview:_thumbnail];
    [self.contentView addSubview:_title];
    [NSLayoutConstraint activateConstraints:@[
      [_thumbnail.topAnchor constraintEqualToAnchor:self.contentView.topAnchor],
      [_thumbnail.centerXAnchor constraintEqualToAnchor:self.contentView.centerXAnchor],
      [_thumbnail.widthAnchor constraintEqualToConstant:kAssetTile],
      [_thumbnail.heightAnchor constraintEqualToConstant:kAssetTile],
      [_title.topAnchor constraintEqualToAnchor:_thumbnail.bottomAnchor constant:6],
      [_title.leadingAnchor constraintEqualToAnchor:self.contentView.leadingAnchor],
      [_title.trailingAnchor constraintEqualToAnchor:self.contentView.trailingAnchor],
    ]];
  }
  return self;
}

- (void)dealloc
{
  [_thumbnail release];
  [_title release];
  [super dealloc];
}

- (UIImageView *)thumbnail { return _thumbnail; }
- (UILabel *)title { return _title; }

- (void)setHighlighted:(BOOL)highlighted
{
  [super setHighlighted:highlighted];
  _thumbnail.alpha = highlighted ? 0.6 : 1.0;
}

@end

@interface GHOSTIOSAssets () <UICollectionViewDataSource,
                              UICollectionViewDelegate,
                              UICollectionViewDragDelegate,
                              UISearchTextFieldDelegate>
@end

@implementation GHOSTIOSAssets {
  UIView *_overlay;
  UIBarButtonItem *_button;
  UIVisualEffectView *_panel;
  UIButton *_libraryButton;
  UIButton *_filterButton;
  UISearchTextField *_search;
  UICollectionView *_grid;
  UILabel *_emptyLabel;
  UIDropInteraction *_dropInteraction;
  NSArray<NSDictionary *> *_items;
  NSArray<NSDictionary *> *_filtered;
  NSString *_library; /* nil: all libraries. */
  NSString *_type;    /* nil: all types. */
  NSString *_catalog; /* nil: all catalogs; otherwise a catalog path and its children. */
  NSCache<NSString *, UIImage *> *_thumbnails;
  BOOL _loading;
  BOOL _visible;
  BOOL _dragging;
  BOOL _dropped;
  CGPoint _dropPoint;
}

@synthesize clientPoint = _clientPoint;
@synthesize visibilityDidChange = _visibilityDidChange;

- (instancetype)initWithOverlay:(UIView *)overlay
{
  if ((self = [super init])) {
    _overlay = overlay;
    _items = [@[] retain];
    _filtered = [@[] retain];
    _thumbnails = [[NSCache alloc] init];

    UIImage *icon = GHOSTIOSBlenderIcon(@"ASSET_MANAGER", NO);
    _button = [[UIBarButtonItem alloc] initWithImage:icon ?: [UIImage systemImageNamed:@"square.grid.2x2"]
        style:UIBarButtonItemStylePlain target:self action:@selector(toggle:)];
    _button.title = _button.accessibilityLabel = @"Assets";
    _button.identifier = _button.accessibilityIdentifier = @"blender.assets";

    _panel = [[UIVisualEffectView alloc] initWithEffect:[UIGlassEffect effectWithStyle:UIGlassEffectStyleRegular]];
    _panel.accessibilityIdentifier = @"blender.assets.panel";
    _panel.layer.cornerRadius = 28;
    _panel.layer.cornerCurve = kCACornerCurveContinuous;
    _panel.clipsToBounds = YES;
    _panel.hidden = YES;
    _panel.alpha = 0;

    _libraryButton = [[self menuButton:@"All Libraries" identifier:@"blender.assets.library"] retain];
    _filterButton = [[self menuButton:@"All Types" identifier:@"blender.assets.filter"] retain];

    _search = [[UISearchTextField alloc] init];
    _search.placeholder = @"Search Assets";
    _search.accessibilityIdentifier = @"blender.assets.search";
    _search.returnKeyType = UIReturnKeySearch;
    _search.autocorrectionType = UITextAutocorrectionTypeNo;
    _search.delegate = self;
    __block GHOSTIOSAssets *owner = self;
    [_search addAction:[UIAction actionWithHandler:^(__kindof UIAction *) {
      [owner applyFilters];
    }] forControlEvents:UIControlEventEditingChanged];
    [_search setContentHuggingPriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];

    UIButton *close = [UIButton buttonWithType:UIButtonTypeClose];
    close.accessibilityLabel = @"Hide Assets";
    [close addTarget:self action:@selector(toggle:) forControlEvents:UIControlEventTouchUpInside];

    UIStackView *header = [[[UIStackView alloc]
        initWithArrangedSubviews:@[_libraryButton, _filterButton, _search, close]] autorelease];
    header.alignment = UIStackViewAlignmentCenter;
    header.spacing = 8;
    header.translatesAutoresizingMaskIntoConstraints = NO;

    UICollectionViewFlowLayout *layout = [[[UICollectionViewFlowLayout alloc] init] autorelease];
    layout.scrollDirection = UICollectionViewScrollDirectionHorizontal;
    layout.itemSize = CGSizeMake(kAssetTile + 8, kAssetTile + 30);
    layout.minimumLineSpacing = 10;
    layout.sectionInset = UIEdgeInsetsMake(0, 14, 0, 14);
    _grid = [[UICollectionView alloc] initWithFrame:CGRectZero collectionViewLayout:layout];
    _grid.translatesAutoresizingMaskIntoConstraints = NO;
    _grid.backgroundColor = UIColor.clearColor;
    _grid.showsHorizontalScrollIndicator = NO;
    _grid.accessibilityIdentifier = @"blender.assets.grid";
    _grid.dataSource = self;
    _grid.delegate = self;
    _grid.dragDelegate = self;
    _grid.dragInteractionEnabled = YES;
    _grid.keyboardDismissMode = UIScrollViewKeyboardDismissModeOnDrag;
    [_grid registerClass:GHOSTIOSAssetCell.class forCellWithReuseIdentifier:@"asset"];

    _emptyLabel = [[UILabel alloc] init];
    _emptyLabel.textAlignment = NSTextAlignmentCenter;
    _emptyLabel.textColor = UIColor.secondaryLabelColor;
    _emptyLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline];
    _emptyLabel.numberOfLines = 0;
    _grid.backgroundView = _emptyLabel;

    [_panel.contentView addSubview:header];
    [_panel.contentView addSubview:_grid];
    [NSLayoutConstraint activateConstraints:@[
      [header.leadingAnchor constraintEqualToAnchor:_panel.contentView.leadingAnchor constant:14],
      [header.trailingAnchor constraintEqualToAnchor:_panel.contentView.trailingAnchor constant:-12],
      [header.topAnchor constraintEqualToAnchor:_panel.contentView.topAnchor constant:10],
      [header.heightAnchor constraintEqualToConstant:40],
      [_grid.leadingAnchor constraintEqualToAnchor:_panel.contentView.leadingAnchor],
      [_grid.trailingAnchor constraintEqualToAnchor:_panel.contentView.trailingAnchor],
      [_grid.topAnchor constraintEqualToAnchor:header.bottomAnchor constant:10],
      [_grid.bottomAnchor constraintEqualToAnchor:_panel.contentView.bottomAnchor constant:-8],
    ]];

    _dropInteraction = [[UIDropInteraction alloc] initWithDelegate:self];
    [_overlay addSubview:_panel];
    [self updateMenus];
    [self updateSnapshot:@{@"loading": @YES, @"items": @[]}];
  }
  return self;
}

- (void)dealloc
{
  [_panel removeFromSuperview];
  [_panel release];
  [_button release];
  [_libraryButton release];
  [_filterButton release];
  [_search release];
  [_grid release];
  [_emptyLabel release];
  [_dropInteraction release];
  [_items release];
  [_filtered release];
  [_library release];
  [_type release];
  [_catalog release];
  [_thumbnails release];
  [_clientPoint release];
  [_visibilityDidChange release];
  [super dealloc];
}

- (UIBarButtonItem *)button { return _button; }
- (UIView *)view { return _panel; }
- (BOOL)visible { return _visible; }
- (UIDropInteraction *)dropInteraction { return _dropInteraction; }

- (CGFloat)height
{
  return _overlay.traitCollection.horizontalSizeClass == UIUserInterfaceSizeClassCompact ?
             kAssetsCompactHeight :
             kAssetsHeight;
}

- (UIButton *)menuButton:(NSString *)title identifier:(NSString *)identifier
{
  UIButtonConfiguration *configuration = [UIButtonConfiguration glassButtonConfiguration];
  configuration.title = title;
  configuration.indicator = UIButtonConfigurationIndicatorPopup;
  configuration.titleLineBreakMode = NSLineBreakByTruncatingTail;
  UIButton *button = [UIButton buttonWithConfiguration:configuration primaryAction:nil];
  button.showsMenuAsPrimaryAction = YES;
  button.accessibilityIdentifier = identifier;
  [button.widthAnchor constraintLessThanOrEqualToConstant:200].active = YES;
  [button setContentCompressionResistancePriority:UILayoutPriorityDefaultHigh
                                          forAxis:UILayoutConstraintAxisHorizontal];
  return button;
}

/* -------------------------------------------------------------------- */
/* Snapshot and filtering. */

- (void)updateSnapshot:(NSDictionary *)snapshot
{
  if (![snapshot isKindOfClass:NSDictionary.class]) {
    return;
  }
  NSArray *items = [snapshot[@"items"] isKindOfClass:NSArray.class] ? snapshot[@"items"] : @[];
  _loading = [snapshot[@"loading"] boolValue];
  if (![items isEqual:_items]) {
    /* Previews arrive over several snapshots; forget images whose path changed. */
    NSMutableDictionary *previous = [NSMutableDictionary dictionary];
    for (NSDictionary *item in _items) {
      previous[item[@"id"]] = item[@"preview"] ?: @"";
    }
    for (NSDictionary *item in items) {
      NSString *before = previous[item[@"id"]];
      if (before && ![before isEqual:item[@"preview"] ?: @""]) {
        [_thumbnails removeObjectForKey:item[@"id"]];
      }
    }
    [_items release];
    _items = [items copy];
    [self updateMenus];
  }
  [self applyFilters];
}

- (NSArray<NSString *> *)libraries
{
  NSMutableOrderedSet *names = [NSMutableOrderedSet orderedSet];
  for (NSDictionary *item in _items) {
    if ([item[@"library"] length]) {
      [names addObject:item[@"library"]];
    }
  }
  /* The bundled Starter library leads, as the place to begin. */
  if ([names containsObject:@"Starter"]) {
    [names removeObject:@"Starter"];
    [names insertObject:@"Starter" atIndex:0];
  }
  return names.array;
}

- (void)setLibrary:(NSString *)library
{
  [_library release];
  _library = [library copy];
  /* Catalogs belong to a library; keep only one that still exists there. */
  [_catalog release];
  _catalog = nil;
  [self updateMenus];
  [self applyFilters];
}

- (void)setType:(NSString *)type catalog:(NSString *)catalog
{
  [_type release];
  _type = [type copy];
  [_catalog release];
  _catalog = [catalog copy];
  [self updateMenus];
  [self applyFilters];
}

- (void)updateMenus
{
  __block GHOSTIOSAssets *owner = self;
  NSMutableArray *libraries = [NSMutableArray array];
  UIAction *all = [UIAction actionWithTitle:@"All Libraries" image:nil identifier:nil
                                    handler:^(__kindof UIAction *) { [owner setLibrary:nil]; }];
  all.state = _library ? UIMenuElementStateOff : UIMenuElementStateOn;
  [libraries addObject:all];
  NSMutableArray *named = [NSMutableArray array];
  for (NSString *library in [self libraries]) {
    UIAction *action = [UIAction actionWithTitle:library image:nil identifier:nil
                                         handler:^(__kindof UIAction *) { [owner setLibrary:library]; }];
    action.state = [_library isEqual:library] ? UIMenuElementStateOn : UIMenuElementStateOff;
    [named addObject:action];
  }
  _libraryButton.menu = [UIMenu menuWithTitle:@"" image:nil identifier:nil
                                      options:UIMenuOptionsSingleSelection
                                     children:@[[UIMenu menuWithTitle:@"" image:nil identifier:nil
                                                              options:UIMenuOptionsDisplayInline
                                                             children:libraries],
                                                [UIMenu menuWithTitle:@"" image:nil identifier:nil
                                                              options:UIMenuOptionsDisplayInline
                                                             children:named]]];
  UIButtonConfiguration *library = _libraryButton.configuration;
  library.title = _library ?: @"All Libraries";
  _libraryButton.configuration = library;

  /* Type and catalog share one filter menu, like the Asset Browser's sidebar. */
  NSMutableArray *types = [NSMutableArray array];
  UIAction *any = [UIAction actionWithTitle:@"All Types" image:nil identifier:nil
                                    handler:^(__kindof UIAction *) {
                                      [owner setType:nil catalog:owner->_catalog];
                                    }];
  any.state = _type ? UIMenuElementStateOff : UIMenuElementStateOn;
  [types addObject:any];
  NSMutableSet *present = [NSMutableSet set];
  NSMutableOrderedSet *catalogs = [NSMutableOrderedSet orderedSet];
  for (NSDictionary *item in _items) {
    if (_library && ![item[@"library"] isEqual:_library]) {
      continue;
    }
    [present addObject:item[@"type"] ?: @""];
    NSString *path = item[@"catalog"];
    /* Offer every parent, so a whole branch can be shown. */
    NSArray *parts = path.length ? [path componentsSeparatedByString:@"/"] : @[];
    for (NSUInteger i = 1; i <= parts.count; i++) {
      [catalogs addObject:[[parts subarrayWithRange:NSMakeRange(0, i)] componentsJoinedByString:@"/"]];
    }
  }
  for (NSArray<NSString *> *entry in blenderAssetTypes()) {
    if (![present containsObject:entry[0]]) {
      continue;
    }
    NSString *type = entry[0];
    UIAction *action = [UIAction actionWithTitle:entry[2] image:GHOSTIOSBlenderIcon(entry[1], NO)
                                      identifier:nil handler:^(__kindof UIAction *) {
                                        [owner setType:type catalog:owner->_catalog];
                                      }];
    action.state = [_type isEqual:type] ? UIMenuElementStateOn : UIMenuElementStateOff;
    [types addObject:action];
  }
  NSMutableArray *catalogItems = [NSMutableArray array];
  UIAction *anyCatalog = [UIAction actionWithTitle:@"All Catalogs" image:nil identifier:nil
                                           handler:^(__kindof UIAction *) {
                                             [owner setType:owner->_type catalog:nil];
                                           }];
  anyCatalog.state = _catalog ? UIMenuElementStateOff : UIMenuElementStateOn;
  [catalogItems addObject:anyCatalog];
  NSArray *sorted = [catalogs.array sortedArrayUsingSelector:@selector(localizedStandardCompare:)];
  for (NSString *path in sorted) {
    const NSUInteger depth = [path componentsSeparatedByString:@"/"].count - 1;
    NSString *title = [[@"" stringByPaddingToLength:depth * 3 withString:@" " startingAtIndex:0]
        stringByAppendingString:path.lastPathComponent];
    UIAction *action = [UIAction actionWithTitle:title image:nil identifier:nil
                                         handler:^(__kindof UIAction *) {
                                           [owner setType:owner->_type catalog:path];
                                         }];
    action.state = [_catalog isEqual:path] ? UIMenuElementStateOn : UIMenuElementStateOff;
    [catalogItems addObject:action];
  }
  UIMenu *catalogMenu = [UIMenu menuWithTitle:@"Catalog" image:[UIImage systemImageNamed:@"folder"]
                                   identifier:nil options:UIMenuOptionsSingleSelection
                                     children:catalogItems];
  _filterButton.menu = [UIMenu menuWithTitle:@"" image:nil identifier:nil options:0 children:@[
    [UIMenu menuWithTitle:@"" image:nil identifier:nil
                  options:UIMenuOptionsDisplayInline | UIMenuOptionsSingleSelection
                 children:types],
    catalogMenu,
  ]];
  NSString *typeTitle = @"All Types";
  for (NSArray<NSString *> *entry in blenderAssetTypes()) {
    if ([entry[0] isEqual:_type]) {
      typeTitle = entry[2];
    }
  }
  UIButtonConfiguration *filter = _filterButton.configuration;
  filter.title = _catalog ? _catalog.lastPathComponent : typeTitle;
  _filterButton.configuration = filter;
}

- (BOOL)item:(NSDictionary *)item matches:(NSArray<NSString *> *)terms
{
  if (_library && ![item[@"library"] isEqual:_library]) {
    return NO;
  }
  if (_type && ![item[@"type"] isEqual:_type]) {
    return NO;
  }
  if (_catalog) {
    NSString *path = item[@"catalog"];
    if (![path isEqual:_catalog] && ![path hasPrefix:[_catalog stringByAppendingString:@"/"]]) {
      return NO;
    }
  }
  if (terms.count == 0) {
    return YES;
  }
  /* Every word must match the name, tags, catalog, description or author, like the
   * Asset Browser's filter. */
  NSMutableArray *fields = [NSMutableArray arrayWithObjects:item[@"name"] ?: @"", item[@"catalog"] ?: @"",
                                                            item[@"description"] ?: @"",
                                                            item[@"author"] ?: @"", item[@"type"] ?: @"", nil];
  if ([item[@"tags"] isKindOfClass:NSArray.class]) {
    [fields addObjectsFromArray:item[@"tags"]];
  }
  NSString *haystack = [fields componentsJoinedByString:@"\n"];
  for (NSString *term in terms) {
    if ([haystack rangeOfString:term
                        options:NSCaseInsensitiveSearch | NSDiacriticInsensitiveSearch]
            .location == NSNotFound)
    {
      return NO;
    }
  }
  return YES;
}

- (void)applyFilters
{
  NSMutableArray *terms = [NSMutableArray array];
  for (NSString *term in [_search.text componentsSeparatedByCharactersInSet:
                                           NSCharacterSet.whitespaceAndNewlineCharacterSet])
  {
    if (term.length) {
      [terms addObject:term];
    }
  }
  NSMutableArray *filtered = [NSMutableArray array];
  for (NSDictionary *item in _items) {
    if ([self item:item matches:terms]) {
      [filtered addObject:item];
    }
  }
  if (_loading && _items.count == 0) {
    _emptyLabel.text = @"Loading Assets…";
  }
  else if (filtered.count == 0) {
    _emptyLabel.text = terms.count ? [NSString stringWithFormat:@"No Results for “%@”", _search.text] :
                                     @"No Assets";
  }
  _emptyLabel.hidden = filtered.count != 0;
  if ([filtered isEqual:_filtered]) {
    return;
  }
  /* Same assets in the same order: only update the cells, keeping the scroll position. */
  BOOL same_order = filtered.count == _filtered.count;
  for (NSUInteger i = 0; same_order && i < filtered.count; i++) {
    same_order = [filtered[i][@"id"] isEqual:_filtered[i][@"id"]];
  }
  [_filtered release];
  _filtered = [filtered copy];
  if (same_order) {
    [_grid reconfigureItemsAtIndexPaths:_grid.indexPathsForVisibleItems];
  }
  else {
    [_grid reloadData];
  }
}

- (UIImage *)thumbnailForItem:(NSDictionary *)item
{
  NSString *path = item[@"preview"];
  if (![path isKindOfClass:NSString.class] || path.length == 0) {
    return nil;
  }
  UIImage *image = [_thumbnails objectForKey:item[@"id"]];
  if (!image) {
    image = [UIImage imageWithContentsOfFile:path];
    if (image) {
      [_thumbnails setObject:image forKey:item[@"id"]];
    }
  }
  return image;
}

/* -------------------------------------------------------------------- */
/* Collection view. */

- (NSInteger)collectionView:(UICollectionView *)collectionView numberOfItemsInSection:(NSInteger)section
{
  return _filtered.count;
}

- (UICollectionViewCell *)collectionView:(UICollectionView *)collectionView
                  cellForItemAtIndexPath:(NSIndexPath *)indexPath
{
  GHOSTIOSAssetCell *cell = [collectionView dequeueReusableCellWithReuseIdentifier:@"asset"
                                                                      forIndexPath:indexPath];
  NSDictionary *item = _filtered[indexPath.item];
  UIImage *thumbnail = [self thumbnailForItem:item];
  cell.thumbnail.image = thumbnail ?: blenderAssetTypeIcon(item[@"type"]);
  cell.thumbnail.contentMode = thumbnail ? UIViewContentModeScaleAspectFill : UIViewContentModeCenter;
  cell.title.text = item[@"name"];
  cell.accessibilityLabel = item[@"name"];
  cell.accessibilityHint = blenderAssetAddTitle(item[@"type"]);
  cell.accessibilityIdentifier = [@"blender.asset." stringByAppendingString:item[@"name"] ?: @""];
  cell.isAccessibilityElement = YES;
  return cell;
}

- (void)collectionView:(UICollectionView *)collectionView didSelectItemAtIndexPath:(NSIndexPath *)indexPath
{
  [collectionView deselectItemAtIndexPath:indexPath animated:YES];
  [self addItem:_filtered[indexPath.item]];
}

- (void)addItem:(NSDictionary *)item
{
  [_search resignFirstResponder];
  WM_apple_ios_ui_asset_drag(GHOSTIOSAssetAdd, [item[@"id"] UTF8String], 0, 0);
  [[[[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleLight] autorelease]
      impactOccurred];
}

- (UIContextMenuConfiguration *)collectionView:(UICollectionView *)collectionView
    contextMenuConfigurationForItemsAtIndexPaths:(NSArray<NSIndexPath *> *)indexPaths
                                           point:(CGPoint)point
{
  if (indexPaths.count != 1) {
    return nil;
  }
  NSDictionary *item = _filtered[indexPaths.firstObject.item];
  __block GHOSTIOSAssets *owner = self;
  return [UIContextMenuConfiguration configurationWithIdentifier:nil previewProvider:nil
      actionProvider:^UIMenu *(NSArray<UIMenuElement *> *) {
        NSMutableArray *children = [NSMutableArray array];
        [children addObject:[UIAction actionWithTitle:blenderAssetAddTitle(item[@"type"])
                                                image:[UIImage systemImageNamed:@"plus"]
                                           identifier:nil
                                              handler:^(__kindof UIAction *) { [owner addItem:item]; }]];
        if ([item[@"catalog"] length]) {
          NSString *catalog = item[@"catalog"];
          [children addObject:[UIAction actionWithTitle:@"Show Catalog"
                                                  image:[UIImage systemImageNamed:@"folder"]
                                             identifier:nil handler:^(__kindof UIAction *) {
                                               [owner setType:nil catalog:catalog];
                                             }]];
        }
        NSString *subtitle = [item[@"catalog"] length] ?
                                 [NSString stringWithFormat:@"%@ · %@", item[@"library"], item[@"catalog"]] :
                                 item[@"library"];
        UIMenu *menu = [UIMenu menuWithTitle:subtitle ?: @"" children:children];
        if ([item[@"description"] length]) {
          menu = [UIMenu menuWithTitle:item[@"description"] children:@[menu]];
        }
        return menu;
      }];
}

/* -------------------------------------------------------------------- */
/* Dragging out of the pane. */

- (NSArray<UIDragItem *> *)collectionView:(UICollectionView *)collectionView
             itemsForBeginningDragSession:(id<UIDragSession>)session
                              atIndexPath:(NSIndexPath *)indexPath
{
  NSDictionary *item = _filtered[indexPath.item];
  NSItemProvider *provider = [[[NSItemProvider alloc] initWithObject:item[@"name"] ?: @""] autorelease];
  UIDragItem *drag = [[[UIDragItem alloc] initWithItemProvider:provider] autorelease];
  drag.localObject = item;
  [_search resignFirstResponder];
  return @[drag];
}

- (UIDragPreviewParameters *)collectionView:(UICollectionView *)collectionView
    dragPreviewParametersForItemAtIndexPath:(NSIndexPath *)indexPath
{
  GHOSTIOSAssetCell *cell = (GHOSTIOSAssetCell *)[collectionView cellForItemAtIndexPath:indexPath];
  UIDragPreviewParameters *parameters = [[[UIDragPreviewParameters alloc] init] autorelease];
  if (cell) {
    parameters.visiblePath = [UIBezierPath bezierPathWithRoundedRect:cell.thumbnail.frame cornerRadius:14];
  }
  parameters.backgroundColor = UIColor.clearColor;
  return parameters;
}

- (BOOL)collectionView:(UICollectionView *)collectionView
    dragSessionIsRestrictedToDraggingApplication:(id<UIDragSession>)session
{
  return YES;
}

/* -------------------------------------------------------------------- */
/* Dropping into the viewport. */

- (NSDictionary *)assetForSession:(id<UIDropSession>)session
{
  if (!session.localDragSession || session.items.count != 1) {
    return nil;
  }
  id object = session.items.firstObject.localObject;
  return [object isKindOfClass:NSDictionary.class] && [object[@"id"] length] ? object : nil;
}

/* Whether the drag is over the viewport rather than this pane or other native controls. */
- (BOOL)isOverViewport:(id<UIDropSession>)session
{
  UIView *view = _dropInteraction.view;
  const CGPoint point = [session locationInView:view];
  UIView *hit = [view hitTest:point withEvent:nil];
  for (UIView *ancestor = hit; ancestor; ancestor = ancestor.superview) {
    if (ancestor == _panel || [ancestor isKindOfClass:UIControl.class]) {
      return NO;
    }
  }
  return YES;
}

- (void)sendDrag:(int)phase session:(id<UIDropSession>)session
{
  const CGPoint point = [session locationInView:_dropInteraction.view];
  const CGPoint client = _clientPoint ? _clientPoint(point) : point;
  NSDictionary *item = [self assetForSession:session];
  WM_apple_ios_ui_asset_drag(phase, [item[@"id"] UTF8String], int(lround(client.x)),
                             int(lround(client.y)));
}

- (BOOL)dropInteraction:(UIDropInteraction *)interaction canHandleSession:(id<UIDropSession>)session
{
  return [self assetForSession:session] != nil;
}

- (void)dropInteraction:(UIDropInteraction *)interaction sessionDidEnter:(id<UIDropSession>)session
{
  _dropped = NO;
}

- (UIDropProposal *)dropInteraction:(UIDropInteraction *)interaction
                   sessionDidUpdate:(id<UIDropSession>)session
{
  const BOOL over = [self isOverViewport:session];
  if (over && !_dragging) {
    /* Start Blender's drag where the asset enters the viewport, so it previews from there. */
    _dragging = YES;
    [self sendDrag:GHOSTIOSAssetDragBegin session:session];
  }
  else if (over) {
    [self sendDrag:GHOSTIOSAssetDragMove session:session];
  }
  else if (_dragging) {
    /* Back over the pane: stop previewing in the viewport. */
    _dragging = NO;
    WM_apple_ios_ui_asset_drag(GHOSTIOSAssetDragCancel, nullptr, 0, 0);
  }
  return [[[UIDropProposal alloc] initWithDropOperation:over ? UIDropOperationCopy :
                                                               UIDropOperationCancel] autorelease];
}

- (void)dropInteraction:(UIDropInteraction *)interaction performDrop:(id<UIDropSession>)session
{
  if (!_dragging) {
    return;
  }
  _dropped = YES;
  _dragging = NO;
  _dropPoint = [session locationInView:interaction.view];
  [self sendDrag:GHOSTIOSAssetDragDrop session:session];
}

- (void)dropInteraction:(UIDropInteraction *)interaction sessionDidExit:(id<UIDropSession>)session
{
  if (_dragging) {
    _dragging = NO;
    WM_apple_ios_ui_asset_drag(GHOSTIOSAssetDragCancel, nullptr, 0, 0);
  }
}

- (void)dropInteraction:(UIDropInteraction *)interaction sessionDidEnd:(id<UIDropSession>)session
{
  if (_dragging && !_dropped) {
    WM_apple_ios_ui_asset_drag(GHOSTIOSAssetDragCancel, nullptr, 0, 0);
  }
  _dragging = NO;
}

- (UITargetedDragPreview *)dropInteraction:(UIDropInteraction *)interaction
                      previewForDroppingItem:(UIDragItem *)item
                                 withDefault:(UITargetedDragPreview *)defaultPreview
{
  /* The asset lands in the scene: shrink the thumbnail into the drop point. */
  UIDragPreviewTarget *target = [[[UIDragPreviewTarget alloc]
      initWithContainer:interaction.view center:_dropPoint transform:CGAffineTransformMakeScale(0.05, 0.05)] autorelease];
  return [defaultPreview retargetedPreviewWithTarget:target];
}

/* -------------------------------------------------------------------- */
/* Search field, visibility and layout. */

- (BOOL)textFieldShouldReturn:(UITextField *)textField
{
  [textField resignFirstResponder];
  return YES;
}

- (void)toggle:(id)sender
{
  _visible = !_visible;
  _button.selected = _visible;
  WM_apple_ios_ui_setting_enqueue("assets", _visible ? "show" : "hide");
  if (!_visible) {
    [_search resignFirstResponder];
  }
  const BOOL reduceMotion = UIAccessibilityIsReduceMotionEnabled();
  CGAffineTransform hiddenTransform = reduceMotion ? CGAffineTransformIdentity :
      CGAffineTransformConcat(CGAffineTransformMakeTranslation(0, 18), CGAffineTransformMakeScale(0.98, 0.98));
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
  [UIView animateWithDuration:reduceMotion ? 0 : 0.3
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
  const CGFloat width = MIN(kAssetsMaxWidth, MAX(0, rect.size.width));
  const CGFloat height = self.height;
  /* Frame is undefined while transformed; layout must not disturb the presentation animation. */
  _panel.bounds = CGRectMake(0, 0, width, height);
  _panel.center = CGPointMake(CGRectGetMidX(rect), CGRectGetMaxY(rect) - height / 2);
}

@end
