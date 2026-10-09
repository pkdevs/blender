/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#import <UIKit/UIKit.h>

#include "GHOST_IOSDialog.hh"
#include "GHOST_WindowIOS.hh"

@class GHOSTIOSDialogRequest;

/* The sheet for dialogs with options, drawn in SwiftUI (`intern/swift/DialogSheet.swift`). */
@interface UIViewController (GHOSTIOSSwiftDialogSheet)
- (instancetype)initWithTitle:(NSString *)title
                      message:(NSString *)message
                      toggles:(NSArray<NSString *> *)toggles
                     selected:(NSArray<NSNumber *> *)selected
                      actions:(NSArray<NSString *> *)actions
                        roles:(NSArray<NSNumber *> *)roles
                    preferred:(NSInteger)preferred
                       choose:(void (^)(NSInteger action, uint64_t toggles))choose;
@end
static NSMutableArray<GHOSTIOSDialogRequest *> *dialog_queue;
static NSTimer *dialog_timer;

@interface GHOSTIOSDialogController : UIViewController
@property(nonatomic, assign) GHOSTIOSDialogRequest *request;
@end

@interface GHOSTIOSDialogRequest : NSObject <UIAdaptivePresentationControllerDelegate>
{
 @public
  GHOST_IOSDialog model;
  std::shared_ptr<std::atomic<bool>> alive;
}
@property(nonatomic) uint64_t identifier;
@property(nonatomic) GHOST_IOSDialogCompletion completion;
@property(nonatomic, retain) UIWindow *window;
@property(nonatomic, retain) UIViewController *controller;
@property(nonatomic, retain) NSMutableArray<UISwitch *> *switches;
@property(nonatomic) BOOL finished;
/* The options chosen in the SwiftUI sheet, which has no switches here. */
@property(nonatomic) uint64_t chosenToggles;
- (void)finish:(int)action;
- (void)presentFrom:(UIViewController *)presenter;
@end

static NSString *dialogString(const std::string &text)
{
  return [NSString stringWithUTF8String:text.c_str()] ?: @"";
}

static void dialogPump()
{
  GHOSTIOSDialogRequest *request = dialog_queue.firstObject;
  if (!request) {
    [dialog_timer invalidate];
    dialog_timer = nil;
    return;
  }
  if (!request->alive->load()) {
    [request finish:-1];
    return;
  }
  if (request.controller || request.finished) {
    return;
  }
  UIViewController *presenter = request.window.rootViewController;
  while (presenter.presentedViewController) {
    if (presenter.isBeingDismissed || presenter.presentedViewController.isBeingDismissed ||
        [presenter.presentedViewController isKindOfClass:UIAlertController.class])
    {
      return;
    }
    presenter = presenter.presentedViewController;
  }
  if (!presenter.view.window || presenter.isBeingPresented || presenter.isBeingDismissed) {
    return;
  }
  [request presentFrom:presenter];
}

@implementation GHOSTIOSDialogController
- (void)viewDidLoad
{
  [super viewDidLoad];
  self.view.backgroundColor = UIColor.systemBackgroundColor;
  self.view.accessibilityIdentifier = @"blender.nativeDialog";
  GHOSTIOSDialogRequest *request = self.request;
  UIScrollView *scroll = [[[UIScrollView alloc] init] autorelease];
  scroll.translatesAutoresizingMaskIntoConstraints = NO;
  [self.view addSubview:scroll];
  UIStackView *stack = [[[UIStackView alloc] init] autorelease];
  stack.axis = UILayoutConstraintAxisVertical;
  stack.spacing = 20;
  stack.translatesAutoresizingMaskIntoConstraints = NO;
  [scroll addSubview:stack];
  [NSLayoutConstraint activateConstraints:@[
    [scroll.leadingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.leadingAnchor],
    [scroll.trailingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.trailingAnchor],
    [scroll.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor],
    [scroll.bottomAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.bottomAnchor],
    [stack.leadingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.leadingAnchor
                                        constant:24],
    [stack.trailingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.trailingAnchor
                                         constant:-24],
    [stack.topAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.topAnchor constant:24],
    [stack.bottomAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.bottomAnchor
                                       constant:-24],
    [stack.widthAnchor constraintEqualToAnchor:scroll.frameLayoutGuide.widthAnchor constant:-48],
  ]];
  for (int i = 0; i < 2; i++) {
    UILabel *label = [[[UILabel alloc] init] autorelease];
    label.text = dialogString(i == 0 ? request->model.title : request->model.message);
    label.font = [UIFont
        preferredFontForTextStyle:i == 0 ? UIFontTextStyleTitle2 : UIFontTextStyleBody];
    label.adjustsFontForContentSizeCategory = YES;
    label.numberOfLines = 0;
    [stack addArrangedSubview:label];
  }
  for (const GHOST_IOSDialogToggle &option : request->model.toggles) {
    UIStackView *row = [[[UIStackView alloc] init] autorelease];
    row.axis = UILayoutConstraintAxisHorizontal;
    row.alignment = UIStackViewAlignmentCenter;
    row.spacing = 16;
    UILabel *label = [[[UILabel alloc] init] autorelease];
    label.text = dialogString(option.title);
    label.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    label.adjustsFontForContentSizeCategory = YES;
    label.numberOfLines = 0;
    UISwitch *toggle = [[[UISwitch alloc] init] autorelease];
    toggle.on = option.selected;
    toggle.accessibilityLabel = label.text;
    [toggle setContentCompressionResistancePriority:UILayoutPriorityRequired
                                            forAxis:UILayoutConstraintAxisHorizontal];
    [row addArrangedSubview:label];
    [row addArrangedSubview:toggle];
    [request.switches addObject:toggle];
    [stack addArrangedSubview:row];
  }
  for (size_t i = 0; i < request->model.actions.size(); i++) {
    const GHOST_IOSDialogAction &action = request->model.actions[i];
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    UIButtonConfiguration *configuration = int(i) == request->model.preferred_action ?
                                               [UIButtonConfiguration filledButtonConfiguration] :
                                               [UIButtonConfiguration borderedButtonConfiguration];
    configuration.title = dialogString(action.title);
    if (action.role == GHOST_IOSDialogRole::Destructive) {
      configuration.baseForegroundColor = UIColor.systemRedColor;
    }
    button.configuration = configuration;
    button.titleLabel.numberOfLines = 0;
    button.tag = i;
    [button.heightAnchor constraintGreaterThanOrEqualToConstant:44].active = YES;
    [button addTarget:self action:@selector(choose:) forControlEvents:UIControlEventTouchUpInside];
    [stack addArrangedSubview:button];
  }
}

- (void)choose:(UIButton *)sender
{
  [self.request finish:int(sender.tag)];
}
@end

@implementation GHOSTIOSDialogRequest
- (void)dealloc
{
  if ([_controller isKindOfClass:GHOSTIOSDialogController.class]) {
    ((GHOSTIOSDialogController *)_controller).request = nil;
  }
  [_controller release];
  [_window release];
  [_switches release];
  [super dealloc];
}

- (void)finish:(int)action
{
  if (self.finished) {
    return;
  }
  self.finished = YES;
  uint64_t toggles = self.chosenToggles;
  for (NSUInteger i = 0; i < self.switches.count; i++) {
    if (self.switches[i].on) {
      toggles |= uint64_t(1) << i;
    }
  }
  /* Resolve only after dismissal so the next action can present a file picker. */
  void (^complete)(void) = ^{
    GHOST_IOSDialogCompletion callback = self.completion;
    self.completion = nullptr;
    if ([self.controller isKindOfClass:GHOSTIOSDialogController.class]) {
      ((GHOSTIOSDialogController *)self.controller).request = nil;
    }
    self.controller = nil;
    [dialog_queue removeObject:self];
    if (callback) {
      callback(self.identifier, action, toggles);
    }
    dialogPump();
  };
  if (self.controller.presentingViewController) {
    [self.controller dismissViewControllerAnimated:YES completion:complete];
  }
  else {
    [[self retain] autorelease];
    complete();
  }
}

- (void)presentationControllerDidDismiss:(UIPresentationController *)controller
{
  [self finish:-1];
}

- (void)presentFrom:(UIViewController *)presenter
{
  if (model.toggles.empty()) {
    UIAlertController *alert = [UIAlertController
        alertControllerWithTitle:dialogString(model.title)
                         message:dialogString(model.message)
                  preferredStyle:UIAlertControllerStyleAlert];
    alert.view.accessibilityIdentifier = @"blender.nativeDialog";
    for (size_t i = 0; i < model.actions.size(); i++) {
      const GHOST_IOSDialogAction &item = model.actions[i];
      UIAlertActionStyle style = item.role == GHOST_IOSDialogRole::Cancel ?
                                     UIAlertActionStyleCancel :
                                 item.role == GHOST_IOSDialogRole::Destructive ?
                                     UIAlertActionStyleDestructive :
                                     UIAlertActionStyleDefault;
      UIAlertAction *action = [UIAlertAction actionWithTitle:dialogString(item.title)
                                                       style:style
                                                     handler:^(UIAlertAction *) {
                                                       [self finish:int(i)];
                                                     }];
      [alert addAction:action];
      if (int(i) == model.preferred_action) {
        alert.preferredAction = action;
      }
    }
    self.controller = alert;
  }
  else if (Class sheet_class = [NSUserDefaults.standardUserDefaults boolForKey:@"BlenderUIKitInterface"] ?
                                    nil :
                                    NSClassFromString(@"GHOSTIOSSwiftDialogSheet"))
  {
    NSMutableArray *toggles = [NSMutableArray array], *selected = [NSMutableArray array];
    for (const GHOST_IOSDialogToggle &option : model.toggles) {
      [toggles addObject:dialogString(option.title)];
      [selected addObject:@(option.selected)];
    }
    NSMutableArray *actions = [NSMutableArray array], *roles = [NSMutableArray array];
    for (const GHOST_IOSDialogAction &action : model.actions) {
      [actions addObject:dialogString(action.title)];
      [roles addObject:@(action.role == GHOST_IOSDialogRole::Cancel ? 1 :
                         action.role == GHOST_IOSDialogRole::Destructive ? 2 : 0)];
    }
    __block GHOSTIOSDialogRequest *owner = self;
    UIViewController *sheet = [[[sheet_class alloc] initWithTitle:dialogString(model.title)
        message:dialogString(model.message) toggles:toggles selected:selected actions:actions
        roles:roles preferred:model.preferred_action
        choose:^(NSInteger action, uint64_t chosen) {
          owner.chosenToggles = chosen;
          [owner finish:int(action)];
        }] autorelease];
    sheet.modalPresentationStyle = UIModalPresentationFormSheet;
    sheet.preferredContentSize = CGSizeMake(520, 600);
    self.controller = sheet;
  }
  else {
    GHOSTIOSDialogController *sheet = [[[GHOSTIOSDialogController alloc] init] autorelease];
    sheet.request = self;
    sheet.modalPresentationStyle = UIModalPresentationFormSheet;
    sheet.preferredContentSize = CGSizeMake(520, 600);
    self.controller = sheet;
  }
  [presenter presentViewController:self.controller animated:YES completion:nil];
  if (![self.controller isKindOfClass:UIAlertController.class]) {
    self.controller.presentationController.delegate = self;
  }
}
@end

bool GHOST_ios_dialog_begin(void *window,
                            uint64_t identifier,
                            const GHOST_IOSDialog &dialog,
                            GHOST_IOSDialogCompletion completion)
{
  if (!window || dialog.actions.empty() || dialog.toggles.size() > 64) {
    return false;
  }
  GHOST_WindowIOS *ghost = static_cast<GHOST_WindowIOS *>(window);
  UIWindow *native_window = ghost->rootWindow;
  auto token = ghost->input_alive;
  GHOST_IOSDialog owned = dialog;
  dispatch_async(dispatch_get_main_queue(), ^{
    if (!token->load()) {
      if (completion) {
        completion(identifier, -1, 0);
      }
      return;
    }
    if (!dialog_queue) {
      dialog_queue = [[NSMutableArray alloc] init];
    }
    /* Reports can arrive through both the operator popup and global report log. */
    if (!completion) {
      for (GHOSTIOSDialogRequest *request in dialog_queue) {
        if (!request.completion && request.window == native_window &&
            request->model.title == owned.title && request->model.message == owned.message)
        {
          return;
        }
      }
    }
    GHOSTIOSDialogRequest *request = [[[GHOSTIOSDialogRequest alloc] init] autorelease];
    request->model = owned;
    request->alive = token;
    request.identifier = identifier;
    request.completion = completion;
    request.window = native_window;
    request.switches = [NSMutableArray array];
    [dialog_queue addObject:request];
    if (!dialog_timer) {
      dialog_timer = [NSTimer timerWithTimeInterval:0.05
                                            repeats:YES
                                              block:^(NSTimer *) {
                                                dialogPump();
                                              }];
      [NSRunLoop.mainRunLoop addTimer:dialog_timer forMode:NSRunLoopCommonModes];
    }
    dialogPump();
  });
  return true;
}

void GHOST_ios_dialog_cancel(uint64_t identifier)
{
  dispatch_async(dispatch_get_main_queue(), ^{
    for (GHOSTIOSDialogRequest *request in [[dialog_queue copy] autorelease]) {
      if (request.identifier == identifier) {
        request.completion = nullptr;
        [request finish:-1];
        break;
      }
    }
  });
}
