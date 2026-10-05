/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#import "GHOST_IOSScenePanels.hh"

@interface GHOSTIOSScenePanels (PropertyHeadingTests)
- (NSArray<UIMenuElement *> *)groupMenuActionsForProperty:(NSDictionary *)property;
- (void)beginNumberScrub:(UITextField *)field;
- (void)changeNumberScrub:(UITextField *)field translation:(CGFloat)translation;
- (void)finishNumberScrub:(UITextField *)field;
- (void)cancelNumberScrub;
- (BOOL)gestureRecognizerShouldBegin:(UIGestureRecognizer *)gesture;
- (UISwipeActionsConfiguration *)hierarchySwipeActionsAtIndexPath:(NSIndexPath *)path leading:(BOOL)leading;
@end

@interface NumberTestPan : UIPanGestureRecognizer
@property(nonatomic) CGPoint testVelocity;
@end
@implementation NumberTestPan
- (CGPoint)velocityInView:(UIView *)view { return self.testVelocity; }
@end

static NSString *setting;
static NSDictionary *request;
extern "C" void WM_apple_ios_ui_setting_enqueue(const char *key, const char *value)
{
  setting = [NSString stringWithUTF8String:key];
  if ([setting isEqual:@"native_panel_edit"] || [setting isEqual:@"native_properties_edit"]) {
    request = [NSJSONSerialization JSONObjectWithData:[[NSString stringWithUTF8String:value]
        dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil];
  }
}

static void tap(UIBarButtonItem *button)
{
  [button.target performSelector:button.action withObject:button];
}

static void testHierarchySwipes(UIView *overlay)
{
  GHOSTIOSScenePanels *panels = [[[GHOSTIOSScenePanels alloc] initWithOverlay:overlay
      icon:^UIImage *(NSString *name) { return nil; }] autorelease];
  [panels layoutInRect:CGRectMake(0, 70, 1194, 680)];
  tap(panels.hierarchyButton);
  NSMutableDictionary *node = [NSMutableDictionary dictionaryWithDictionary:
      @{@"key": @"object", @"parent": @"", @"id": @"42", @"kind": @"object", @"name": @"Cube",
        @"enabled": @YES, @"actions": @[@"duplicate", @"rename", @"delete", @"visibility"]}];
  void (^update)(void) = ^{
    [panels updateSnapshot:@{@"ack": @99999, @"hierarchy": @[[[node copy] autorelease]]}];
  };
  update();
  NSIndexPath *path = [NSIndexPath indexPathForItem:0 inSection:0];
  UISwipeActionsConfiguration *leading = [panels hierarchySwipeActionsAtIndexPath:path leading:YES];
  UISwipeActionsConfiguration *trailing = [panels hierarchySwipeActionsAtIndexPath:path leading:NO];
  NSCAssert([leading.actions[0].title isEqual:@"Duplicate"] && [leading.actions[1].title isEqual:@"Rename"],
            @"Leading swipe exposes duplicate and rename");
  NSCAssert([trailing.actions[0].title isEqual:@"Delete"] && trailing.actions[0].style == UIContextualActionStyleDestructive &&
            [trailing.actions[1].title isEqual:@"Hide"], @"Trailing swipe exposes destructive delete and visibility");
  NSCAssert(!leading.performsFirstActionWithFullSwipe && !trailing.performsFirstActionWithFullSwipe,
            @"Full swipe cannot accidentally execute an action");
  __block BOOL completed = NO;
  UIContextualAction *duplicate = leading.actions[0];
  duplicate.handler(duplicate, overlay, ^(BOOL success) { completed = success; });
  NSCAssert(completed && [request[@"action"] isEqual:@"duplicate"] && [request[@"id"] isEqual:@"42"],
            @"Duplicate queues only the swiped object");
  node[@"hidden"] = @YES;
  update();
  UIContextualAction *hide = trailing.actions[1];
  hide.handler(hide, overlay, ^(BOOL success) {});
  NSCAssert([request[@"action"] isEqual:@"visibility"] && ![request[@"value"] boolValue],
            @"Visibility uses the latest state even when the swipe was opened earlier");
  node[@"actions"] = @[@"visibility"];
  update();
  NSDictionary *before = request;
  duplicate.handler(duplicate, overlay, ^(BOOL success) { completed = success; });
  NSCAssert(!completed && request == before, @"Stale swipe actions cannot bypass changed availability");
  NSCAssert(![panels hierarchySwipeActionsAtIndexPath:path leading:YES] &&
            [[panels hierarchySwipeActionsAtIndexPath:path leading:NO].actions[0].title isEqual:@"Show"],
            @"Unavailable actions disappear and hidden rows offer Show");
  [panels dismiss];
}

static void testShadingPopover(UIViewController *controller)
{
  GHOSTIOSScenePanels *panels = [[[GHOSTIOSScenePanels alloc] initWithOverlay:controller.view
      icon:^UIImage *(NSString *name) { return nil; }] autorelease];
  UIBarButtonItem *anchor = [[[UIBarButtonItem alloc] initWithTitle:@"Shading"
      style:UIBarButtonItemStylePlain target:nil action:nil] autorelease];
  controller.navigationItem.rightBarButtonItem = anchor;
  [panels presentShadingFrom:anchor controller:controller];
  GHOSTIOSScenePanels *shading = [panels valueForKey:@"_shadingPanels"];
  NSCAssert(shading && [setting isEqual:@"native_shading_visible"], @"Opening requests asynchronous shading snapshots");
  NSMutableDictionary *mode = [NSMutableDictionary dictionaryWithDictionary:
      @{@"key": @"shading.type", @"property": @"type", @"type": @"ENUM", @"label": @"Shading",
        @"tab": @"SHADING", @"native": @YES, @"editable": @YES, @"value": @"SOLID", @"options": @[
          @{@"value": @"WIREFRAME", @"label": @"Wireframe"},
          @{@"value": @"SOLID", @"label": @"Solid"},
          @{@"value": @"MATERIAL", @"label": @"Material"},
          @{@"value": @"RENDERED", @"label": @"Rendered"}]}];
  void (^snapshot)(void) = ^{
    [panels updateSnapshot:@{@"ack": @99999, @"shading": @{@"id": @"SHADING:1", @"tab": @"SHADING",
      @"name": @"Viewport Shading", @"tabs": @[], @"sections": @[
        @{@"id": @"shading", @"title": @"Shading", @"open": @YES,
          @"properties": @[[[mode copy] autorelease]]}]}}];
  };
  snapshot();
  UISegmentedControl *segments = [shading valueForKey:@"_controls"][@"shading.type"];
  NSCAssert([segments isKindOfClass:UISegmentedControl.class] && segments.numberOfSegments == 4 &&
            segments.selectedSegmentIndex == 1, @"Shading modes use the native segmented control");
  UITableView *form = [shading valueForKey:@"_form"];
  NSCAssert(!form.backgroundView, @"First snapshot replaces the loading indicator");
  segments.selectedSegmentIndex = 2;
  [segments sendActionsForControlEvents:UIControlEventValueChanged];
  NSCAssert([request[@"tab"] isEqual:@"SHADING"] && [request[@"value"] isEqual:@"MATERIAL"] &&
            [setting isEqual:@"native_properties_edit"], @"Mode changes use the asynchronous RNA edit queue");
  mode[@"value"] = @"RENDERED";
  snapshot();
  NSCAssert([shading valueForKey:@"_controls"][@"shading.type"] == segments && segments.selectedSegmentIndex == 3,
            @"External shading changes update the selector in place");
  GHOSTIOSScenePanels *other = [[[GHOSTIOSScenePanels alloc] initWithOverlay:controller.view
      icon:^UIImage *(NSString *name) { return nil; }] autorelease];
  NSNumber *shadingSequence = request[@"sequence"];
  [other performSelector:@selector(send:) withObject:@{@"native": @YES, @"key": @"test"}];
  NSCAssert([request[@"sequence"] unsignedLongLongValue] > shadingSequence.unsignedLongLongValue,
            @"Inspector and shading instances share monotonic command sequence numbers");
  [panels dismissShading];
  NSCAssert(![[shading valueForKey:@"_showInspector"] boolValue] && [setting isEqual:@"native_shading_visible"],
            @"Closing stops shading snapshot requests");
}

static void testNumberScrubbing(UIView *overlay)
{
  GHOSTIOSScenePanels *panels = [[[GHOSTIOSScenePanels alloc] initWithOverlay:overlay
      icon:^UIImage *(NSString *name) { return nil; }] autorelease];
  [panels layoutInRect:CGRectMake(0, 70, 1194, 680)];
  tap(panels.inspectorButton);
  NSMutableDictionary *property = [NSMutableDictionary dictionaryWithDictionary:
      @{@"key": @"number", @"owner": @"object", @"property": @"test", @"label": @"Number",
        @"type": @"FLOAT", @"value": @1.5, @"editable": @YES, @"dragStep": @0.01,
        @"min": @(-1e30), @"max": @1e30}];
  UITextField *(^apply)(void) = ^UITextField *{
    [panels updateSnapshot:@{@"ack": @99999, @"inspector": @{@"id": @"99", @"name": @"Test",
        @"sections": @[@{@"id": @"numbers", @"title": @"Numbers", @"open": @YES,
                         @"properties": @[[[property copy] autorelease]]}]}}];
    return [panels valueForKey:@"_controls"][@"number"];
  };
  UITextField *field = apply();
  NSNumberFormatter *numbers = [[[NSNumberFormatter alloc] init] autorelease];
  numbers.numberStyle = NSNumberFormatterDecimalStyle;
  [overlay layoutIfNeeded];
  [[panels valueForKey:@"_form"] layoutIfNeeded];
  BOOL hasPan = NO;
  for (UIGestureRecognizer *gesture in field.gestureRecognizers) {
    hasPan |= [gesture.name isEqual:@"blender.number.scrub"];
  }
  NSCAssert(hasPan, @"Non-slider numeric fields have a scrub gesture");
  NumberTestPan *pan = [[[NumberTestPan alloc] init] autorelease];
  pan.name = @"blender.number.scrub";
  [field addGestureRecognizer:pan];
  pan.testVelocity = CGPointMake(2, 30);
  NSCAssert(![panels gestureRecognizerShouldBegin:pan], @"Vertical drags remain scrolling");
  pan.testVelocity = CGPointMake(30, 2);
  NSCAssert([panels gestureRecognizerShouldBegin:pan], @"Horizontal drags scrub");
  NSCAssert([field becomeFirstResponder], @"Numeric field can still receive typed input");
  NSCAssert(![panels gestureRecognizerShouldBegin:pan], @"Typing preserves text-selection gestures");
  [field resignFirstResponder];
  NSDictionary *before = request;
  [panels beginNumberScrub:field];
  [panels changeNumberScrub:field translation:25];
  NSCAssert([[numbers numberFromString:field.text] doubleValue] == 1.75 && request == before,
            @"Drag previews smoothly without engine commands");
  apply();
  NSCAssert([[numbers numberFromString:field.text] doubleValue] == 1.75, @"Snapshots do not erase an active drag");
  [panels finishNumberScrub:field];
  NSCAssert([request[@"value"] doubleValue] == 1.75, @"Release commits one edit");
  [panels beginNumberScrub:field];
  [panels changeNumberScrub:field translation:25];
  NSCAssert([[numbers numberFromString:field.text] doubleValue] == 2.0,
            @"Rapid repeated drags start from the pending value without waiting for Blender");
  [panels cancelNumberScrub];
  property[@"value"] = @1.75;
  field = apply();
  before = request;
  [panels beginNumberScrub:field];
  [panels changeNumberScrub:field translation:-50];
  NSCAssert([[numbers numberFromString:field.text] doubleValue] == 1.25, @"Leftward drags decrease the number");
  [panels cancelNumberScrub];
  NSCAssert([[numbers numberFromString:field.text] doubleValue] == 1.75 && request == before, @"Cancelled drags restore without an edit");
  [panels beginNumberScrub:field];
  [panels finishNumberScrub:field];
  NSCAssert(request == before, @"Zero-distance drags do not create undo entries");
  [panels beginNumberScrub:field];
  [panels changeNumberScrub:field translation:100];
  property[@"value"] = @7;
  apply();
  [panels finishNumberScrub:field];
  NSCAssert([field.text isEqual:@"7"] && request == before, @"External changes cancel stale scrubs");
  property[@"type"] = @"INT";
  property[@"dragStep"] = @0.5;
  property[@"min"] = @0;
  property[@"max"] = @10;
  property[@"value"] = @9;
  field = apply();
  [panels beginNumberScrub:field];
  [panels changeNumberScrub:field translation:100];
  NSCAssert([field.text isEqual:@"10"], @"Scrubbing respects hard limits");
  [panels changeNumberScrub:field translation:96];
  NSCAssert([field.text isEqual:@"8"], @"Reversing at a limit responds immediately");
  [panels finishNumberScrub:field];
  NSCAssert([request[@"value"] intValue] == 8, @"Integer scrubs commit integral values");
  property[@"type"] = @"FLOAT";
  property[@"dragStep"] = @1;
  property[@"angle"] = @YES;
  property[@"max"] = @1000;
  property[@"value"] = @0;
  field = apply();
  [panels beginNumberScrub:field];
  [panels changeNumberScrub:field translation:90];
  NSCAssert([field.text isEqual:@"90"], @"Angles scrub in displayed degrees");
  [panels finishNumberScrub:field];
  NSCAssert(fabs([request[@"value"] doubleValue] - M_PI_2) < 1e-9, @"Angles commit radians to Blender");
  property[@"editable"] = @NO;
  field = apply();
  before = request;
  [panels beginNumberScrub:field];
  [panels changeNumberScrub:field translation:50];
  [panels finishNumberScrub:field];
  NSCAssert(request == before, @"Disabled fields cannot be scrubbed");
  [panels dismiss];
}

int main()
{
  @autoreleasepool {
    [UIApplication sharedApplication];
    [UIView setAnimationsEnabled:NO];
    UIWindow *window = [[[UIWindow alloc] initWithFrame:CGRectMake(0, 0, 1194, 834)] autorelease];
    UIViewController *controller = [[[UIViewController alloc] init] autorelease];
    window.rootViewController = controller;
    [window makeKeyAndVisible];
    GHOSTIOSScenePanels *panels = [[[GHOSTIOSScenePanels alloc] initWithOverlay:controller.view
        icon:^UIImage *(NSString *name) { return [UIImage systemImageNamed:@"cube"]; }] autorelease];
    CGRect safe = CGRectMake(0, 70, 1194, 680);
    [panels layoutInRect:safe];
    tap(panels.inspectorButton);
    tap(panels.hierarchyButton);
    NSCAssert(panels.inspectorButton.selected && panels.hierarchyButton.selected, @"Independent toggles");
    UIView *inspector = [panels valueForKey:@"_inspector"];
    UIView *hierarchy = [panels valueForKey:@"_hierarchy"];
    NSCAssert(CGRectContainsRect(safe, inspector.frame) && CGRectContainsRect(safe, hierarchy.frame) &&
              !CGRectIntersectsRect(inspector.frame, hierarchy.frame), @"iPad floating bounds");
    NSDictionary *property = @{@"owner": @"object", @"ownerID": @"7", @"property": @"location", @"index": @0,
                               @"label": @"X", @"type": @"FLOAT", @"value": @1.5, @"editable": @YES};
    NSDictionary *model = @{@"ack": @0, @"error": @"", @"inspector": @{@"id": @"7", @"name": @"Cube",
      @"sections": @[@{@"title": @"Location", @"properties": @[property]}]},
      @"hierarchy": @[
        @{@"key": @"/c1", @"parent": @"", @"id": @"1", @"kind": @"collection", @"depth": @0, @"name": @"Scene", @"icon": @"COLLECTION", @"enabled": @YES},
        @{@"key": @"/c1/o7", @"parent": @"/c1", @"id": @"7", @"kind": @"object", @"depth": @1, @"name": @"Cube", @"icon": @"MESH", @"active": @YES, @"selected": @YES, @"enabled": @YES},
        @{@"key": @"/c1/o8", @"parent": @"/c1", @"id": @"8", @"kind": @"object", @"depth": @1, @"name": @"Camera", @"icon": @"CAMERA", @"enabled": @YES},
      ]};
    [panels updateSnapshot:model];
    UICollectionView *tree = [panels valueForKey:@"_tree"];
    UICollectionViewDiffableDataSource *dataSource = [panels valueForKey:@"_treeDataSource"];
    NSCAssert([tree isKindOfClass:UICollectionView.class] && [tree numberOfItemsInSection:0] == 3, @"Native outline rows");
    [tree layoutIfNeeded];
    UICollectionViewListCell *root = (UICollectionViewListCell *)[tree cellForItemAtIndexPath:[NSIndexPath indexPathForItem:0 inSection:0]];
    NSCAssert([root isKindOfClass:UICollectionViewListCell.class] &&
              [root.accessories.firstObject isKindOfClass:UICellAccessoryOutlineDisclosure.class], @"System disclosure accessory");
    NSCAssert([tree.indexPathsForSelectedItems containsObject:[NSIndexPath indexPathForItem:1 inSection:0]], @"Blender selection uses native selected state");
    NSDiffableDataSourceSectionSnapshot *outline = [dataSource snapshotForSection:@"scene"];
    dataSource.sectionSnapshotHandlers.willCollapseItemHandler(@"/c1");
    [outline collapseItems:@[@"/c1"]];
    [dataSource applySnapshot:outline toSection:@"scene" animatingDifferences:NO];
    NSCAssert([tree numberOfItemsInSection:0] == 1, @"Collapse collection");
    UISearchBar *search = [panels valueForKey:@"_search"];
    search.text = @"camera";
    [search.delegate searchBar:search textDidChange:search.text];
    NSCAssert([tree numberOfItemsInSection:0] == 2, @"Search reveals ancestors");
    [tree.delegate collectionView:tree didSelectItemAtIndexPath:[NSIndexPath indexPathForItem:1 inSection:0]];
    NSCAssert([request[@"action"] isEqual:@"select"] && [request[@"id"] isEqual:@"8"], @"Selection queued");
    search.text = @"";
    [search.delegate searchBar:search textDidChange:search.text];
    NSCAssert([tree numberOfItemsInSection:0] == 1, @"Search preserves collapsed branches");
    NSDictionary *controls = [panels valueForKey:@"_controls"];
    UITextField *field = controls[@"object.location.0"];
    NSNumberFormatter *formatter = [[[NSNumberFormatter alloc] init] autorelease];
    formatter.numberStyle = NSNumberFormatterDecimalStyle;
    NSCAssert([[formatter numberFromString:field.text] doubleValue] == 1.5, @"Blender transform value: %@", field.text);
    NSString *editedText = [formatter stringFromNumber:@2.75];
    field.text = editedText;
    for (id target in field.allTargets) {
      for (NSString *action in [field actionsForTarget:target forControlEvent:UIControlEventEditingDidEnd]) {
        [target performSelector:NSSelectorFromString(action) withObject:field];
      }
    }
    NSCAssert([request[@"action"] isEqual:@"property"] && [request[@"value"] doubleValue] == 2.75 &&
              [request[@"id"] isEqual:@"7"], @"Native field queues typed edit to original object: %@, %@, %@", request, field.text, [[panels valueForKey:@"_status"] text]);
    [panels updateSnapshot:model];
    NSCAssert([field.text isEqual:editedText], @"Stale snapshot cannot erase pending edit");
    NSMutableDictionary *acknowledged = [[model mutableCopy] autorelease];
    acknowledged[@"ack"] = @2;
    [panels updateSnapshot:acknowledged];
    NSCAssert([panels valueForKey:@"_controls"] == controls && controls[@"object.location.0"] == field,
              @"Stable control identity");
    NSDictionary *outerToggle = @{@"key": @"outer.enable", @"label": @"Enable Feature",
      @"type": @"BOOLEAN", @"header": @YES, @"native": @YES, @"value": @YES, @"editable": @YES};
    NSDictionary *innerToggle = @{@"key": @"inner.enable", @"label": @"Enable Subfeature",
      @"type": @"BOOLEAN", @"header": @YES, @"section": @"Subfeature", @"native": @YES,
      @"value": @NO, @"editable": @YES};
    NSMutableDictionary *headerModel = [[acknowledged mutableCopy] autorelease];
    headerModel[@"inspector"] = @{@"id": @"header-test", @"name": @"Properties",
      @"sections": @[@{@"id": @"outer", @"title": @"Feature", @"open": @YES,
        @"properties": @[outerToggle, property, innerToggle]}]};
    [panels updateSnapshot:headerModel];
    UITableView *form = [panels valueForKey:@"_form"];
    UIStackView *header = (UIStackView *)[form.delegate tableView:form viewForHeaderInSection:0];
    controls = [panels valueForKey:@"_controls"];
    UISwitch *outerSwitch = controls[@"outer.enable"];
    UISwitch *innerSwitch = controls[@"inner.enable"];
    NSCAssert([header isKindOfClass:UIStackView.class] && [outerSwitch isDescendantOfView:header],
              @"Outer enable switch belongs to header, not contents");
    NSCAssert(outerSwitch.on && !innerSwitch.on, @"Header switches reflect live Blender state");
    NSArray *content = [panels valueForKey:@"_formSections"][0];
    NSCAssert(content.count == 2 && [innerSwitch isDescendantOfView:content[1]],
              @"Nested panel is an inline section with its own header switch");
    NSCAssert([form.dataSource numberOfSectionsInTableView:form] == 1,
              @"Only outermost panels are table sections");
    NSDictionary *rgba = @{@"key": @"object.color", @"label": @"Color", @"type": @"COLOR",
      @"native": @YES, @"alpha": @YES, @"editable": @YES, @"value": @[@0.5, @0.25, @0.75, @0.4]};
    NSDictionary *rgb = @{@"key": @"light.color", @"label": @"Light Color", @"type": @"COLOR",
      @"native": @YES, @"alpha": @NO, @"editable": @YES, @"value": @[@1, @0.5, @0.25]};
    NSMutableDictionary *colorModel = [[acknowledged mutableCopy] autorelease];
    colorModel[@"inspector"] = @{@"id": @"colors", @"name": @"Properties",
      @"sections": @[@{@"id": @"colors", @"title": @"Colors", @"open": @YES, @"properties": @[rgba, rgb]}]};
    [panels updateSnapshot:colorModel];
    controls = [panels valueForKey:@"_controls"];
    UIButton *colorButton = controls[@"object.color"];
    UIColorWell *well = [colorButton valueForKey:@"well"];
    NSCAssert([well isKindOfClass:UIColorWell.class] && well.supportsAlpha &&
              ![[controls[@"light.color"] valueForKey:@"well"] supportsAlpha],
              @"RGB and RGBA use native color swatches with correct alpha support");
    NSCAssert(controls.count == 2, @"Color properties are single controls, not component fields");
    [panels performSelector:NSSelectorFromString(@"openColorPicker:") withObject:colorButton];
    UIColorPickerViewController *picker = [panels valueForKey:@"_colorPicker"];
    NSCAssert([picker isKindOfClass:UIColorPickerViewController.class] && picker.supportsAlpha,
              @"Swatch opens the native color picker");
    NSDictionary *beforeColor = request;
    UIColor *chosen = [UIColor colorWithRed:0.2 green:0.4 blue:0.6 alpha:0.3];
    [picker.delegate colorPickerViewController:picker didSelectColor:chosen continuously:YES];
    NSCAssert(request == beforeColor, @"Continuous color preview does not flood engine or undo");
    [picker.delegate colorPickerViewController:picker didSelectColor:chosen continuously:NO];
    NSCAssert([setting isEqual:@"native_properties_edit"] && [request[@"key"] isEqual:@"object.color"] &&
              [request[@"value"] count] == 4 && fabs([request[@"value"][3] doubleValue] - 0.3) < 1e-5,
              @"Completed color gesture commits all components atomically");
    [panels performSelector:NSSelectorFromString(@"closeColorPicker")];
    NSMutableDictionary *sliderModel = [[acknowledged mutableCopy] autorelease];
    sliderModel[@"ack"] = @3;
    NSMutableArray *sliderProperties = [NSMutableArray array];
    for (NSDictionary *spec in @[
      @{@"key": @"factor", @"type": @"FLOAT", @"value": @0.25, @"min": @0, @"max": @1, @"scale": @"LINEAR"},
      @{@"key": @"count", @"type": @"INT", @"value": @3, @"min": @1, @"max": @9, @"scale": @"LINEAR"},
      @{@"key": @"angle", @"type": @"FLOAT", @"value": @(M_PI / 2), @"min": @0, @"max": @(M_PI), @"scale": @"LINEAR", @"angle": @YES},
      @{@"key": @"log", @"type": @"FLOAT", @"value": @10, @"min": @1, @"max": @1000, @"scale": @"LOG"},
      @{@"key": @"cubic", @"type": @"FLOAT", @"value": @0.125, @"min": @0, @"max": @1, @"scale": @"CUBIC"},
    ]) {
      NSMutableDictionary *descriptor = [[spec mutableCopy] autorelease];
      descriptor[@"label"] = spec[@"key"];
      descriptor[@"editable"] = @YES;
      descriptor[@"native"] = @YES;
      descriptor[@"slider"] = @{@"min": spec[@"min"], @"max": spec[@"max"], @"scale": spec[@"scale"]};
      [sliderProperties addObject:descriptor];
    }
    sliderModel[@"inspector"] = @{@"id": @"sliders", @"name": @"Properties",
      @"sections": @[@{@"id": @"sliders", @"title": @"Sliders", @"open": @YES, @"properties": sliderProperties}]};
    [panels updateSnapshot:sliderModel];
    NSDictionary *sliders = [panels valueForKey:@"_sliders"];
    UISlider *slider = sliders[@"factor"];
    NSCAssert([slider isKindOfClass:UISlider.class] && fabs(slider.value - 0.25) < 1e-6,
              @"Blender sliders use native UISlider controls");
    NSDictionary *beforeSlide = request;
    [panels performSelector:NSSelectorFromString(@"beginSlider:") withObject:slider];
    slider.value = 0.7;
    [panels performSelector:NSSelectorFromString(@"changeSlider:") withObject:slider];
    NSCAssert(request == beforeSlide, @"Dragging previews locally without intermediate undo steps");
    UITextField *factorField = [panels valueForKey:@"_controls"][@"factor"];
    NSCAssert(fabs([[formatter numberFromString:factorField.text] doubleValue] - 0.7) < 1e-9,
              @"Slider feedback hides floating-point representation noise");
    [panels performSelector:NSSelectorFromString(@"finishSlider:") withObject:slider];
    NSCAssert([request[@"key"] isEqual:@"factor"] && fabs([request[@"value"] doubleValue] - 0.7) < 1e-6,
              @"Releasing slider queues one property edit");
    beforeSlide = request;
    [panels performSelector:NSSelectorFromString(@"beginSlider:") withObject:slider];
    slider.value = 0.1;
    [panels performSelector:NSSelectorFromString(@"changeSlider:") withObject:slider];
    [panels performSelector:NSSelectorFromString(@"cancelSlider:") withObject:slider];
    NSCAssert(request == beforeSlide && fabs(slider.value - 0.25) < 1e-6, @"Cancel restores the snapshot without an edit");
    [panels performSelector:NSSelectorFromString(@"beginSlider:") withObject:slider];
    [panels performSelector:NSSelectorFromString(@"finishSlider:") withObject:slider];
    NSCAssert(request == beforeSlide, @"Touching a slider without dragging does not create an edit");
    [panels performSelector:NSSelectorFromString(@"beginSlider:") withObject:slider];
    slider.value = 0.25;
    [panels performSelector:NSSelectorFromString(@"changeSlider:") withObject:slider];
    [panels performSelector:NSSelectorFromString(@"finishSlider:") withObject:slider];
    NSCAssert(request != beforeSlide && [request[@"value"] doubleValue] == 0.25,
              @"A rapid return to the old value is queued even before the preceding edit is acknowledged");
    UISlider *integerSlider = sliders[@"count"];
    integerSlider.value = 0.6;
    [panels performSelector:NSSelectorFromString(@"changeSlider:") withObject:integerSlider];
    NSCAssert([request[@"value"] intValue] == 6, @"Integer slider snaps to whole values");
    NSCAssert(fabs(((UISlider *)sliders[@"log"]).value - 1.0 / 3) < 1e-6 &&
              fabs(((UISlider *)sliders[@"cubic"]).value - 0.5) < 1e-6,
              @"Logarithmic and cubic ranges match Blender");
    UISlider *angleSlider = sliders[@"angle"];
    angleSlider.value = 0.25;
    [panels performSelector:NSSelectorFromString(@"changeSlider:") withObject:angleSlider];
    NSCAssert(fabs([request[@"value"] doubleValue] - M_PI / 4) < 1e-6,
              @"Native angle slider commits radians");
    UITextField *angleField = [panels valueForKey:@"_controls"][@"angle"];
    NSCAssert([[formatter numberFromString:angleField.text] doubleValue] == 45,
              @"Angle numeric field displays degrees");
    NSMutableDictionary *lockModel = [[acknowledged mutableCopy] autorelease];
    lockModel[@"ack"] = [panels valueForKey:@"_sequence"];
    NSMutableDictionary *lockProperty = [@{@"key": @"location-x", @"property": @"location", @"index": @0,
      @"label": @"X", @"group": @"Location", @"type": @"FLOAT", @"value": @0, @"editable": @YES,
      @"lock": @{@"key": @"lock-location-x", @"property": @"lock_location", @"index": @0,
                  @"type": @"BOOLEAN", @"native": @YES, @"editable": @YES, @"value": @NO}} mutableCopy];
    lockModel[@"inspector"] = @{@"id": @"locked-object", @"name": @"Properties",
      @"sections": @[@{@"id": @"transform", @"title": @"Transform", @"open": @YES, @"properties": @[lockProperty]}]};
    [panels updateSnapshot:lockModel];
    UITextField *lockField = [panels valueForKey:@"_controls"][@"location-x"];
    UIContextMenuInteraction *interaction = nil;
    for (id item in lockField.superview.interactions) {
      if ([item isKindOfClass:UIContextMenuInteraction.class]) { interaction = item; }
    }
    NSCAssert(interaction && [interaction.delegate contextMenuInteraction:interaction configurationForMenuAtLocation:CGPointZero],
              @"Corresponding property row supports a native long-press context menu");
    UIMenu *lockMenu = [lockField.delegate textField:lockField editMenuForCharactersInRanges:@[] suggestedActions:@[]];
    UIAction *lockAction = (UIAction *)lockMenu.children.firstObject;
    NSCAssert([lockAction.title isEqual:@"Lock"] && [[panels valueForKey:@"_controls"] count] == 1,
              @"Lock appears in the menu, without a separate lock toggle");
    [lockAction performWithSender:lockField target:nil];
    NSCAssert([request[@"key"] isEqual:@"lock-location-x"] && [request[@"value"] boolValue],
              @"Context menu changes the exact axis lock through the engine queue");
    lockProperty[@"lock"] = @{@"key": @"lock-location-x", @"property": @"lock_location", @"index": @0,
                                @"type": @"BOOLEAN", @"native": @YES, @"editable": @YES, @"value": @YES};
    lockModel[@"ack"] = [panels valueForKey:@"_sequence"];
    [panels updateSnapshot:lockModel];
    NSCAssert([panels valueForKey:@"_controls"][@"location-x"] == lockField &&
              [[panels valueForKey:@"_lockIndicators"][@"location-x"] alpha] == 1,
              @"Lock indicator updates without rebuilding controls");
    lockMenu = [lockField.delegate textField:lockField editMenuForCharactersInRanges:@[] suggestedActions:@[]];
    UIAction *unlockAction = (UIAction *)lockMenu.children.firstObject;
    NSCAssert([unlockAction.title isEqual:@"Unlock"], @"Locked properties offer Unlock");
    [unlockAction performWithSender:lockField target:nil];
    NSCAssert(![request[@"value"] boolValue], @"Unlock queues the inverse lock value");
    lockProperty[@"native"] = @YES;
    lockProperty[@"animation"] = @[@{@"id": @"insert_single", @"title": @"Insert Single Keyframe", @"icon": @"KEY_HLT"}];
    lockModel[@"ack"] = [panels valueForKey:@"_sequence"];
    [panels updateSnapshot:lockModel];
    UITextField *keyField = [panels valueForKey:@"_controls"][@"location-x"];
    UIMenu *keyMenu = [keyField.delegate textField:keyField editMenuForCharactersInRanges:@[] suggestedActions:@[]];
    NSCAssert(keyMenu.children.count == 2, @"Native property menu combines lock and animation actions");
    UIAction *insertAction = (UIAction *)keyMenu.children.lastObject;
    [insertAction performWithSender:keyField target:nil];
    NSCAssert([setting isEqual:@"native_properties_edit"] &&
              [request[@"animation_action"] isEqual:@"insert_single"] &&
              [request[@"key"] isEqual:@"location-x"], @"Keying queues the exact native property and scope");
    lockProperty[@"animation"] = @[
      @{@"id": @"insert_single", @"title": @"Replace Single Keyframe", @"icon": @"KEY_HLT"},
      @{@"id": @"delete_single", @"title": @"Delete Single Keyframe", @"icon": @"KEY_DEHLT"}];
    lockModel[@"ack"] = [panels valueForKey:@"_sequence"];
    [panels updateSnapshot:lockModel];
    NSCAssert([panels valueForKey:@"_controls"][@"location-x"] == keyField,
              @"Keyframe changes do not rebuild the property controls");
    keyMenu = [keyField.delegate textField:keyField editMenuForCharactersInRanges:@[] suggestedActions:@[]];
    NSCAssert(keyMenu.children.count == 3 && [keyMenu.children[1].title isEqual:@"Replace Single Keyframe"],
              @"Native menu reflects the latest Blender animation state");
    UIAction *deleteAction = (UIAction *)keyMenu.children.lastObject;
    lockProperty[@"animation"] = @[@{@"id": @"insert_single", @"title": @"Insert Single Keyframe", @"icon": @"KEY_HLT"}];
    [panels updateSnapshot:lockModel];
    NSDictionary *beforeUnavailable = request;
    [deleteAction performWithSender:keyField target:nil];
    NSCAssert(request == beforeUnavailable, @"No-longer-available keyframe actions are ignored");
    NSDictionary *beforeStaleMenu = request;
    lockModel[@"ack"] = [panels valueForKey:@"_sequence"];
    lockModel[@"inspector"] = @{@"id": @"other-object", @"name": @"Properties", @"sections": @[]};
    [panels updateSnapshot:lockModel];
    [lockAction performWithSender:nil target:nil];
    NSCAssert(request == beforeStaleMenu, @"A stale menu cannot change the newly selected object");
    [insertAction performWithSender:nil target:nil];
    NSCAssert(request == beforeStaleMenu, @"A stale animation menu cannot key a different object");
    NSArray *scalarAnimation = @[@{@"id": @"insert_all", @"title": @"Insert Keyframe", @"icon": @"KEY_HLT"}];
    lockModel[@"inspector"] = @{@"id": @"animated-object", @"name": @"Properties",
      @"sections": @[@{@"id": @"animated", @"title": @"Animated", @"open": @YES, @"properties": @[
        @{@"key": @"amount", @"label": @"Amount", @"type": @"FLOAT", @"value": @1, @"editable": @YES, @"animation": scalarAnimation},
        @{@"key": @"enabled", @"label": @"Enabled", @"type": @"BOOLEAN", @"value": @YES, @"header": @YES, @"editable": @YES, @"animation": scalarAnimation},
        @{@"key": @"color", @"label": @"Color", @"type": @"COLOR", @"value": @[@1, @0, @0], @"editable": @YES, @"animation": scalarAnimation}]}]};
    [panels updateSnapshot:lockModel];
    for (NSString *propertyKey in @[@"amount", @"enabled", @"color"]) {
      UIControl *control = [panels valueForKey:@"_controls"][propertyKey];
      UIView *source = [propertyKey isEqual:@"enabled"] ? control : control.superview;
      UIContextMenuInteraction *menuInteraction = nil;
      for (id item in source.interactions) {
        if ([item isKindOfClass:UIContextMenuInteraction.class]) { menuInteraction = item; }
      }
      NSCAssert(menuInteraction && [menuInteraction.delegate contextMenuInteraction:menuInteraction configurationForMenuAtLocation:CGPointZero],
                @"Animation-only fields, header switches and color swatches support native context menus");
    }
    NSMutableDictionary *mode = [@{@"key": @"rotation-mode", @"label": @"Rotation Mode", @"type": @"ENUM",
      @"native": @YES, @"editable": @YES, @"value": @"XYZ", @"options": @[
        @{@"value": @"XYZ", @"label": @"XYZ Euler", @"checked": @YES},
        @{@"value": @"ZYX", @"label": @"ZYX Euler", @"checked": @NO}]} mutableCopy];
    NSDictionary *rotation = @{@"key": @"rotation-x", @"label": @"X", @"group": @"Rotation",
      @"property": @"rotation_euler", @"type": @"FLOAT", @"value": @0, @"editable": @YES, @"group_menu": mode};
    lockModel[@"inspector"] = @{@"id": @"rotation-object", @"name": @"Properties", @"sections": @[
      @{@"id": @"transform", @"title": @"Transform", @"open": @YES, @"properties": @[rotation]}]};
    [panels updateSnapshot:lockModel];
    UILabel *rotationHeading = [panels valueForKey:@"_groupHeadings"][@"rotation-x"];
    NSCAssert([rotationHeading.text isEqual:@"Rotation"] && rotationHeading.userInteractionEnabled,
              @"The Rotation heading is a native long-press target");
    UIContextMenuInteraction *headingInteraction = (UIContextMenuInteraction *)rotationHeading.interactions.firstObject;
    NSCAssert([headingInteraction.delegate contextMenuInteraction:headingInteraction configurationForMenuAtLocation:CGPointZero],
              @"Rotation heading provides a native context menu");
    NSArray *modeActions = [panels groupMenuActionsForProperty:rotation];
    NSCAssert(((UIAction *)modeActions[0]).state == UIMenuElementStateOn &&
              ((UIAction *)modeActions[1]).state == UIMenuElementStateOff, @"Current rotation mode is checked");
    UIAction *changeMode = modeActions[1];
    [changeMode performWithSender:rotationHeading target:nil];
    NSCAssert([request[@"key"] isEqual:@"rotation-mode"] && [request[@"value"] isEqual:@"ZYX"] &&
              [setting isEqual:@"native_properties_edit"], @"Heading options edit the real enum through the engine queue");
    mode[@"value"] = @"ZYX";
    mode[@"options"] = @[@{@"value": @"XYZ", @"label": @"XYZ Euler", @"checked": @NO},
                          @{@"value": @"ZYX", @"label": @"ZYX Euler", @"checked": @YES}];
    lockModel[@"ack"] = [panels valueForKey:@"_sequence"];
    [panels updateSnapshot:lockModel];
    NSCAssert([panels valueForKey:@"_groupHeadings"][@"rotation-x"] == rotationHeading &&
              [rotationHeading.accessibilityValue isEqual:@"ZYX Euler"], @"Euler-order changes refresh without rebuilding the heading");
    modeActions = [panels groupMenuActionsForProperty:rotation];
    NSCAssert(((UIAction *)modeActions[1]).state == UIMenuElementStateOn, @"Mode checkmark follows the latest snapshot");
    NSDictionary *beforeModeRepeat = request;
    [changeMode performWithSender:rotationHeading target:nil];
    NSCAssert(request == beforeModeRepeat, @"Selecting the current mode does not add an undo step");
    UIAction *oldMode = modeActions[0];
    lockModel[@"inspector"] = @{@"id": @"another-object", @"name": @"Properties", @"sections": @[]};
    [panels updateSnapshot:lockModel];
    [oldMode performWithSender:nil target:nil];
    NSCAssert(request == beforeModeRepeat, @"A stale heading menu cannot change another object's mode");
    [mode release];
    NSDictionary *fastA = @{@"key": @"fast.a", @"label": @"Amount", @"type": @"FLOAT", @"value": @1, @"editable": @YES};
    NSDictionary *fastB = @{@"key": @"fast.b", @"label": @"Count", @"type": @"INT", @"value": @2, @"editable": @YES};
    NSMutableDictionary *fastInspector = [@{@"id": @"fast-object", @"name": @"Properties", @"tab": @"FAST",
      @"tabs": @[@{@"id": @"FAST", @"title": @"Fast", @"icon": @"OBJECT_DATA"}],
      @"sections": @[@{@"id": @"a", @"title": @"First", @"open": @YES, @"properties": @[fastA]},
                     @{@"id": @"b", @"title": @"Second", @"open": @NO, @"properties": @[]}]} mutableCopy];
    lockModel[@"inspector"] = [[fastInspector copy] autorelease];
    [panels updateSnapshot:lockModel];
    UITextField *fastField = [panels valueForKey:@"_controls"][@"fast.a"];
    UIButton *secondHeader = (UIButton *)[form.delegate tableView:form viewForHeaderInSection:1];
    [secondHeader sendActionsForControlEvents:UIControlEventTouchUpInside];
    fastInspector[@"sections"] = @[@{@"id": @"a", @"title": @"First", @"open": @YES, @"properties": @[fastA]},
                                    @{@"id": @"b", @"title": @"Second", @"open": @YES, @"properties": @[fastB]}];
    lockModel[@"inspector"] = [[fastInspector copy] autorelease];
    [panels updateSnapshot:lockModel];
    NSCAssert([panels valueForKey:@"_controls"][@"fast.a"] == fastField && [form numberOfRowsInSection:1] == 1,
              @"Opening another section preserves existing controls");
    UIButton *firstHeader = (UIButton *)[form.delegate tableView:form viewForHeaderInSection:0];
    [firstHeader sendActionsForControlEvents:UIControlEventTouchUpInside];
    NSCAssert([form numberOfRowsInSection:0] == 0, @"Collapse happens locally without an engine round trip");
    fastInspector[@"sections"] = @[@{@"id": @"a", @"title": @"First", @"open": @NO, @"properties": @[]},
                                    @{@"id": @"b", @"title": @"Second", @"open": @YES, @"properties": @[fastB]}];
    lockModel[@"inspector"] = [[fastInspector copy] autorelease];
    [panels updateSnapshot:lockModel];
    [firstHeader sendActionsForControlEvents:UIControlEventTouchUpInside];
    NSCAssert([form numberOfRowsInSection:0] == 1 && [panels valueForKey:@"_controls"][@"fast.a"] == fastField,
              @"Cached sections reopen immediately with the same controls");
    NSMutableDictionary *disabledA = [[fastA mutableCopy] autorelease];
    disabledA[@"editable"] = @NO;
    disabledA[@"value"] = @4;
    fastInspector[@"sections"] = @[@{@"id": @"a", @"title": @"First", @"open": @YES, @"properties": @[disabledA]},
                                    @{@"id": @"b", @"title": @"Second", @"open": @YES, @"properties": @[fastB]}];
    lockModel[@"inspector"] = [[fastInspector copy] autorelease];
    [panels updateSnapshot:lockModel];
    NSCAssert([panels valueForKey:@"_controls"][@"fast.a"] == fastField && !fastField.enabled,
              @"Value and availability changes update controls in place");
    [UIView setAnimationsEnabled:YES];
    for (int i = 0; i < 10; i++) {
      [firstHeader sendActionsForControlEvents:UIControlEventTouchUpInside];
    }
    NSCAssert([form numberOfRowsInSection:0] == 1, @"Rapid animated toggles leave consistent table state");
    [UIView setAnimationsEnabled:NO];
    [fastInspector release];
    [lockProperty release];
    [panels layoutInRect:CGRectMake(0, 60, 390, 600)];
    NSCAssert(!(panels.inspectorButton.selected && panels.hierarchyButton.selected), @"Phone panels do not overlap");
    tap(panels.hierarchyButton);
    NSCAssert(panels.hierarchyButton.selected && !panels.inspectorButton.selected, @"Phone switches floating panel");
    [panels dismiss];
    NSCAssert(!panels.hierarchyButton.selected && !panels.inspectorButton.selected, @"Temporary panels dismiss");
    testNumberScrubbing(controller.view);
    testHierarchySwipes(controller.view);
    testShadingPopover(controller);
    NSLog(@"PASS: floating scene panels, search, selection, native fields, async ownership, responsive layout");
  }
}
