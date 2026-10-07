/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#import "GHOST_IOSPencilMenu.hh"

#include <cmath>
#include <vector>

/* Pills sit on up to two concentric elliptical rings around the Pencil. */
static const CGFloat pill_height = 30.0;
static const CGFloat pill_gap = 6.0;
static const NSUInteger inner_slots_max = 12;
static const NSUInteger outer_slots = 16;

struct PencilRing {
  NSUInteger slots = 0;
  CGFloat radius_x = 0, radius_y = 0, phase = 0;
};

static CGPoint ring_point(const PencilRing &ring, NSUInteger slot, CGPoint center)
{
  const double angle = -M_PI_2 + ring.phase + slot * 2.0 * M_PI / ring.slots;
  return CGPointMake(center.x + cos(angle) * ring.radius_x, center.y + sin(angle) * ring.radius_y);
}

static CGRect pill_rect(CGPoint center, CGSize pill)
{
  return CGRectMake(center.x - pill.width / 2, center.y - pill.height / 2, pill.width, pill.height);
}

/* Smallest ring of `slots` pills with the given aspect that overlaps neither itself nor
 * `obstacles` (relative to the menu center), starting the search at `start`. */
static PencilRing solve_ring(NSUInteger slots, CGFloat aspect, CGFloat phase, CGSize pill,
                             const std::vector<CGRect> &obstacles, CGFloat start)
{
  PencilRing ring;
  ring.slots = slots;
  ring.phase = phase;
  for (CGFloat scale = start; scale < 2000.0; scale += 2.0) {
    ring.radius_x = scale * aspect;
    ring.radius_y = scale;
    std::vector<CGRect> rects;
    bool clear = true;
    for (NSUInteger i = 0; i < slots && clear; i++) {
      const CGRect rect = CGRectInset(pill_rect(ring_point(ring, i, CGPointZero), pill),
                                      -pill_gap / 2, -pill_gap / 2);
      for (const CGRect &other : rects) {
        clear = clear && !CGRectIntersectsRect(rect, other);
      }
      for (const CGRect &other : obstacles) {
        clear = clear && !CGRectIntersectsRect(rect, other);
      }
      rects.push_back(rect);
    }
    if (clear) {
      return ring;
    }
  }
  return ring;
}

static CGSize ring_extent(const PencilRing &ring, CGSize pill)
{
  return CGSizeMake(2 * ring.radius_x + pill.width, 2 * ring.radius_y + pill.height);
}

@implementation GHOSTIOSPencilMenu {
  NSArray<NSDictionary *> *_items;
  NSMutableArray<UIButton *> *_buttons;
  NSMutableArray<NSDictionary *> *_history;
  NSString *_title;
  UIVisualEffectView *_centerView;
  UILabel *_centerLabel;
  UISelectionFeedbackGenerator *_feedback;
  UITouch *_capturedTouch;
  CGPoint _lastPencilPoint;
  CGPoint _anchor;
  CGPoint _center;
  PencilRing _inner, _outer;
  NSUInteger _perPage;
  NSMutableArray<NSNumber *> *_slots;
  CGSize _layoutSize;
  NSInteger _selected;
  NSUInteger _page;
  NSUInteger _requestGeneration;
  BOOL _loading;
  BOOL _active;
  BOOL _menuVisible;
  BOOL _hasPencilLocation;
  BOOL _openedOnTouch;
}

@synthesize delegate;

- (instancetype)initWithFrame:(CGRect)frame
{
  self = [super initWithFrame:frame];
  if (self) {
    self.backgroundColor = UIColor.clearColor;
    self.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    _selected = NSNotFound;
    _buttons = [[NSMutableArray alloc] init];
    _slots = [[NSMutableArray alloc] init];
    _perPage = inner_slots_max;
    _history = [[NSMutableArray alloc] init];
    _feedback = [[UISelectionFeedbackGenerator alloc] init];
    _centerView = [[UIVisualEffectView alloc] initWithEffect:
        [UIGlassEffect effectWithStyle:UIGlassEffectStyleRegular]];
    _centerView.cornerConfiguration = [UICornerConfiguration capsuleConfiguration];
    _centerView.userInteractionEnabled = NO;
    _centerView.hidden = YES;
    [self addSubview:_centerView];
    _centerLabel = [[UILabel alloc] init];
    _centerLabel.textAlignment = NSTextAlignmentCenter;
    _centerLabel.font = [UIFont systemFontOfSize:12 weight:UIFontWeightSemibold];
    _centerLabel.textColor = UIColor.labelColor;
    _centerLabel.numberOfLines = 2;
    _centerLabel.adjustsFontSizeToFitWidth = YES;
    _centerLabel.minimumScaleFactor = 0.85;
    [_centerView.contentView addSubview:_centerLabel];
  }
  return self;
}

- (void)dealloc
{
  [_items release];
  [_buttons release];
  [_slots release];
  [_history release];
  [_title release];
  [_centerView release];
  [_centerLabel release];
  [_feedback release];
  [super dealloc];
}

- (BOOL)active { return _active; }
- (BOOL)menuVisible { return _menuVisible; }

- (UIView *)hitTest:(CGPoint)point withEvent:(UIEvent *)event
{
  UIView *hit = [super hitTest:point withEvent:event];
  /* An open menu shields the underlying controls until selection or dismissal. */
  return hit == self && !_active ? nil : hit;
}

- (void)updateCenterLabel
{
  if (_loading || _items.count == 0) {
    _centerLabel.text = _loading ? @"Loading..." : @"No Context Actions";
    return;
  }
  if (_selected != NSNotFound) {
    _centerLabel.text = _buttons[_selected].accessibilityLabel;
    return;
  }
  /* The menu's name; a menu with several pages adds which page is showing. */
  NSString *title = _title.length ? _title : @"Actions";
  _centerLabel.text = [self pageCount] > 1 ?
      [NSString stringWithFormat:@"%@\n%lu / %lu", title, (unsigned long)_page + 1,
          (unsigned long)[self pageCount]] :
      title;
}

- (NSUInteger)pageCount
{
  return MAX((NSUInteger)1, (_items.count + _perPage - 1) / _perPage);
}

- (void)selectIndex:(NSInteger)index
{
  if (index != NSNotFound && !_buttons[index].enabled) {
    index = NSNotFound;
  }
  if (_selected == index) {
    return;
  }
  if (_selected != NSNotFound) {
    _buttons[_selected].selected = NO;
  }
  _selected = index;
  if (_selected != NSNotFound) {
    _buttons[_selected].selected = YES;
    [_feedback selectionChanged];
  }
  [self updateCenterLabel];
}

- (void)layoutSubviews
{
  [super layoutSubviews];
  if (_menuVisible && !CGSizeEqualToSize(_layoutSize, self.bounds.size)) {
    /* Rotation or Stage Manager resizing invalidates the Pencil anchor. */
    [self cancel];
  }
}

- (void)buildPageAnimated:(BOOL)animated
{
  [self selectIndex:NSNotFound];
  for (UIButton *button in _buttons) {
    [button removeFromSuperview];
  }
  [_buttons removeAllObjects];

  [_slots removeAllObjects];
  CGRect available = UIEdgeInsetsInsetRect(self.bounds, self.safeAreaInsets);
  available = CGRectInset(available, 12.0, 12.0);

  /* One pill width per menu, fitted to the longest title, keeps the rings regular. */
  UIFont *font = [UIFont systemFontOfSize:13 weight:UIFontWeightMedium];
  CGFloat title_width = 0;
  for (NSDictionary *item in _items) {
    title_width = MAX(title_width, ceil([item[@"title"] sizeWithAttributes:@{NSFontAttributeName: font}].width));
  }
  const CGSize pill = CGSizeMake(MIN(132.0, MAX(88.0, title_width + 54.0)), pill_height);
  const CGRect center_rect = CGRectMake(-53 - pill_gap, -22 - pill_gap, 106 + 2 * pill_gap, 44 + 2 * pill_gap);
  auto fits = [&](CGSize extent) {
    return extent.width <= available.size.width && extent.height <= available.size.height;
  };

  /* The widest ring that fits: 12 clock positions, fewer in small windows, and a taller ring
   * when the window is narrow. */
  PencilRing inner;
  CGFloat aspect = 1.8;
  bool found = false;
  for (NSUInteger slots : {inner_slots_max, (NSUInteger)10, (NSUInteger)8, (NSUInteger)6}) {
    for (CGFloat candidate : {1.8, 1.4, 1.0, 0.7, 0.5, 0.35}) {
      inner = solve_ring(slots, candidate, 0, pill, {center_rect}, 40.0);
      if (fits(ring_extent(inner, pill))) {
        aspect = candidate;
        found = true;
        break;
      }
    }
    if (found) {
      break;
    }
  }
  const NSUInteger inner_capacity = inner.slots;
  std::vector<CGRect> inner_rects{center_rect};
  for (NSUInteger i = 0; i < inner.slots; i++) {
    inner_rects.push_back(CGRectInset(pill_rect(ring_point(inner, i, CGPointZero), pill), -pill_gap / 2, -pill_gap / 2));
  }
  /* A second ring sits between the inner pills, half a step around. */
  PencilRing outer = solve_ring(outer_slots, aspect, M_PI / outer_slots, pill, inner_rects, inner.radius_y + pill_height);
  const NSUInteger capacity = inner_capacity + (fits(ring_extent(outer, pill)) ? outer_slots : 0);
  const BOOL paged = _items.count > capacity;
  _perPage = paged ? capacity - 1 : capacity;

  const NSUInteger start = _page * _perPage;
  const NSUInteger count = MIN(_perPage, _items.count - MIN(start, _items.count));
  const NSUInteger shown = count + (paged ? 1 : 0);
  if (shown <= inner_capacity) {
    /* Small menus stay compact: no more inner slots than needed, but at least eight. */
    const NSUInteger slots = MIN(inner_capacity, MAX(shown, (NSUInteger)8));
    if (slots != inner.slots) {
      PencilRing compact = solve_ring(slots, aspect, 0, pill, {center_rect}, 40.0);
      if (fits(ring_extent(compact, pill))) {
        inner = compact;
      }
    }
  }
  _inner = inner;
  _outer = shown > inner.slots ? outer : PencilRing();
  const CGSize extent = _outer.slots ? ring_extent(_outer, pill) : ring_extent(_inner, pill);
  _center = CGPointMake(
      MAX(CGRectGetMinX(available) + extent.width / 2.0,
          MIN(_anchor.x, CGRectGetMaxX(available) - extent.width / 2.0)),
      MAX(CGRectGetMinY(available) + extent.height / 2.0,
          MIN(_anchor.y, CGRectGetMaxY(available) - extent.height / 2.0)));

  for (NSUInteger i = 0; i < shown; i++) {
    const BOOL more = paged && i == count;
    NSDictionary *item = more ? nil : _items[start + i];
    NSString *title = more ? @"More" : item[@"title"];
    UIImage *icon = more ? [UIImage systemImageNamed:@"ellipsis"] : item[@"image"];
    const BOOL destructive = [item[@"destructive"] boolValue];
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    /* Raw Pencil routing owns selection and release, including More. UIKit must
     * not also dispatch the same lift as a second button activation. */
    button.userInteractionEnabled = NO;
    button.accessibilityLabel = title;
    button.accessibilityIdentifier = more ? @"blender.pencil.more" :
                                     [item[@"back"] boolValue] ? @"blender.pencil.back" :
                                     item[@"operator"];
    button.enabled = ![item[@"disabled"] boolValue];
    button.accessibilityValue = [item[@"checked"] boolValue] ? @"On" : nil;
    button.tag = more ? -1 : (NSInteger)(start + i);
    const BOOL checked = [item[@"checked"] boolValue];
    button.tintColor = destructive ? UIColor.systemRedColor : UIColor.labelColor;
    button.configurationUpdateHandler = ^(UIButton *updated) {
      UIButtonConfiguration *config = updated.selected ?
          [UIButtonConfiguration prominentGlassButtonConfiguration] :
          [UIButtonConfiguration glassButtonConfiguration];
      config.title = title;
      config.image = icon;
      config.imagePlacement = NSDirectionalRectEdgeLeading;
      config.imagePadding = icon ? 6.0 : 0.0;
      config.preferredSymbolConfigurationForImage =
          [UIImageSymbolConfiguration configurationWithPointSize:13 weight:UIImageSymbolWeightMedium];
      config.contentInsets = NSDirectionalEdgeInsetsMake(0.0, 10.0, 0.0, 12.0);
      config.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
      config.titleLineBreakMode = NSLineBreakByTruncatingTail;
      config.titleTextAttributesTransformer = ^NSDictionary *(NSDictionary *incoming) {
        NSMutableDictionary *attributes = [[incoming mutableCopy] autorelease];
        attributes[NSFontAttributeName] = [UIFont systemFontOfSize:13 weight:UIFontWeightMedium];
        return attributes;
      };
      if (checked && !updated.selected && !destructive) {
        /* An active toggle reads as tinted, as checked items do in iOS menus. */
        config.baseForegroundColor = UIColor.systemBlueColor;
      }
      if (updated.selected || destructive) {
        config.baseForegroundColor = updated.selected ? UIColor.whiteColor : UIColor.systemRedColor;
        config.baseBackgroundColor = destructive ? UIColor.systemRedColor : UIColor.systemBlueColor;
      }
      updated.configuration = config;
    };
    [button addTarget:self action:@selector(activateButton:) forControlEvents:UIControlEventTouchUpInside];
    /* The inner ring fills clockwise from the top, then the outer ring. More keeps the inner
     * ring's last slot, just left of the top, so it doesn't move between pages. */
    const BOOL outer_ring = !more && i >= _inner.slots - (paged ? 1 : 0);
    const NSUInteger slot = more ? _inner.slots - 1 : outer_ring ? i - (_inner.slots - (paged ? 1 : 0)) : i;
    [_slots addObject:@((outer_ring ? 1000 : 0) + slot)];
    button.bounds = CGRectMake(0, 0, pill.width, pill.height);
    button.center = ring_point(outer_ring ? _outer : _inner, slot, _center);
    [self addSubview:button];
    [_buttons addObject:button];
    [button setNeedsUpdateConfiguration];
    if (animated) {
      button.alpha = 0.0;
      button.transform = CGAffineTransformConcat(
          CGAffineTransformMakeTranslation((_center.x - button.center.x) * 0.3,
                                           (_center.y - button.center.y) * 0.3),
          CGAffineTransformMakeScale(0.8, 0.8));
    }
  }
  _centerView.bounds = CGRectMake(0, 0, 106, 44);
  _centerView.center = _center;
  _centerLabel.frame = CGRectInset(_centerView.bounds, 8, 4);
  _centerView.hidden = NO;
  [self updateCenterLabel];
  if (animated) {
    [UIView animateWithDuration:0.24 delay:0 usingSpringWithDamping:0.82 initialSpringVelocity:0
                        options:UIViewAnimationOptionAllowUserInteraction
                     animations:^{
                       for (UIButton *button in self->_buttons) {
                         button.alpha = 1.0;
                         button.transform = CGAffineTransformIdentity;
                       }
                     } completion:nil];
  }
}

- (void)openAtPoint:(CGPoint)point
{
  if (_menuVisible || !_active) {
    return;
  }
  _anchor = point;
  _layoutSize = self.bounds.size;
  _page = 0;
  _menuVisible = YES;
  [_history removeAllObjects];
  [self loadItemsForRoute:@{@"root": @"context", @"path": @[]} title:nil];
}

- (void)loadItemsForRoute:(NSDictionary *)route title:(NSString *)title
{
  [_title release];
  _title = [title copy];
  const NSUInteger generation = ++_requestGeneration;
  _loading = YES;
  [self selectIndex:NSNotFound];
  [_items release];
  _items = [@[] retain];
  _page = 0;
  [self buildPageAnimated:NO];
  [delegate pencilMenu:self requestItemsForRoute:route atPoint:_anchor
      completion:^(NSArray<NSDictionary *> *items) {
        if (!self->_active || !self->_menuVisible || generation != self->_requestGeneration) {
          return;
        }
        NSMutableArray *page = [NSMutableArray array];
        NSMutableArray *actions = [NSMutableArray array];
        for (NSDictionary *item in items) {
          if ([item[@"heading"] isKindOfClass:NSString.class]) {
            if (!self->_title.length) {
              [self->_title release];
              self->_title = [item[@"heading"] copy];
            }
          }
          else {
            [actions addObject:item];
          }
        }
        if (self->_history.count) {
          [page addObject:@{@"title": @"Back", @"back": @YES,
                            @"image": [UIImage systemImageNamed:@"chevron.backward"]}];
        }
        [page addObjectsFromArray:actions];
        [self->_items release];
        self->_items = [page copy];
        self->_loading = NO;
        [self buildPageAnimated:YES];
      }];
}

- (void)updateSelectionAtPoint:(CGPoint)point
{
  if (!_menuVisible) {
    return;
  }
  /* Neither opening at the tip nor returning to the center chooses an action. */
  if (hypot(point.x - _anchor.x, point.y - _anchor.y) < 18.0 ||
      hypot(point.x - _center.x, point.y - _center.y) < 40.0)
  {
    [self selectIndex:NSNotFound];
    return;
  }
  NSInteger hit = NSNotFound;
  for (NSUInteger i = 0; i < _buttons.count; i++) {
    UIButton *button = _buttons[i];
    CGRect target = CGRectMake(button.center.x - button.bounds.size.width / 2.0,
                               button.center.y - button.bounds.size.height / 2.0,
                               button.bounds.size.width, button.bounds.size.height);
    if (CGRectContainsPoint(CGRectInset(target, -8, -8), point)) {
      hit = i;
      break;
    }
  }
  if (hit == NSNotFound && _inner.slots) {
    /* Between pills, the direction picks the slot and the distance picks the ring. */
    const CGFloat dx = (point.x - _center.x) / _inner.radius_x;
    const CGFloat dy = (point.y - _center.y) / _inner.radius_y;
    const CGFloat distance = hypot(dx, dy);
    const CGFloat outer_distance = _outer.slots ? _outer.radius_y / _inner.radius_y : 0;
    const BOOL outer_ring = _outer.slots && distance > (1.0 + outer_distance) / 2.0;
    const CGFloat limit = _outer.slots ? outer_distance + 0.35 : 1.45;
    if (distance >= 0.6 && distance <= limit) {
      const PencilRing &ring = outer_ring ? _outer : _inner;
      const double step = 2.0 * M_PI / ring.slots;
      const NSInteger slot = (NSInteger(lround((atan2(dy, dx) + M_PI_2 - ring.phase) / step)) +
                              NSInteger(ring.slots)) % NSInteger(ring.slots);
      const NSInteger key = (outer_ring ? 1000 : 0) + slot;
      for (NSUInteger i = 0; i < _slots.count; i++) {
        if (_slots[i].integerValue == key) {
          hit = i;
          break;
        }
      }
    }
  }
  [self selectIndex:hit];
}

- (void)dismiss
{
  ++_requestGeneration;
  _loading = NO;
  _menuVisible = NO;
  _active = NO;
  [self selectIndex:NSNotFound];
  for (UIButton *button in _buttons) {
    [button.layer removeAllAnimations];
    button.hidden = YES;
  }
  _centerView.hidden = YES;
}

- (void)activateButton:(UIButton *)button
{
  if (!_active || !_menuVisible || _loading || !button.enabled) {
    return;
  }
  if (button.tag < 0) {
    _page = (_page + 1) % [self pageCount];
    [self buildPageAnimated:NO];
    return;
  }
  NSDictionary *item = [[_items[button.tag] retain] autorelease];
  if ([item[@"back"] boolValue]) {
    NSDictionary *parent = [[_history.lastObject retain] autorelease];
    [_history removeLastObject];
    [self selectIndex:NSNotFound];
    [_items release];
    _items = [parent[@"items"] copy];
    _page = [parent[@"page"] unsignedIntegerValue];
    [_title release];
    _title = [parent[@"title"] copy];
    [self buildPageAnimated:NO];
    return;
  }
  if ([item[@"submenu"] boolValue]) {
    [_history addObject:@{@"items": _items, @"page": @(_page), @"title": _title ?: @""}];
    [self loadItemsForRoute:item[@"route"] title:item[@"title"]];
    return;
  }
  [self dismiss];
  [delegate pencilMenu:self didChooseItem:item atPoint:_anchor];
}

- (void)toggleWithPencilLocation:(NSValue *)location
{
  if (_active) {
    [self cancel];
    return;
  }
  _active = YES;
  [_feedback prepare];
  if (location) {
    _lastPencilPoint = location.CGPointValue;
    _hasPencilLocation = YES;
  }
  if (_hasPencilLocation) {
    [self openAtPoint:_lastPencilPoint];
  }
}

- (BOOL)handlePencilTouch:(UITouch *)touch
{
  _lastPencilPoint = [touch preciseLocationInView:self];
  _hasPencilLocation = YES;
  if (touch.phase == UITouchPhaseBegan && touch != _capturedTouch) {
    /* UIKit may retire a cancelled touch without another event when backgrounded. */
    _capturedTouch = nil;
  }
  if (!_active && touch != _capturedTouch) {
    return NO;
  }
  if (_capturedTouch == nil && _active && touch.phase == UITouchPhaseBegan) {
    _capturedTouch = touch;
    _openedOnTouch = !_menuVisible;
  }
  if (touch != _capturedTouch) {
    /* A tool stroke cancelled by the secondary interaction must lift before
     * it can select a menu item. Only a fresh contact starts menu selection. */
    return _active;
  }
  if (_active) {
    CGPoint point = [touch preciseLocationInView:self];
    [self openAtPoint:point];
    [self updateSelectionAtPoint:point];
    if (touch.phase == UITouchPhaseEnded) {
      if (_selected != NSNotFound) {
        [self activateButton:_buttons[_selected]];
      }
      else if (!_loading && !_openedOnTouch) {
        [self dismiss];
      }
    }
    else if (touch.phase == UITouchPhaseCancelled) {
      [self dismiss];
    }
  }
  if (touch.phase == UITouchPhaseEnded || touch.phase == UITouchPhaseCancelled) {
    _capturedTouch = nil;
  }
  return YES;
}

- (BOOL)handlePencilHover:(UIHoverGestureRecognizer *)hover
{
  if (hover.state == UIGestureRecognizerStateBegan || hover.state == UIGestureRecognizerStateChanged) {
    _lastPencilPoint = [hover locationInView:self];
    _hasPencilLocation = YES;
    if (_active && _capturedTouch == nil) {
      [self openAtPoint:_lastPencilPoint];
      [self updateSelectionAtPoint:_lastPencilPoint];
    }
  }
  else {
    if (_capturedTouch == nil) {
      [self selectIndex:NSNotFound];
    }
  }
  return _active || _capturedTouch != nil;
}

- (void)cancel
{
  [self dismiss];
  /* A captured Pencil remains swallowed until lift after dismissal. */
}
@end
