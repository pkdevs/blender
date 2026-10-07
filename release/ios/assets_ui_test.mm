/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#import "GHOST_IOSAssets.hh"

static NSMutableArray<NSArray *> *sent;
extern "C" void WM_apple_ios_ui_setting_enqueue(const char *key, const char *value)
{
  [sent addObject:@[@"setting", [NSString stringWithUTF8String:key], [NSString stringWithUTF8String:value]]];
}
extern "C" void WM_apple_ios_ui_asset_drag(int phase, const char *asset_id, int x, int y)
{
  [sent addObject:@[@"drag", @(phase), asset_id ? [NSString stringWithUTF8String:asset_id] : @"", @(x), @(y)]];
}

/* Stands in for a local drag session carrying one asset. */
@interface FakeDropSession : NSObject
@property(nonatomic, retain) NSArray<UIDragItem *> *items;
@property(nonatomic) CGPoint location;
@end

@implementation FakeDropSession
- (id)localDragSession { return self; }
- (CGPoint)locationInView:(UIView *)view { return self.location; }
@end


static UIView *find(UIView *root, NSString *identifier)
{
  if ([root.accessibilityIdentifier isEqual:identifier]) {
    return root;
  }
  for (UIView *child in root.subviews) {
    if (UIView *match = find(child, identifier)) {
      return match;
    }
  }
  return nil;
}

static NSArray<NSString *> *names(UICollectionView *grid)
{
  NSMutableArray *result = [NSMutableArray array];
  const NSInteger count = [grid.dataSource collectionView:grid numberOfItemsInSection:0];
  for (NSInteger i = 0; i < count; i++) {
    UICollectionViewCell *cell = [grid.dataSource collectionView:grid
                                          cellForItemAtIndexPath:[NSIndexPath indexPathForItem:i inSection:0]];
    [result addObject:cell.accessibilityLabel ?: @""];
  }
  return result;
}

int main()
{
  @autoreleasepool {
    [UIApplication sharedApplication];
    [UIView setAnimationsEnabled:NO];
    sent = [NSMutableArray array];
    UIWindow *window = [[[UIWindow alloc] initWithFrame:CGRectMake(0, 0, 1194, 834)] autorelease];
    UIViewController *controller = [[[UIViewController alloc] init] autorelease];
    window.rootViewController = controller;
    [window makeKeyAndVisible];
    GHOSTIOSAssets *assets = [[[GHOSTIOSAssets alloc] initWithOverlay:controller.view] autorelease];
    __block int relayouts = 0;
    assets.visibilityDidChange = ^{ relayouts++; };
    assets.clientPoint = ^CGPoint(CGPoint point) { return CGPointMake(point.x * 2, point.y * 2); };
    [controller.view addInteraction:assets.dropInteraction];

    NSCAssert([assets.button.accessibilityIdentifier isEqual:@"blender.assets"] &&
              [assets.button.title isEqual:@"Assets"], @"Assets have an accessible toolbar toggle");
    NSCAssert(assets.view.hidden && !assets.visible, @"The asset pane starts hidden");
    [assets.button.target performSelector:assets.button.action withObject:assets.button];
    NSCAssert(assets.visible && assets.button.selected && !assets.view.hidden && relayouts == 1 &&
              [sent.lastObject isEqual:(@[@"setting", @"assets", @"show"])],
              @"Toggle shows the pane, asks Blender to list assets and the host to relayout");

    const CGRect lane = CGRectMake(100, 70, 900, 680);
    [assets layoutInRect:lane];
    [assets.view layoutIfNeeded];
    NSCAssert(fabs(CGRectGetMaxY(assets.view.frame) - CGRectGetMaxY(lane)) < 0.5 &&
              fabs(CGRectGetMidX(assets.view.frame) - CGRectGetMidX(lane)) < 0.5 &&
              assets.view.frame.size.height == assets.height,
              @"The pane sits centered along the bottom of its lane");

    UICollectionView *grid = (UICollectionView *)find(assets.view, @"blender.assets.grid");
    UILabel *empty = (UILabel *)grid.backgroundView;
    NSCAssert(!empty.hidden && [empty.text containsString:@"Loading"], @"The pane says while assets load");

    NSDictionary *chair = @{@"id": @"chair", @"name": @"Chair", @"type": @"Object", @"library": @"Essentials",
        @"catalog": @"Furniture/Seating", @"description": @"", @"author": @"", @"tags": @[@"wood"], @"preview": @""};
    NSDictionary *table = @{@"id": @"table", @"name": @"Table", @"type": @"Object", @"library": @"Props",
        @"catalog": @"Furniture", @"description": @"Dining table", @"author": @"", @"tags": @[], @"preview": @""};
    NSDictionary *brass = @{@"id": @"brass", @"name": @"Brass", @"type": @"Material", @"library": @"Essentials",
        @"catalog": @"Metals", @"description": @"", @"author": @"", @"tags": @[@"metal"], @"preview": @""};
    NSDictionary *(^starter)(NSString *, NSString *) = ^(NSString *name, NSString *catalog) {
      return @{@"id": name, @"name": name, @"type": @"Object", @"library": @"Starter",
               @"catalog": catalog, @"description": @"", @"author": @"", @"tags": @[],
               @"preview": @""};
    };
    NSDictionary *building = starter(@"Building", @"Objects/Architecture");
    NSDictionary *cube = starter(@"Cube", @"Objects/Primitives");
    NSDictionary *plane = starter(@"Plane", @"Objects/Primitives");
    NSDictionary *scatter = @{@"id": @"scatter", @"name": @"Scatter on Surface", @"type": @"Modifier",
        @"library": @"Starter", @"catalog": @"Generate", @"description": @"", @"author": @"",
        @"tags": @[@"scatter"], @"preview": @""};
    [assets updateSnapshot:@{@"items": @[chair, table, brass, building, cube, plane, scatter]}];
    NSCAssert([names(grid) isEqual:(@[@"Cube", @"Plane"])] && empty.hidden,
              @"The pane starts on the primitives: %@", names(grid));

    /* One row of categories: objects by catalog name, across libraries, then other types. */
    UIView *chips = find(assets.view, @"blender.assets.catalogs");
    NSMutableArray *titles = [NSMutableArray array];
    for (UIView *chip in ((UIStackView *)chips.subviews.firstObject).arrangedSubviews) {
      [titles addObject:chip.accessibilityLabel];
    }
    NSCAssert([titles isEqual:(@[@"Architecture", @"Furniture", @"Primitives", @"Seating",
                                 @"Materials", @"Modifiers"])],
              @"Chips list the categories: %@", titles);
    UIButton *(^chip)(NSString *) = ^(NSString *title) {
      return (UIButton *)find(assets.view, [@"blender.assets.category." stringByAppendingString:title]);
    };
    NSCAssert(chip(@"Primitives").selected, @"The shown category's chip is selected");
    [chip(@"Architecture") sendActionsForControlEvents:UIControlEventPrimaryActionTriggered];
    NSCAssert([names(grid) isEqual:@[@"Building"]] && chip(@"Architecture").selected &&
              !chip(@"Primitives").selected, @"A chip shows its category");

    UISearchTextField *search = (UISearchTextField *)find(assets.view, @"blender.assets.search");
    search.text = @"WOOD";
    [search sendActionsForControlEvents:UIControlEventEditingChanged];
    NSCAssert([names(grid) isEqual:@[@"Chair"]] && !chip(@"Architecture").selected,
              @"Search looks through every category, matching tags and ignoring case");
    search.text = @"furniture dining";
    [search sendActionsForControlEvents:UIControlEventEditingChanged];
    NSCAssert([names(grid) isEqual:@[@"Table"]], @"Every word must match the catalog or description");
    search.text = @"nothing";
    [search sendActionsForControlEvents:UIControlEventEditingChanged];
    NSCAssert(names(grid).count == 0 && !empty.hidden && [empty.text containsString:@"nothing"],
              @"An empty search says what found nothing");
    [chip(@"Seating") sendActionsForControlEvents:UIControlEventPrimaryActionTriggered];
    NSCAssert([names(grid) isEqual:@[@"Chair"]] && search.text.length == 0,
              @"Picking a category clears the search");

    [chip(@"Modifiers") sendActionsForControlEvents:UIControlEventPrimaryActionTriggered];
    NSCAssert([names(grid) isEqual:@[@"Scatter on Surface"]], @"Other types have a category each");
    [grid.delegate collectionView:grid didSelectItemAtIndexPath:[NSIndexPath indexPathForItem:0 inSection:0]];
    NSCAssert([sent.lastObject isEqual:(@[@"drag", @4, @"scatter", @0, @0])], @"Tapping a modifier adds it");
    [chip(@"Materials") sendActionsForControlEvents:UIControlEventPrimaryActionTriggered];
    [sent removeAllObjects];
    [grid.delegate collectionView:grid didSelectItemAtIndexPath:[NSIndexPath indexPathForItem:0 inSection:0]];
    NSCAssert([sent.lastObject isEqual:(@[@"drag", @4, @"brass", @0, @0])], @"Tapping adds the asset: %@", sent);
    [assets updateSnapshot:@{@"items": @[chair, table, brass]}];
    NSCAssert([names(grid) isEqual:@[@"Brass"]], @"The picked category stays while it exists");

    /* Drag the chair from the pane into the viewport and drop it. */
    UIDragItem *item = [[[UIDragItem alloc] initWithItemProvider:[[[NSItemProvider alloc] init] autorelease]] autorelease];
    item.localObject = chair;
    FakeDropSession *session = [[[FakeDropSession alloc] init] autorelease];
    session.items = @[item];
    id<UIDropInteractionDelegate> drop = assets.dropInteraction.delegate;
    UIDropInteraction *interaction = assets.dropInteraction;
    NSCAssert([drop dropInteraction:interaction canHandleSession:(id)session], @"Asset drags are accepted");
    [sent removeAllObjects];
    session.location = CGPointMake(CGRectGetMidX(assets.view.frame), CGRectGetMidY(assets.view.frame));
    [drop dropInteraction:interaction sessionDidEnter:(id)session];
    UIDropProposal *proposal = [drop dropInteraction:interaction sessionDidUpdate:(id)session];
    NSCAssert(proposal.operation == UIDropOperationCancel && sent.count == 0,
              @"Over the pane, nothing previews in the viewport");
    session.location = CGPointMake(300, 200);
    proposal = [drop dropInteraction:interaction sessionDidUpdate:(id)session];
    session.location = CGPointMake(310, 210);
    [drop dropInteraction:interaction sessionDidUpdate:(id)session];
    NSCAssert(proposal.operation == UIDropOperationCopy &&
              [sent[0] isEqual:(@[@"drag", @0, @"chair", @600, @400])] &&
              [sent[1] isEqual:(@[@"drag", @1, @"chair", @620, @420])],
              @"Entering the viewport starts Blender's drag, then moves it in client coordinates: %@", sent);
    UIDragPreview *pill = item.previewProvider ? item.previewProvider() : nil;
    NSCAssert(pill && pill.view.bounds.size.width > 100 && assets.view.alpha < 0.5,
              @"Over the viewport the preview becomes a pill beside the finger and the pane fades");
    [drop dropInteraction:interaction performDrop:(id)session];
    [drop dropInteraction:interaction sessionDidEnd:(id)session];
    NSCAssert(assets.view.alpha == 1.0, @"The pane comes back when the drag ends");
    NSCAssert([sent.lastObject isEqual:(@[@"drag", @2, @"chair", @620, @420])] && sent.count == 3,
              @"Releasing drops the asset where it is, without a cancel: %@", sent);

    /* A drag that returns to the pane stops previewing; one that leaves cancels. */
    [sent removeAllObjects];
    session.location = CGPointMake(300, 200);
    [drop dropInteraction:interaction sessionDidEnter:(id)session];
    [drop dropInteraction:interaction sessionDidUpdate:(id)session];
    session.location = CGPointMake(CGRectGetMidX(assets.view.frame), CGRectGetMidY(assets.view.frame));
    [drop dropInteraction:interaction sessionDidUpdate:(id)session];
    NSCAssert([sent.lastObject[1] isEqual:@3], @"Back over the pane cancels the viewport drag: %@", sent);
    UIDragPreview *thumbnail = item.previewProvider ? item.previewProvider() : nil;
    NSCAssert(thumbnail && thumbnail.view.bounds.size.width == 96 && assets.view.alpha == 1.0,
              @"Back over the pane the thumbnail preview and the pane return");
    session.location = CGPointMake(300, 200);
    [drop dropInteraction:interaction sessionDidUpdate:(id)session];
    [drop dropInteraction:interaction sessionDidExit:(id)session];
    [drop dropInteraction:interaction sessionDidEnd:(id)session];
    NSCAssert([sent.lastObject[1] isEqual:@3] && sent.count == 4,
              @"Leaving cancels exactly once: %@", sent);

    [assets.button.target performSelector:assets.button.action withObject:assets.button];
    NSCAssert(!assets.visible && [sent.lastObject isEqual:(@[@"setting", @"assets", @"hide"])],
              @"Hiding stops Blender listing assets");
    NSLog(@"PASS: native asset pane");
  }
  return 0;
}
