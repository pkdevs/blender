/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "GHOST_IOSMessageQueue.hh"
#include "GHOST_IOSSurface.hh"
#import <QuartzCore/QuartzCore.h>
#import <UIKit/UIKit.h>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <thread>

struct bContext {};
static bContext context;
static bContext *C;
static int argc;
static const char **argv;
static std::atomic<bool> engine_active{true}, engine_wakeup{false}, stopped{false};
static std::condition_variable engine_condition;
static std::mutex engine_wait_mutex;
/* Queues wake the engine as in `GHOST_SystemIOS.mm`. */
void GHOST_ios_mailbox_published()
{
  engine_wakeup = true;
  engine_condition.notify_one();
}
static GHOST_IOSMessageQueue<std::function<void()>> engine_requests;
static GHOST_IOSMessageQueue<int> commands;
static GHOST_IOSLatest<int> snapshots;
static std::atomic<int> iterations{0}, consumed{0}, stage{0};
static std::shared_ptr<GHOST_IOSSurface> surface;
static id<MTLCommandQueue> queue;
static id<MTLTexture> texture;
static std::atomic<bool> stallRequested{false}, stalled{false};
static bool pendingFrame;
static std::atomic<bool> refuseFirstDrawable{true};
static std::atomic<int> acquiredDrawables{0};

struct TestWindow {
  void flushDeferredSwapBuffers() {}
  void resignKeyWindow() {}
  void makeKeyWindow() {}
};
struct GHOST_SystemIOS {
  TestWindow *current_active_window_ = nullptr, *next_active_window_ = nullptr;
  void handleOpenDocumentRequest(NSString *) {}
};
static GHOST_SystemIOS ghost_system;
struct GHOST_ISystem {
  static GHOST_SystemIOS *getSystem()
  {
    return &ghost_system;
  }
};

static void paint(double r, double g, double b)
{
  assert(!NSThread.isMainThread);
  id<MTLCommandBuffer> buffer = [queue commandBuffer];
  MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = texture;
  pass.colorAttachments[0].loadAction = MTLLoadActionClear;
  pass.colorAttachments[0].clearColor = MTLClearColorMake(r, g, b, 1);
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  id<MTLRenderCommandEncoder> encoder = [buffer renderCommandEncoderWithDescriptor:pass];
  [encoder endEncoding];
  [buffer commit];
}

static int main_ios_callback(int, const char **)
{
  assert(!NSThread.isMainThread);
  C = &context;
  queue = [surface->device() newCommandQueue];
  MTLTextureDescriptor *desc = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                   width:128
                                  height:128
                               mipmapped:NO];
  desc.storageMode = MTLStorageModePrivate;
  desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
  texture = [surface->device() newTextureWithDescriptor:desc];
  paint(0.05, 0.65, 0.15);
  surface->publish(queue, texture);
  snapshots.publish(1);
  return 0;
}

static void WM_apple_ios_ui_flush_commands(bContext *)
{
  assert(!NSThread.isMainThread);
  for (int value : commands.take()) {
    assert(value == consumed.fetch_add(1));
  }
}

static void WM_main_loop_body(bContext *)
{
  assert(!NSThread.isMainThread);
  iterations++;
  if (stallRequested.exchange(false)) {
    id<MTLSharedEvent> gate = [surface->device() newSharedEvent];
    id<MTLCommandBuffer> blocked = [queue commandBuffer];
    [blocked encodeWaitForEvent:gate value:1];
    [blocked commit];
    /* A mutable, incomplete frame must never replace the published green frame. */
    paint(0.9, 0.0, 0.9);
    int accepted = 0;
    for (int i = 0; i < 20; i++) {
      accepted += surface->publish(queue, texture);
    }
    assert(accepted > 0 && accepted <= 3);
    stalled = true;
    std::this_thread::sleep_for(std::chrono::seconds(6));
    gate.signaledValue = 1;
    [gate release];
    paint(0.05, 0.15, 0.75);
    pendingFrame = true;
    snapshots.publish(2);
    stage = 2;
    stalled = false;
  }
  if (pendingFrame) {
    pendingFrame = !surface->publish(queue, texture);
  }
}

// ENGINE_LOOP

@interface TestMetalLayer : CAMetalLayer
@end
@implementation TestMetalLayer
- (id<CAMetalDrawable>)nextDrawable
{
  assert(!NSThread.isMainThread);
  if (refuseFirstDrawable.exchange(false)) {
    assert(!surface->hasPresentedFrame());
    return nil;
  }
  id<CAMetalDrawable> drawable = [super nextDrawable];
  if (drawable) {
    acquiredDrawables++;
  }
  return drawable;
}
@end

@interface Viewport : UIView
@end
@implementation Viewport
+ (Class)layerClass
{
  return TestMetalLayer.class;
}
@end

static void phase(NSString *name)
{
  NSString *documents =
      NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
  [name writeToFile:[documents stringByAppendingPathComponent:@"phase"]
         atomically:YES
           encoding:NSUTF8StringEncoding
              error:nil];
}

struct LoadingWindow {
  std::shared_ptr<GHOST_IOSSurface> surface;
};
static LoadingWindow loadingWindow;

@interface LoadingController : UIViewController {
 @public
  UIActivityIndicatorView *_viewport_loading_indicator;
  LoadingWindow *_ghost_window;
}
@end
@implementation LoadingController
// LOADING_INDICATOR
@end

@interface ResponsiveScene : UIResponder <UIWindowSceneDelegate>
{
  UIWindow *_window;
  Viewport *_viewport;
  UIScrollView *_scroll;
  UIButton *_button;
  UIView *_animated;
  CADisplayLink *_link;
  int _ticks, _actions, _snapshot, _ticksAtStall, _stallTicks;
  BOOL _observedStall;
  NSTimeInterval _stallStart;
  NSTimeInterval _previousStallTick, _maximumStallGap;
  CGFloat _maxMovement;
}
@end

@implementation ResponsiveScene
- (void)scene:(UIScene *)scene
    willConnectToSession:(UISceneSession *)session
                 options:(UISceneConnectionOptions *)options
{
  _window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
  LoadingController *controller = [[[LoadingController alloc] init] autorelease];
  _window.rootViewController = controller;
  UIView *root = _window.rootViewController.view;
  root.backgroundColor = UIColor.systemBackgroundColor;
  _viewport = [[Viewport alloc] initWithFrame:root.bounds];
  _viewport.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  [root addSubview:_viewport];
  CAMetalLayer *layer = (CAMetalLayer *)_viewport.layer;
  layer.device = MTLCreateSystemDefaultDevice();
  layer.pixelFormat = MTLPixelFormatRGBA16Float;
  layer.drawableSize = CGSizeMake(800, 600);
  layer.allowsNextDrawableTimeout = YES;
  surface = std::make_shared<GHOST_IOSSurface>(layer);
  assert(!surface->hasPresentedFrame());
  [controller installViewportLoadingIndicator];
  [controller updateViewportLoadingIndicator]; /* The surface is attached after view loading. */
  assert(controller->_viewport_loading_indicator.isAnimating);
  assert(!controller->_viewport_loading_indicator.userInteractionEnabled);
  loadingWindow.surface = surface;
  controller->_ghost_window = &loadingWindow;
  [controller updateViewportLoadingIndicator];
  assert(controller->_viewport_loading_indicator.isAnimating);
  const CGRect originalBounds = root.bounds;
  for (NSValue *size in @[[NSValue valueWithCGSize:CGSizeMake(466, 900)],
                         [NSValue valueWithCGSize:CGSizeMake(1194, 834)]]) {
    root.bounds = (CGRect){CGPointZero, size.CGSizeValue};
    [root layoutIfNeeded];
    assert(fabs(controller->_viewport_loading_indicator.center.x - CGRectGetMidX(root.bounds)) < 1);
    assert(fabs(controller->_viewport_loading_indicator.center.y - CGRectGetMidY(root.bounds)) < 1);
  }
  root.bounds = originalBounds;
  surface->resize(layer.drawableSize);
  surface->setVisible(true);
  _scroll = [[UIScrollView alloc] initWithFrame:CGRectMake(30, 100, 200, 400)];
  _scroll.backgroundColor = UIColor.secondarySystemBackgroundColor;
  _scroll.contentSize = CGSizeMake(200, 2000);
  for (int i = 0; i < 40; i++) {
    UILabel *label = [[[UILabel alloc] initWithFrame:CGRectMake(10, i * 50, 180, 50)] autorelease];
    label.text = [NSString stringWithFormat:@"Tool %d", i + 1];
    [_scroll addSubview:label];
  }
  [root addSubview:_scroll];
  _button = [UIButton buttonWithType:UIButtonTypeSystem];
  _button.frame = CGRectMake(30, 40, 200, 44);
  _button.configuration = UIButtonConfiguration.prominentGlassButtonConfiguration;
  [_button setTitle:@"Queue Command" forState:UIControlStateNormal];
  [_button addTarget:self action:@selector(command:) forControlEvents:UIControlEventTouchUpInside];
  [root addSubview:_button];
  _animated = [[UIView alloc] initWithFrame:CGRectMake(250, 100, 24, 24)];
  _animated.backgroundColor = UIColor.whiteColor;
  [root addSubview:_animated];
  [_window makeKeyAndVisible];
  NSThread *thread = [[NSThread alloc] initWithBlock:^{
    run_engine();
    [texture release];
    [queue release];
    stopped = true;
  }];
  thread.name = @"Blender Engine Test";
  thread.stackSize = 8 * 1024 * 1024;
  [thread start];
  [thread release];
  _link = [CADisplayLink displayLinkWithTarget:self selector:@selector(tick:)];
  [_link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
    assert(acquiredDrawables > 0); /* Retry must work without another engine render. */
    assert(surface->hasPresentedFrame());
    [controller updateViewportLoadingIndicator];
    assert(!controller->_viewport_loading_indicator.isAnimating);
    assert(controller->_viewport_loading_indicator.hidden);
    assert(controller->_viewport_loading_indicator.accessibilityElementsHidden);
    phase(@"before");
    stallRequested = true;
  });
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 9 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
    engine_active = false;
    surface->setVisible(false);
    dispatch_after(
        dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_MSEC * 200), dispatch_get_main_queue(), ^{
          const int paused = iterations.load();
          dispatch_after(
              dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_MSEC * 200), dispatch_get_main_queue(), ^{
                assert(iterations == paused);
                engine_active = true;
                surface->setVisible(true);
                engine_condition.notify_one();
              });
        });
  });
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 11 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
    assert(stage == 2 && !stalled && consumed == 20 && _snapshot == 2);
    assert(_ticks - _ticksAtStall > 60 && _maxMovement > 40);
    assert(_stallTicks >= 120 && _maximumStallGap < 0.25);
    NSLog(
        @"PASS: responsive UIKit; %d stall ticks, %.1f ms maximum gap, %d queued actions, "
        @"animation %.1f",
        _stallTicks,
        _maximumStallGap * 1000,
        _actions,
        double(_maxMovement));
    engine_requests.push([] { C = nullptr; });
    engine_wakeup = true;
    engine_condition.notify_one();
    [self finishWhenStopped];
  });
}

- (void)command:(UIButton *)sender
{
  assert(NSThread.isMainThread);
  commands.push(_actions++);
  sender.selected = !sender.selected;
}

- (void)tick:(CADisplayLink *)link
{
  assert(NSThread.isMainThread);
  _ticks++;
  LoadingController *controller = (LoadingController *)_window.rootViewController;
  [controller updateViewportLoadingIndicator];
  if (stalled || stage == 2) {
    assert(surface->hasPresentedFrame());
    assert(!controller->_viewport_loading_indicator.isAnimating);
    assert(controller->_viewport_loading_indicator.hidden);
  }
  snapshots.take(_snapshot);
  if (stalled && !_observedStall) {
    _observedStall = YES;
    _ticksAtStall = _ticks;
    _stallStart = link.timestamp;
    assert(_snapshot == 1 && consumed == 0);
    for (int i = 0; i < 20; i++) {
      [_button sendActionsForControlEvents:UIControlEventTouchUpInside];
    }
    [_scroll setContentOffset:CGPointMake(0, 600) animated:YES];
    [UIView animateWithDuration:2
                     animations:^{
                       _animated.frame = CGRectMake(250, 300, 24, 24);
                     }];
    /* Resizing must be able to present the old frame without any engine work. */
    ((CAMetalLayer *)_viewport.layer).drawableSize = CGSizeMake(640, 480);
    surface->resize(CGSizeMake(640, 480));
  }
  if (stalled) {
    _stallTicks++;
    if (_previousStallTick) {
      _maximumStallGap = MAX(_maximumStallGap, link.timestamp - _previousStallTick);
    }
    _previousStallTick = link.timestamp;
    assert(consumed == 0 && _snapshot == 1);
    _maxMovement = MAX(_maxMovement, _animated.layer.presentationLayer.frame.origin.y - 100);
    if (link.timestamp - _stallStart > 2) {
      assert(_scroll.contentOffset.y == 600);
      phase(@"stall");
    }
  }
  if (stage == 2 && !stalled) {
    phase(@"after");
  }
}

- (void)finishWhenStopped
{
  if (!stopped) {
    dispatch_after(
        dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_MSEC * 20), dispatch_get_main_queue(), ^{
          [self finishWhenStopped];
        });
    return;
  }
  [_link invalidate];
  surface->close();
  loadingWindow.surface.reset();
  surface.reset();
  exit(0);
}
@end

@interface ResponsiveApp : UIResponder <UIApplicationDelegate>
@end
@implementation ResponsiveApp
@end
int main(int argc, char **argv)
{
  @autoreleasepool {
    return UIApplicationMain(argc, argv, nil, NSStringFromClass(ResponsiveApp.class));
  }
}
