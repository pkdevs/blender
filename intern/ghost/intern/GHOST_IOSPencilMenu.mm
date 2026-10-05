/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#import "GHOST_IOSPencilMenu.hh"

#include <cmath>

@implementation GHOSTIOSPencilMenu {
  NSArray<NSDictionary *> *_items;
  NSMutableArray<UIButton *> *_buttons;
  NSMutableArray<NSDictionary *> *_history;
  UIVisualEffectView *_centerView;
  UILabel *_centerLabel;
  UISelectionFeedbackGenerator *_feedback;
  UITouch *_capturedTouch;
  CGPoint _lastPencilPoint;
  CGPoint _anchor;
  CGPoint _center;
  CGFloat _radiusX, _radiusY;
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
  [_history release];
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
  _centerLabel.text = _selected == NSNotFound ?
      [NSString stringWithFormat:@"%lu / %lu", (unsigned long)_page + 1,
          (unsigned long)((_items.count + 6) / 7)] :
      _buttons[_selected].accessibilityLabel;
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

  CGRect available = UIEdgeInsetsInsetRect(self.bounds, self.safeAreaInsets);
  available = CGRectInset(available, 12.0, 12.0);
  const CGFloat width = MIN(92.0, MAX(70.0, (available.size.width - 16.0) / 4.0));
  const CGFloat height = 70.0;
  _radiusX = MIN(140.0, (available.size.width - width) / 2.0);
  _radiusY = MIN(128.0, (available.size.height - height) / 2.0);
  _center = CGPointMake(
      MAX(CGRectGetMinX(available) + _radiusX + width / 2.0,
          MIN(_anchor.x, CGRectGetMaxX(available) - _radiusX - width / 2.0)),
      MAX(CGRectGetMinY(available) + _radiusY + height / 2.0,
          MIN(_anchor.y, CGRectGetMaxY(available) - _radiusY - height / 2.0)));

  const NSUInteger start = _page * 7;
  const NSUInteger count = MIN((NSUInteger)7, _items.count - start);
  const BOOL paged = _items.count > 7;
  for (NSUInteger i = 0; i < count + (paged ? 1 : 0); i++) {
    const BOOL more = i == count;
    NSDictionary *item = more ? nil : _items[start + i];
    NSString *title = more ? @"More" : item[@"title"];
    UIImage *icon = more ? [UIImage systemImageNamed:@"ellipsis"] : item[@"image"];
    const BOOL destructive = [item[@"destructive"] boolValue];
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    button.titleLabel.numberOfLines = 2;
    button.titleLabel.adjustsFontSizeToFitWidth = YES;
    button.titleLabel.minimumScaleFactor = 0.75;
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
    button.tintColor = destructive ? UIColor.systemRedColor : UIColor.labelColor;
    button.configurationUpdateHandler = ^(UIButton *updated) {
      UIButtonConfiguration *config = updated.selected ?
          [UIButtonConfiguration prominentGlassButtonConfiguration] :
          [UIButtonConfiguration glassButtonConfiguration];
      config.title = title;
      config.subtitle = [item[@"checked"] boolValue] ? @"On" : nil;
      config.image = icon;
      config.imagePlacement = NSDirectionalRectEdgeTop;
      config.imagePadding = icon ? 3.0 : 0.0;
      config.contentInsets = NSDirectionalEdgeInsetsMake(5.0, 5.0, 5.0, 5.0);
      config.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
      config.titleAlignment = UIButtonConfigurationTitleAlignmentCenter;
      config.titleLineBreakMode = NSLineBreakByWordWrapping;
      config.titleTextAttributesTransformer = ^NSDictionary *(NSDictionary *incoming) {
        NSMutableDictionary *attributes = [[incoming mutableCopy] autorelease];
        attributes[NSFontAttributeName] = [UIFont systemFontOfSize:12 weight:UIFontWeightMedium];
        return attributes;
      };
      if (updated.selected || destructive) {
        config.baseForegroundColor = updated.selected ? UIColor.whiteColor : UIColor.systemRedColor;
        config.baseBackgroundColor = destructive ? UIColor.systemRedColor : UIColor.systemBlueColor;
      }
      updated.configuration = config;
    };
    [button addTarget:self action:@selector(activateButton:) forControlEvents:UIControlEventTouchUpInside];
    /* Keep More in the north-west slot so it doesn't move between pages. */
    const NSUInteger slot = more ? 7 : i;
    const double angle = -M_PI_2 + slot * M_PI_4;
    button.bounds = CGRectMake(0, 0, width, height);
    button.center = CGPointMake(_center.x + cos(angle) * _radiusX,
                                _center.y + sin(angle) * _radiusY);
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
  [self loadItemsForRoute:@{@"root": @"context", @"path": @[]}];
}

- (void)loadItemsForRoute:(NSDictionary *)route
{
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
        if (self->_history.count) {
          [page addObject:@{@"title": @"Back", @"back": @YES,
                            @"image": [UIImage systemImageNamed:@"chevron.backward"]}];
        }
        [page addObjectsFromArray:items ?: @[]];
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
  if (hit == NSNotFound) {
    const CGFloat dx = (point.x - _center.x) / _radiusX;
    const CGFloat dy = (point.y - _center.y) / _radiusY;
    const CGFloat distance = hypot(dx, dy);
    if (distance >= 0.65 && distance <= 1.45) {
      const NSInteger slot = (NSInteger(lround((atan2(dy, dx) + M_PI_2) / M_PI_4)) + 8) % 8;
      for (NSUInteger i = 0; i < _buttons.count; i++) {
        if ((_buttons[i].tag < 0 ? 7 : NSInteger(i)) == slot) {
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
    _page = (_page + 1) % ((_items.count + 6) / 7);
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
    [self buildPageAnimated:NO];
    return;
  }
  if ([item[@"submenu"] boolValue]) {
    [_history addObject:@{@"items": _items, @"page": @(_page)}];
    [self loadItemsForRoute:item[@"route"]];
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
