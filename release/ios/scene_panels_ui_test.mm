/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#import "GHOST_IOSScenePanels.hh"

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
    UITableView *tree = [panels valueForKey:@"_tree"];
    NSCAssert([tree.dataSource tableView:tree numberOfRowsInSection:0] == 3, @"Hierarchy rows");
    [tree.delegate tableView:tree didSelectRowAtIndexPath:[NSIndexPath indexPathForRow:0 inSection:0]];
    NSCAssert([tree.dataSource tableView:tree numberOfRowsInSection:0] == 1, @"Collapse collection");
    UISearchBar *search = [panels valueForKey:@"_search"];
    search.text = @"camera";
    [search.delegate searchBar:search textDidChange:search.text];
    NSCAssert([tree.dataSource tableView:tree numberOfRowsInSection:0] == 2, @"Search reveals ancestors");
    [tree.delegate tableView:tree didSelectRowAtIndexPath:[NSIndexPath indexPathForRow:1 inSection:0]];
    NSCAssert([request[@"action"] isEqual:@"select"] && [request[@"id"] isEqual:@"8"], @"Selection queued");
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
    NSDictionary *beforeStaleMenu = request;
    lockModel[@"ack"] = [panels valueForKey:@"_sequence"];
    lockModel[@"inspector"] = @{@"id": @"other-object", @"name": @"Properties", @"sections": @[]};
    [panels updateSnapshot:lockModel];
    [lockAction performWithSender:nil target:nil];
    NSCAssert(request == beforeStaleMenu, @"A stale menu cannot change the newly selected object");
    [lockProperty release];
    [panels layoutInRect:CGRectMake(0, 60, 390, 600)];
    NSCAssert(!(panels.inspectorButton.selected && panels.hierarchyButton.selected), @"Phone panels do not overlap");
    tap(panels.hierarchyButton);
    NSCAssert(panels.hierarchyButton.selected && !panels.inspectorButton.selected, @"Phone switches floating panel");
    [panels dismiss];
    NSCAssert(!panels.hierarchyButton.selected && !panels.inspectorButton.selected, @"Temporary panels dismiss");
    NSLog(@"PASS: floating scene panels, search, selection, native fields, async ownership, responsive layout");
  }
}
