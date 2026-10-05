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
    [panels layoutInRect:CGRectMake(0, 60, 390, 600)];
    NSCAssert(!(panels.inspectorButton.selected && panels.hierarchyButton.selected), @"Phone panels do not overlap");
    tap(panels.hierarchyButton);
    NSCAssert(panels.hierarchyButton.selected && !panels.inspectorButton.selected, @"Phone switches floating panel");
    [panels dismiss];
    NSCAssert(!panels.hierarchyButton.selected && !panels.inspectorButton.selected, @"Temporary panels dismiss");
    NSLog(@"PASS: floating scene panels, search, selection, native fields, async ownership, responsive layout");
  }
}
