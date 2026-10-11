/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "GHOST_SystemIOS.hh"

#include "GHOST_ContextIOS.hh"
#import "GHOST_IOSPlatform.hh"
#include "GHOST_IOSRuntime.hh"
#include "GHOST_IOSTheme.hh"
#include "GHOST_WindowIOS.hh"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "GHOST_Debug.hh"
#include "GHOST_EventButton.hh"
#include "GHOST_EventCursor.hh"
#include "GHOST_EventDragnDrop.hh"
#include "GHOST_EventString.hh"
#include "GHOST_WindowManager.hh"

#ifdef WITH_INPUT_NDOF
#  include "GHOST_NDOFManagerCocoa.hh"
#endif

#import <MetalKit/MTKView.h>
#import <UIKit/UIKit.h>

#include <sys/sysctl.h>
#include <sys/time.h>

// #define IOS_SYSTEM_LOGGING
#if defined(IOS_SYSTEM_LOGGING)
#  define IOS_SYSTEM_LOG(...) NSLog(__VA_ARGS__)
#else
#  define IOS_SYSTEM_LOG(...)
#endif

int argc = 0;
const char **argv = nullptr;

namespace blender {
struct bContext;
static bContext *C = nullptr;

/* Implemented in wm.cc. */
void WM_main_loop_body(bContext *C);
extern "C" void WM_apple_ios_ui_flush_commands(bContext *C);
extern "C" double WM_apple_ios_engine_idle_seconds(bContext *C);
/* Implemented in wm_files.cc. */
void WM_apple_ios_autosave_now(bContext *C);
}  // namespace blender

#import "GHOST_IOSDocument.hh"
#include "GHOST_IOSDocumentPicker.hh"
#import "GHOST_IOSLauncher.hh"
using blender::C;
using blender::WM_apple_ios_engine_idle_seconds;
using blender::WM_apple_ios_ui_flush_commands;
using blender::WM_main_loop_body;
int main_ios_callback(int argc, const char **argv);

static UIWindowScene *g_window_scene = nil;
static NSMutableArray<UIWindow *> *native_windows; /* UIKit-only window registry. */
static std::atomic<bool> engine_active{false};
static std::atomic<bool> engine_wakeup{false};
static std::condition_variable engine_condition;
static std::mutex engine_wait_mutex;
static GHOST_IOSMessageQueue<std::function<void()>> engine_requests;
/* Auto-save requested by UIKit when the app leaves the foreground; runs even while inactive. */
static std::atomic<bool> engine_autosave_requested{false};
static std::mutex engine_autosave_mutex;
static std::vector<std::function<void()>> engine_autosave_done;
/* Work for documents, which UIKit waits on. Runs while inactive too: iOS saves documents as the
 * app leaves the foreground. */
static GHOST_IOSMessageQueue<std::function<void()>> engine_document_requests;
static std::atomic<bool> engine_document_pending{false};
static std::atomic<bool> app_in_background{false};
/* A render the user started goes on while the app is inactive or in the background. */
static std::atomic<bool> engine_continues_work{false};

/* When the engine's current pass began (seconds on the monotonic clock), 0 between passes. UIKit
 * reads it to tell when Blender is held up, such as by compiling shaders. */
static std::atomic<double> engine_pass_started{0.0};
static std::atomic<int> shader_compilations_finished{0};
static std::atomic<int> shader_compilations_total{0};

static double engine_clock()
{
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

extern "C" void GHOST_ios_shader_compilation_progress(int finished, int total)
{
  shader_compilations_total.store(total);
  shader_compilations_finished.store(finished);
}

double GHOST_ios_engine_busy_seconds()
{
  const double started = engine_pass_started.load();
  return started > 0.0 ? engine_clock() - started : 0.0;
}

bool GHOST_ios_shader_compilation(int *finished, int *total)
{
  *total = shader_compilations_total.load();
  *finished = std::min(shader_compilations_finished.load(), *total);
  return *total > 0;
}

/* The interval between Blender's passes while it draws, a frame of the display. */
static std::atomic<int64_t> engine_frame_microseconds{16667};

/* UIKit is told once about any number of snapshots published before it gets to them. */
static std::atomic<bool> mailbox_notification_pending{false};

void GHOST_ios_mailbox_published()
{
  if (NSThread.isMainThread) {
    GHOST_ios_engine_wake();
    return;
  }
  if (mailbox_notification_pending.exchange(true)) {
    return;
  }
  dispatch_async(dispatch_get_main_queue(), ^{
    mailbox_notification_pending.store(false);
    [NSNotificationCenter.defaultCenter postNotificationName:@"BlenderSnapshotsPublished"
                                                      object:nil];
  });
}

void GHOST_ios_engine_wake()
{
  engine_wakeup.store(true);
  engine_condition.notify_one();
}

static void open_document(NSURL *url)
{
  GHOST_ios_document_open_url(url);
}

void GHOST_ios_engine_perform(std::function<void(blender::bContext *)> work)
{
  engine_document_requests.push([work = std::move(work)] {
    @autoreleasepool {
      work(C);
    }
  });
  engine_document_pending.store(true);
  GHOST_ios_engine_wake();
}

bool GHOST_ios_app_in_background()
{
  return app_in_background.load();
}

extern "C" void GHOST_ios_engine_continue_work(bool continues)
{
  engine_continues_work.store(continues);
  GHOST_ios_engine_wake();
}

/** Ask the engine to write the auto-save now; `done` runs on the engine thread afterwards. */
static void request_autosave(std::function<void()> done)
{
  {
    std::lock_guard lock(engine_autosave_mutex);
    engine_autosave_done.push_back(std::move(done));
  }
  engine_autosave_requested.store(true);
  GHOST_ios_engine_wake();
}

static void run_engine_autosave()
{
  if (!engine_autosave_requested.exchange(false)) {
    return;
  }
  if (C) {
    @autoreleasepool {
      blender::WM_apple_ios_autosave_now(C);
    }
  }
  std::vector<std::function<void()>> done;
  {
    std::lock_guard lock(engine_autosave_mutex);
    done.swap(engine_autosave_done);
  }
  for (std::function<void()> &callback : done) {
    callback();
  }
}

static void run_engine()
{
  @autoreleasepool {
    main_ios_callback(argc, argv);
  }
  auto deadline = std::chrono::steady_clock::now();
  while (C) {
    {
      std::unique_lock lock(engine_wait_mutex);
      if (engine_active.load()) {
        engine_condition.wait_until(lock, deadline, [] { return engine_wakeup.exchange(false); });
      }
      else {
        /* UIKit signals without taking this lock. A bounded wait also covers a
         * notification racing the transition into sleep. */
        engine_condition.wait_for(lock, std::chrono::milliseconds(100), [] {
          return engine_active.load() || engine_autosave_requested.load() ||
                 engine_document_pending.load() || engine_continues_work.load();
        });
      }
    }
    /* Runs in the background too: leaving the foreground is when work must be kept. */
    run_engine_autosave();
    if (engine_document_pending.exchange(false)) {
      for (auto &request : engine_document_requests.take()) {
        request();
      }
    }
    /* A render keeps Blender's loop running in the background, a few times a second, so its job
     * finishes and reports progress; windows aren't drawn there, see #getState. */
    const bool continuing = !engine_active.load() && engine_continues_work.load();
    if (!engine_active.load() && !continuing) {
      continue;
    }
    deadline = std::chrono::steady_clock::now() +
               std::chrono::microseconds(engine_frame_microseconds.load());
    if (continuing) {
      std::unique_lock lock(engine_wait_mutex);
      engine_condition.wait_for(
          lock, std::chrono::milliseconds(100), [] { return engine_active.load(); });
    }
    @autoreleasepool {
      auto *system = static_cast<GHOST_SystemIOS *>(GHOST_ISystem::getSystem());
      for (auto &request : engine_requests.take()) {
        request();
      }
      if (!C) {
        break;
      }
      engine_pass_started.store(engine_clock());
      WM_apple_ios_ui_flush_commands(C);
      WM_main_loop_body(C);
      if (system->current_active_window_) {
        system->current_active_window_->flushDeferredSwapBuffers();
      }
      engine_pass_started.store(0.0);
      /* With nothing to do, sleep until Blender's next timer instead of the next frame. Input,
       * native commands and requests wake the loop at once. */
      if (C) {
        deadline += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(WM_apple_ios_engine_idle_seconds(C)));
      }
      if (system->next_active_window_) {
        if (system->current_active_window_) {
          system->current_active_window_->resignKeyWindow();
        }
        system->next_active_window_->makeKeyWindow();
        system->next_active_window_ = nullptr;
      }
    }
  }
}

UIWindowScene *GHOST_getIOSWindowScene()
{
  GHOST_ASSERT(NSThread.isMainThread, "Scene access belongs to UIKit");
  return g_window_scene;
}

void GHOST_ios_register_window(UIWindow *window)
{
  blenderIOSApplyTheme(window);
  if (!native_windows) {
    native_windows = [[NSMutableArray alloc] init];
  }
  [native_windows addObject:window];
}

void GHOST_ios_unregister_window(UIWindow *window)
{
  id<UIWindowSceneDelegate> delegate = (id<UIWindowSceneDelegate>)g_window_scene.delegate;
  if (delegate.window == window) {
    delegate.window = nil;
  }
  [native_windows removeObject:window];
}

@interface IOSAppDelegate : UIResponder <UIApplicationDelegate>

@property(strong, nonatomic) UIWindow *window;

@end

@implementation IOSAppDelegate

- (void)buildMenuWithBuilder:(id<UIMenuBuilder>)builder
{
  [super buildMenuWithBuilder:builder];
  if (builder.system != UIMenuSystem.mainSystem) {
    return;
  }
  /* UIKit asks the app delegate, not the view controller, for the system menu bar. */
  for (UIWindow *window in g_window_scene.windows) {
    if (window.isKeyWindow) {
      [window.rootViewController buildMenuWithBuilder:builder];
      break;
    }
  }
}

- (BOOL)application:(UIApplication *)application
    didFinishLaunchingWithOptions:(NSDictionary *)launchOptions
{
  UNUSED_VARS(application, launchOptions);
  return YES;
}

- (BOOL)application:(UIApplication *)application
            openURL:(NSURL *)url
            options:(NSDictionary<UIApplicationOpenURLOptionsKey, id> *)options
{
  open_document(url);

  return YES;
}

@end

@interface IOSSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(strong, nonatomic) UIWindow *window;
@end

@implementation IOSSceneDelegate

- (void)scene:(UIScene *)scene
    willConnectToSession:(UISceneSession *)session
                 options:(UISceneConnectionOptions *)connectionOptions
{
  UNUSED_VARS(session);
  if (![scene isKindOfClass:[UIWindowScene class]]) {
    return;
  }

  g_window_scene = (UIWindowScene *)scene;
  /* Draw as often as the display shows frames: 120 times a second with ProMotion. */
#if defined(APPLE_TARGET_VISIONOS)
  const NSInteger frames_per_second = GHOSTIOSMaximumFramesPerSecond();
#else
  const NSInteger frames_per_second = g_window_scene.screen.maximumFramesPerSecond;
#endif
  engine_frame_microseconds.store(1000000 / std::max<NSInteger>(60, frames_per_second));
  for (UIWindow *window in native_windows) {
    window.windowScene = g_window_scene;
  }
  if (native_windows.count && GHOST_ios_document_window_visible()) {
    [native_windows.lastObject makeKeyAndVisible];
  }
  static bool started = false; /* UIKit-only startup state. */
  if (!started) {
    started = true;
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(applicationWillTerminate)
                                               name:UIApplicationWillTerminateNotification
                                             object:nil];
    /* The document launcher shows at once, while Blender starts behind it. Blender's window
     * comes forward once a document is open. */
    self.window = [[[UIWindow alloc] initWithWindowScene:g_window_scene] autorelease];
    blenderIOSApplyTheme(self.window);
    self.window.rootViewController = GHOST_ios_launcher_controller();
    [self.window makeKeyAndVisible];
    NSThread *thread = [[NSThread alloc] initWithBlock:^{
      run_engine();
    }];
    thread.name = @"Blender Engine";
    thread.stackSize = 8 * 1024 * 1024;
    thread.qualityOfService = NSQualityOfServiceUserInitiated;
    [thread start];
    [thread release];
  }
  [self scene:scene openURLContexts:connectionOptions.URLContexts];
}

- (void)scene:(UIScene *)scene openURLContexts:(NSSet<UIOpenURLContext *> *)URLContexts
{
  UNUSED_VARS(scene);
  for (UIOpenURLContext *context in URLContexts) {
    open_document(context.URL);
  }
}

- (void)sceneDidBecomeActive:(UIScene *)scene
{
  engine_active.store(true);
  GHOST_ios_engine_wake();
  [[NSNotificationCenter defaultCenter] postNotificationName:@"BlenderSceneActive" object:scene];
}

- (void)sceneWillResignActive:(UIScene *)scene
{
  engine_active.store(false);
  [[NSNotificationCenter defaultCenter] postNotificationName:@"BlenderSceneInactive" object:scene];
}

- (void)sceneWillEnterForeground:(UIScene *)scene
{
  UNUSED_VARS(scene);
  app_in_background.store(false);
}

- (void)sceneDidEnterBackground:(UIScene *)scene
{
  UNUSED_VARS(scene);
  app_in_background.store(true);
  /* iOS may end a background app at any moment without telling it. Keep the app running long
   * enough to write the auto-save of any unsaved work. */
  UIApplication *application = UIApplication.sharedApplication;
  __block UIBackgroundTaskIdentifier task = UIBackgroundTaskInvalid;
  void (^finish)(void) = ^{
    if (task != UIBackgroundTaskInvalid) {
      [application endBackgroundTask:task];
      task = UIBackgroundTaskInvalid;
    }
  };
  task = [application beginBackgroundTaskWithName:@"Blender Auto-Save" expirationHandler:finish];
  void (^finish_copy)(void) = [[finish copy] autorelease];
  [finish_copy retain];
  request_autosave([finish_copy] {
    dispatch_async(dispatch_get_main_queue(), ^{
      finish_copy();
      [finish_copy release];
    });
  });
}

- (void)applicationWillTerminate
{
  /* Rare on iOS, but give the engine a moment to keep unsaved work before the process ends. */
  auto written = std::make_shared<std::atomic<bool>>(false);
  request_autosave([written] { written->store(true); });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!written->load() && std::chrono::steady_clock::now() < deadline) {
    [NSThread sleepForTimeInterval:0.02];
  }
}

- (void)sceneDidDisconnect:(UIScene *)scene
{
  if (g_window_scene == scene) {
    engine_active.store(false);
    [NSNotificationCenter.defaultCenter postNotificationName:@"BlenderSceneInactive" object:scene];
    g_window_scene = nil;
  }
}

- (void)dealloc
{
  [_window release];
  [super dealloc];
}

@end

@implementation GHOST_IOSMetalRenderer
{
  GHOST_WindowIOS *_window;
}

- (nonnull instancetype)initWithWindow:(nonnull GHOST_WindowIOS *)window
{
  self = [super init];
  if (self) {
    _window = window;
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(sceneActive:)
                                               name:@"BlenderSceneActive"
                                             object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(sceneInactive:)
                                               name:@"BlenderSceneInactive"
                                             object:nil];
    /* The window may only come forward later, over the document launcher, after the scene became
     * active: drawing resumes then too. */
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(windowBecameKey:)
                                               name:UIWindowDidBecomeKeyNotification
                                             object:nil];
  }
  return self;
}

- (void)detach
{
  [NSNotificationCenter.defaultCenter removeObserver:self];
  _window = nullptr;
}

- (void)dealloc
{
  [self detach];
  [super dealloc];
}

- (void)sceneActive:(NSNotification *)notification
{
  if (_window && _window->surface && _window->rootWindow.isKeyWindow) {
    _window->surface->setVisible(true);
  }
}

- (void)windowBecameKey:(NSNotification *)notification
{
  if (_window && _window->surface && notification.object == _window->rootWindow &&
      _window->rootWindow.windowScene.activationState == UISceneActivationStateForegroundActive)
  {
    _window->surface->setVisible(true);
  }
}

- (void)sceneInactive:(NSNotification *)notification
{
  if (_window && _window->surface) {
    _window->surface->setVisible(false);
  }
}

- (void)drawInMTKView:(nonnull MTKView *)view
{
  GHOST_ios_engine_wake();
}

- (void)mtkView:(nonnull MTKView *)view drawableSizeWillChange:(CGSize)size
{
  if (_window) {
    _window->publishGeometry(size);
  }
}

@end

namespace blender {
int GHOST_iosmain(int _argc, const char **_argv)
{
  argc = _argc;
  argv = _argv;
  @autoreleasepool {
    return UIApplicationMain(
        _argc, (char *_Nullable *)_argv, nil, NSStringFromClass([IOSAppDelegate class]));
  }
}

void GHOST_iosfinalize(bContext *CTX)
{
  C = CTX;
}
}  // namespace blender

#pragma mark KeyMap, mouse converters

static GHOST_TButton convertButton(int button)
{
  switch (button) {
    case 0:
      return GHOST_kButtonMaskLeft;
    case 1:
      return GHOST_kButtonMaskRight;
    case 2:
      return GHOST_kButtonMaskMiddle;
    case 3:
      return GHOST_kButtonMaskButton4;
    case 4:
      return GHOST_kButtonMaskButton5;
    case 5:
      return GHOST_kButtonMaskButton6;
    case 6:
      return GHOST_kButtonMaskButton7;
    default:
      return GHOST_kButtonMaskLeft;
  }
}

/**
 * Converts Mac raw-key codes (same for Cocoa & Carbon)
 * into GHOST key codes
 * \param rawCode: The raw physical key code
 * \param recvChar: the character ignoring modifiers (except for shift)
 * \return Ghost key code
 */
GHOST_TKey convertKey(int rawCode, unichar recvChar, uint16_t /*keyAction*/)
{
  switch (rawCode) {
      /* Numbers keys: mapped to handle some int'l keyboard (e.g. French). */
      /*
    case kVK_ISO_Section:
      return GHOST_kKeyUnknown;
    case kVK_ANSI_1:
      return GHOST_kKey1;
    case kVK_ANSI_2:
      return GHOST_kKey2;
    case kVK_ANSI_3:
      return GHOST_kKey3;
    case kVK_ANSI_4:
      return GHOST_kKey4;
    case kVK_ANSI_5:
      return GHOST_kKey5;
    case kVK_ANSI_6:
      return GHOST_kKey6;
    case kVK_ANSI_7:
      return GHOST_kKey7;
    case kVK_ANSI_8:
      return GHOST_kKey8;
    case kVK_ANSI_9:
      return GHOST_kKey9;
    case kVK_ANSI_0:
      return GHOST_kKey0;

    case kVK_ANSI_Keypad0:
      return GHOST_kKeyNumpad0;
    case kVK_ANSI_Keypad1:
      return GHOST_kKeyNumpad1;
    case kVK_ANSI_Keypad2:
      return GHOST_kKeyNumpad2;
    case kVK_ANSI_Keypad3:
      return GHOST_kKeyNumpad3;
    case kVK_ANSI_Keypad4:
      return GHOST_kKeyNumpad4;
    case kVK_ANSI_Keypad5:
      return GHOST_kKeyNumpad5;
    case kVK_ANSI_Keypad6:
      return GHOST_kKeyNumpad6;
    case kVK_ANSI_Keypad7:
      return GHOST_kKeyNumpad7;
    case kVK_ANSI_Keypad8:
      return GHOST_kKeyNumpad8;
    case kVK_ANSI_Keypad9:
      return GHOST_kKeyNumpad9;
    case kVK_ANSI_KeypadDecimal:
      return GHOST_kKeyNumpadPeriod;
    case kVK_ANSI_KeypadEnter:
      return GHOST_kKeyNumpadEnter;
    case kVK_ANSI_KeypadPlus:
      return GHOST_kKeyNumpadPlus;
    case kVK_ANSI_KeypadMinus:
      return GHOST_kKeyNumpadMinus;
    case kVK_ANSI_KeypadMultiply:
      return GHOST_kKeyNumpadAsterisk;
    case kVK_ANSI_KeypadDivide:
      return GHOST_kKeyNumpadSlash;
    case kVK_ANSI_KeypadClear:
      return GHOST_kKeyUnknown;

    case kVK_F1:
      return GHOST_kKeyF1;
    case kVK_F2:
      return GHOST_kKeyF2;

    case kVK_F3:
      return GHOST_kKeyF3;
    case kVK_F4:
      return GHOST_kKeyF4;
    case kVK_F5:
      return GHOST_kKeyF5;
    case kVK_F6:
      return GHOST_kKeyF6;
    case kVK_F7:
      return GHOST_kKeyF7;
    case kVK_F8:
      return GHOST_kKeyF8;
    case kVK_F9:
      return GHOST_kKeyF9;
    case kVK_F10:
      return GHOST_kKeyF10;
    case kVK_F11:
      return GHOST_kKeyF11;
    case kVK_F12:
      return GHOST_kKeyF12;
    case kVK_F13:
      return GHOST_kKeyF13;
    case kVK_F14:
      return GHOST_kKeyF14;
    case kVK_F15:
      return GHOST_kKeyF15;
    case kVK_F16:
      return GHOST_kKeyF16;
    case kVK_F17:
      return GHOST_kKeyF17;
    case kVK_F18:
      return GHOST_kKeyF18;
    case kVK_F19:
      return GHOST_kKeyF19;
    case kVK_F20:
      return GHOST_kKeyF20;

    case kVK_UpArrow:
      return GHOST_kKeyUpArrow;
    case kVK_DownArrow:
      return GHOST_kKeyDownArrow;
    case kVK_LeftArrow:
      return GHOST_kKeyLeftArrow;
    case kVK_RightArrow:
      return GHOST_kKeyRightArrow;

    case kVK_Return:
      return GHOST_kKeyEnter;
    case kVK_Delete:
      return GHOST_kKeyBackSpace;
    case kVK_ForwardDelete:
      return GHOST_kKeyDelete;
    case kVK_Escape:
      return GHOST_kKeyEsc;
    case kVK_Tab:
      return GHOST_kKeyTab;
    case kVK_Space:
      return GHOST_kKeySpace;

    case kVK_Home:
      return GHOST_kKeyHome;
    case kVK_End:
      return GHOST_kKeyEnd;
    case kVK_PageUp:
      return GHOST_kKeyUpPage;
    case kVK_PageDown:
      return GHOST_kKeyDownPage;

       */

    default: {
      /* Alphanumerical or punctuation key that is remappable in int'l keyboards. */
      if ((recvChar >= 'A') && (recvChar <= 'Z')) {
        return (GHOST_TKey)(recvChar - 'A' + GHOST_kKeyA);
      }
      else if ((recvChar >= 'a') && (recvChar <= 'z')) {
        return (GHOST_TKey)(recvChar - 'a' + GHOST_kKeyA);
      }
      else {

        switch (recvChar) {
          case '-':
            return GHOST_kKeyMinus;
          case '+':
            return GHOST_kKeyPlus;
          case '=':
            return GHOST_kKeyEqual;
          case ',':
            return GHOST_kKeyComma;
          case '.':
            return GHOST_kKeyPeriod;
          case '/':
            return GHOST_kKeySlash;
          case ';':
            return GHOST_kKeySemicolon;
          case '\'':
            return GHOST_kKeyQuote;
          case '\\':
            return GHOST_kKeyBackslash;
          case '[':
            return GHOST_kKeyLeftBracket;
          case ']':
            return GHOST_kKeyRightBracket;
          case '`':
            return GHOST_kKeyAccentGrave;
          default:
            return GHOST_kKeyUnknown;
        }
      }
    }
  }
  return GHOST_kKeyUnknown;
}

#pragma mark Utility functions

#define FIRSTFILEBUFLG 512
static bool g_hasFirstFile = false;
static char g_firstFileBuf[512];

extern "C" int GHOST_HACK_getFirstFile(char buf[FIRSTFILEBUFLG])
{
  if (g_hasFirstFile) {
    strncpy(buf, g_firstFileBuf, FIRSTFILEBUFLG - 1);
    buf[FIRSTFILEBUFLG - 1] = '\0';
    return 1;
  }
  else {
    return 0;
  }
}

#pragma mark initialization/finalization

GHOST_SystemIOS::GHOST_SystemIOS()
{
  int mib[2];
  struct timeval boottime;
  size_t len;
  char *rstring = NULL;

  modifier_mask_ = 0;
  outside_loop_event_processed_ = false;
  need_delayed_application_become_active_event_processing_ = false;

  /* TODO: sysctl likely should be replaced with another approach. */
  mib[0] = CTL_KERN;
  mib[1] = KERN_BOOTTIME;
  len = sizeof(struct timeval);

  sysctl(mib, 2, &boottime, &len, NULL, 0);
  m_start_time = ((boottime.tv_sec * 1000) + (boottime.tv_usec / 1000));

  /* Detect multi-touch track-pad. */
  mib[0] = CTL_HW;
  mib[1] = HW_MODEL;
  sysctl(mib, 2, NULL, &len, NULL, 0);
  rstring = (char *)malloc(len);
  sysctl(mib, 2, rstring, &len, NULL, 0);

  free(rstring);
  rstring = NULL;

  ignore_window_sized_message_ = false;
  ignore_momentum_scroll_ = false;
  multi_touch_scroll_ = false;
  last_warp_timestamp_ = 0;
}

GHOST_SystemIOS::~GHOST_SystemIOS()
{
  GHOST_ios_document_access_shutdown();
}

GHOST_TSuccess GHOST_SystemIOS::init()
{
  GHOST_ios_document_access_restore();
  GHOST_TSuccess success = GHOST_System::init();
  if (success) {

#ifdef WITH_INPUT_NDOF
    m_ndofManager = new GHOST_NDOFManagerCocoa(*this);
#endif
  }
  return success;
}

#pragma mark window management

uint64_t GHOST_SystemIOS::getMilliSeconds() const
{
  struct timeval currentTime;

  gettimeofday(&currentTime, NULL);
  return ((currentTime.tv_sec * 1000) + (currentTime.tv_usec / 1000) - m_start_time);
}

uint8_t GHOST_SystemIOS::getNumDisplays() const
{
  return 1;
}

void GHOST_SystemIOS::getMainDisplayDimensions(uint32_t &width, uint32_t &height) const
{
  __block CGRect screenRect;
  __block CGFloat scaling_fac;
  GHOST_ios_main_sync(^{
    screenRect = GHOSTIOSDisplayBounds();
    scaling_fac = GHOSTIOSDisplayScale(nil);
  });
  CGFloat screenWidth = screenRect.size.width * scaling_fac;
  CGFloat screenHeight = screenRect.size.height * scaling_fac;

  if (screenWidth <= 0 || screenHeight <= 0) {
    GHOST_ASSERT(false, "Negative or null display dimmensions");
    screenWidth = 2532;
    screenHeight = 1170;
  }

  width = screenWidth;
  height = screenHeight;
}

void GHOST_SystemIOS::getAllDisplayDimensions(uint32_t &width, uint32_t &height) const
{
  /* TOOD: iOS passthrough. */
  getMainDisplayDimensions(width, height);
}

GHOST_IWindow *GHOST_SystemIOS::createWindow(const char *title,
                                             int32_t /*left*/,
                                             int32_t /*top*/,
                                             uint32_t /*width*/,
                                             uint32_t /*height*/,
                                             GHOST_TWindowState state,
                                             GHOST_GPUSettings gpu_settings,
                                             const bool /*exclusive*/,
                                             const bool is_dialog,
                                             const GHOST_IWindow *parent_window)
{
  const GHOST_ContextParams context_params = GHOST_CONTEXT_PARAMS_FROM_GPU_SETTINGS(gpu_settings);
  GHOST_IWindow *window = nullptr;
  @autoreleasepool {

    /* Create window at native size. */
    __block CGRect bounds;
    GHOST_ios_main_sync(^{
      bounds = GHOSTIOSDisplayBounds();
    });

    window = (GHOST_IWindow *)new GHOST_WindowIOS(this,
                                                  title,
                                                  (int)bounds.origin.x,
                                                  (int)bounds.origin.y,
                                                  (unsigned int)bounds.size.width,
                                                  (unsigned int)bounds.size.height,
                                                  state,
                                                  gpu_settings.context_type,
                                                  context_params,
                                                  is_dialog,
                                                  (GHOST_WindowIOS *)parent_window);

    if (window->getValid()) {
      // Store the pointer to the window
      GHOST_ASSERT(window_manager_, "m_windowManager not initialized");
      window_manager_->addWindow(window);
      window_manager_->setActiveWindow(window);
      pushEvent(
          std::make_unique<GHOST_Event>(getMilliSeconds(), GHOST_kEventWindowActivate, window));
      pushEvent(std::make_unique<GHOST_Event>(getMilliSeconds(), GHOST_kEventWindowSize, window));
    }
    else {
      GHOST_PRINT("GHOST_SystemIOS::createWindow(): window invalid\n");
      delete window;
      window = nullptr;
    }
  }
  return window;
}

/**
 * Create a new offscreen context.
 * Never explicitly delete the context, use #disposeContext() instead.
 * \return The new context (or 0 if creation failed).
 */
GHOST_IContext *GHOST_SystemIOS::createOffscreenContext(GHOST_GPUSettings gpu_settings)
{
  const GHOST_ContextParams context_params_offscreen =
      GHOST_CONTEXT_PARAMS_FROM_GPU_SETTINGS_OFFSCREEN(gpu_settings);

  GHOST_Context *context = new GHOST_ContextIOS(context_params_offscreen);
  if (context->initializeDrawingContext()) {
    return context;
  }

  delete context;
  return nullptr;
}

/**
 * Dispose of a context.
 * \param context: Pointer to the context to be disposed.
 * \return Indication of success.
 */
GHOST_TSuccess GHOST_SystemIOS::disposeContext(GHOST_IContext *context)
{
  delete context;

  return GHOST_kSuccess;
}

/* iOS has no global cursor query. Use the last absolute input sample rather than
 * reporting success with uninitialized output coordinates. */
GHOST_TSuccess GHOST_SystemIOS::getCursorPosition(int32_t &x, int32_t &y) const
{
  const GHOST_WindowIOS *window = static_cast<const GHOST_WindowIOS *>(
      window_manager_->getActiveWindow());
  if (!window) {
    return GHOST_kFailure;
  }
  return window->getPointerPosition(x, y);
}

/**
 * \note : expect Cocoa screen coordinates
 * TODO: If external mouse or trackpad is connected, we can set cursor position.
 */
GHOST_TSuccess GHOST_SystemIOS::setCursorPosition(int32_t x, int32_t y)
{
  GHOST_WindowIOS *window = (GHOST_WindowIOS *)window_manager_->getActiveWindow();
  if (!window)
    return GHOST_kFailure;

  window->updatePointerPosition(x, y);
  pushEvent(std::make_unique<GHOST_EventCursor>(
      getMilliSeconds(), GHOST_kEventCursorMove, window, x, y, window->getTabletData()));
  outside_loop_event_processed_ = true;

  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_SystemIOS::setMouseCursorPosition(int32_t /*x*/, int32_t /*y*/)
{
  /* iOS Passthrough. */
  GHOST_WindowIOS *window = (GHOST_WindowIOS *)window_manager_->getActiveWindow();
  if (!window)
    return GHOST_kFailure;
  GHOST_ASSERT(FALSE, "GHOST_SystemIOS::setMouseCursorPosition unsupported on iOS");
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_SystemIOS::getModifierKeys(GHOST_ModifierKeys & /*keys*/) const
{
  /* iOS Passthrough. */
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_SystemIOS::getButtons(GHOST_Buttons & /*buttons*/) const
{
  /* iOS Passthrough. */
  return GHOST_kSuccess;
}
GHOST_TCapabilityFlag GHOST_SystemIOS::getCapabilities() const
{
  return GHOST_TCapabilityFlag(GHOST_kCapabilityGPUReadFrontBuffer);
}

#pragma mark Event handlers

/**
 * The event queue polling function
 */
bool GHOST_SystemIOS::processEvents(bool /*waitForEvent*/)
{
  GHOST_ASSERT(!NSThread.isMainThread, "Blender events belong to the engine thread");
  for (auto &entry : native_events_.take()) {
    if (entry.alive->load() && validWindow(entry.event->getWindow())) {
      GHOST_System::pushEvent(std::move(entry.event));
    }
  }
  return true;
}

GHOST_TSuccess GHOST_SystemIOS::pushEvent(std::unique_ptr<const GHOST_IEvent> event)
{
  if (!NSThread.isMainThread) {
    return GHOST_System::pushEvent(std::move(event));
  }
  auto *window = static_cast<GHOST_WindowIOS *>(event->getWindow());
  if (!window || !window->input_alive->load()) {
    return GHOST_kFailure;
  }
  native_events_.push({std::move(event), window->input_alive});
  GHOST_ios_engine_wake();
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_SystemIOS::disposeWindow(GHOST_IWindow *window)
{
  if (validWindow(window)) {
    static_cast<GHOST_WindowIOS *>(window)->detachNativeInput();
  }
  return GHOST_System::disposeWindow(window);
}

GHOST_TSuccess GHOST_SystemIOS::handleApplicationBecomeActiveEvent()
{
  modifier_mask_ = 0;

  outside_loop_event_processed_ = true;
  return GHOST_kSuccess;
}

bool GHOST_SystemIOS::hasDialogWindow()
{
  for (GHOST_IWindow *iwindow : window_manager_->getWindows()) {
    GHOST_WindowIOS *window = (GHOST_WindowIOS *)iwindow;
    if (window->isDialog()) {
      return true;
    }
  }
  return false;
}

void GHOST_SystemIOS::notifyExternalEventProcessed()
{
  outside_loop_event_processed_ = true;
}

GHOST_TSuccess GHOST_SystemIOS::handleWindowEvent(GHOST_TEventType eventType,
                                                  GHOST_WindowIOS *window)
{
  if (!validWindow(window)) {
    return GHOST_kFailure;
  }
  switch (eventType) {
    case GHOST_kEventWindowClose:
      pushEvent(std::make_unique<GHOST_Event>(getMilliSeconds(), GHOST_kEventWindowClose, window));
      break;
    case GHOST_kEventWindowActivate:
      window_manager_->setActiveWindow(window);
      window->loadCursor(window->getCursorVisibility(), window->getCursorShape());
      pushEvent(
          std::make_unique<GHOST_Event>(getMilliSeconds(), GHOST_kEventWindowActivate, window));
      break;
    case GHOST_kEventWindowDeactivate:
      window_manager_->setWindowInactive(window);
      pushEvent(
          std::make_unique<GHOST_Event>(getMilliSeconds(), GHOST_kEventWindowDeactivate, window));
      break;
    case GHOST_kEventWindowUpdate:
      if (native_pixel_) {
        window->setNativePixelSize();
        pushEvent(std::make_unique<GHOST_Event>(
            getMilliSeconds(), GHOST_kEventNativeResolutionChange, window));
      }
      pushEvent(
          std::make_unique<GHOST_Event>(getMilliSeconds(), GHOST_kEventWindowUpdate, window));
      break;
    case GHOST_kEventWindowMove:
      pushEvent(std::make_unique<GHOST_Event>(getMilliSeconds(), GHOST_kEventWindowMove, window));
      break;
    case GHOST_kEventWindowSize:
      if (!ignore_window_sized_message_) {
        // Enforce only one resize message per event loop
        // (coalescing all the live resize messages)
        window->updateDrawingContext();
        pushEvent(
            std::make_unique<GHOST_Event>(getMilliSeconds(), GHOST_kEventWindowSize, window));
        // Mouse up event is trapped by the resizing event loop,
        // so send it anyway to the window manager.
        pushEvent(std::make_unique<GHOST_EventButton>(getMilliSeconds(),
                                                      GHOST_kEventButtonUp,
                                                      window,
                                                      GHOST_kButtonMaskLeft,
                                                      GHOST_TABLET_DATA_NONE));
      }
      break;
    case GHOST_kEventNativeResolutionChange:

      if (native_pixel_) {
        pushEvent(std::make_unique<GHOST_Event>(
            getMilliSeconds(), GHOST_kEventNativeResolutionChange, window));
      }

    default:
      return GHOST_kFailure;
      break;
  }

  outside_loop_event_processed_ = true;

  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_SystemIOS::popupOnScreenKeyboard(
    GHOST_IWindow *window, const GHOST_KeyboardProperties &keyboard_properties)
{
  if (!validWindow((GHOST_IWindow *)window)) {
    return GHOST_kFailure;
  }
  GHOST_WindowIOS *windowIOS = (GHOST_WindowIOS *)window;
  return windowIOS->popupOnscreenKeyboard(keyboard_properties);
}

GHOST_TSuccess GHOST_SystemIOS::hideOnScreenKeyboard(GHOST_IWindow *window)
{
  if (!validWindow((GHOST_IWindow *)window)) {
    return GHOST_kFailure;
  }

  GHOST_WindowIOS *windowIOS = (GHOST_WindowIOS *)window;

  return windowIOS->hideOnscreenKeyboard();
}

const char *GHOST_SystemIOS::getKeyboardInput(GHOST_IWindow *window)
{
  if (!validWindow((GHOST_IWindow *)window)) {
    return nullptr;
  }

  GHOST_WindowIOS *windowIOS = (GHOST_WindowIOS *)window;

  return windowIOS->getLastKeyboardString();
}

GHOST_TSuccess GHOST_SystemIOS::startSecurityScopedFileAccess(const char *filepath)
{
  NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:filepath]];
  BOOL success = [url startAccessingSecurityScopedResource];

  return success ? GHOST_kSuccess : GHOST_kFailure;
}

GHOST_TSuccess GHOST_SystemIOS::stopSecurityScopedFileAccess(const char *filepath)
{
  NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:filepath]];
  [url stopAccessingSecurityScopedResource];

  return GHOST_kSuccess;
}

// Note: called from NSWindow subclass
GHOST_TSuccess GHOST_SystemIOS::handleDraggingEvent(GHOST_TEventType eventType,
                                                    GHOST_TDragnDropTypes draggedObjectType,
                                                    GHOST_WindowIOS *window,
                                                    int mouseX,
                                                    int mouseY,
                                                    void *data)
{
  if (!validWindow((GHOST_IWindow *)window)) {
    return GHOST_kFailure;
  }
  switch (eventType) {
    case GHOST_kEventDraggingEntered:
    case GHOST_kEventDraggingUpdated:
    case GHOST_kEventDraggingExited:
      window->clientToScreenIntern(mouseX, mouseY, mouseX, mouseY);
      pushEvent(std::make_unique<GHOST_EventDragnDrop>(
          getMilliSeconds(), eventType, draggedObjectType, window, mouseX, mouseY, nullptr));
      break;

    case GHOST_kEventDraggingDropDone: {
      uint8_t *temp_buff;
      GHOST_TStringArray *strArray;
      NSArray *droppedArray;
      size_t pastedTextSize;
      NSString *droppedStr;
      GHOST_TDragnDropDataPtr eventData;
      int i;

      if (!data)
        return GHOST_kFailure;

      switch (draggedObjectType) {
        case GHOST_kDragnDropTypeFilenames:
          droppedArray = (NSArray *)data;

          strArray = (GHOST_TStringArray *)malloc(sizeof(GHOST_TStringArray));
          if (!strArray)
            return GHOST_kFailure;

          strArray->count = [droppedArray count];
          if (strArray->count == 0) {
            free(strArray);
            return GHOST_kFailure;
          }

          strArray->strings = (uint8_t **)malloc(strArray->count * sizeof(uint8_t *));

          for (i = 0; i < strArray->count; i++) {
            droppedStr = [droppedArray objectAtIndex:i];

            pastedTextSize = [droppedStr lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
            temp_buff = (uint8_t *)malloc(pastedTextSize + 1);

            if (!temp_buff) {
              strArray->count = i;
              break;
            }

            strncpy((char *)temp_buff,
                    [droppedStr cStringUsingEncoding:NSUTF8StringEncoding],
                    pastedTextSize);
            temp_buff[pastedTextSize] = '\0';

            strArray->strings[i] = temp_buff;
          }

          eventData = static_cast<GHOST_TDragnDropDataPtr>(strArray);
          break;

        case GHOST_kDragnDropTypeString:
          droppedStr = (NSString *)data;
          pastedTextSize = [droppedStr lengthOfBytesUsingEncoding:NSUTF8StringEncoding];

          temp_buff = (uint8_t *)malloc(pastedTextSize + 1);

          if (temp_buff == NULL) {
            return GHOST_kFailure;
          }

          strncpy((char *)temp_buff,
                  [droppedStr cStringUsingEncoding:NSUTF8StringEncoding],
                  pastedTextSize);

          temp_buff[pastedTextSize] = '\0';

          eventData = static_cast<GHOST_TDragnDropDataPtr>(temp_buff);
          break;

        case GHOST_kDragnDropTypeBitmap: {
          /* Unsupported iOS. */
          return GHOST_kFailure;
          break;
        }
        default:
          return GHOST_kFailure;
          break;
      }

      pushEvent(std::make_unique<GHOST_EventDragnDrop>(
          getMilliSeconds(), eventType, draggedObjectType, window, mouseX, mouseY, eventData));

      break;
    }
    default:
      return GHOST_kFailure;
  }
  outside_loop_event_processed_ = true;
  return GHOST_kSuccess;
}

void GHOST_SystemIOS::handleQuitRequest()
{
  GHOST_Window *window = (GHOST_Window *)window_manager_->getActiveWindow();

  // Discard quit event if we are in cursor grab sequence
  if (window && window->getCursorGrabModeIsWarp())
    return;

  // Push the event to Blender so it can open a dialog if needed
  pushEvent(std::make_unique<GHOST_Event>(getMilliSeconds(), GHOST_kEventQuitRequest, window));
  outside_loop_event_processed_ = true;
}

bool GHOST_SystemIOS::handleOpenDocumentRequest(void *filepathStr)
{
  NSString *filepath = (NSString *)filepathStr;

  @autoreleasepool {
    if (!current_active_window_) {
      return NO;
    }

    /* Discard event if we are in cursor grab sequence,
     * it'll lead to "stuck cursor" situation if the alert panel is raised. */
    if (current_active_window_->getCursorGrabModeIsWarp()) {
      return NO;
    }

    const size_t filenameTextSize = [filepath lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
    char *temp_buff = (char *)malloc(filenameTextSize + 1);

    if (temp_buff == nullptr) {
      return GHOST_kFailure;
    }

    memcpy(temp_buff, [filepath cStringUsingEncoding:NSUTF8StringEncoding], filenameTextSize);
    temp_buff[filenameTextSize] = '\0';

    pushEvent(std::make_unique<GHOST_EventString>(getMilliSeconds(),
                                                  GHOST_kEventOpenMainFile,
                                                  current_active_window_,
                                                  static_cast<GHOST_TEventDataPtr>(temp_buff)));
  }
  return YES;
}

/* None of this currently required for iOS */
#if 0
GHOST_TSuccess GHOST_SystemIOS::handleTabletEvent(void * /*eventPtr*/, short /*eventType*/)
{
  GHOST_WindowIOS *window = (GHOST_WindowIOS *)window_manager_->getActiveWindow();
  if (!window)
    return GHOST_kFailure;
  
  return GHOST_kSuccess;
}

bool GHOST_SystemIOS::handleTabletEvent(void * /*eventPtr*/)
{
  /* TODO: Handle events. */
  GHOST_ASSERT(FALSE,"GHOST_SystemIOS::handleTabletEvent unsupported on iOS");
  return true;
}

GHOST_TSuccess GHOST_SystemIOS::handleMouseEvent(void * /*eventPtr*/)
{
  /* TODO: Handle events (here or elsewhere).
   * NOTE: "Touch" events already handled in other code paths above. */
  GHOST_ASSERT(FALSE,"GHOST_SystemIOS::handleMouseEvent unsupported on iOS");
  return GHOST_kSuccess;
}

#  include <Metal/Metal.h>
bool frame_capture = false;
extern id<MTLDevice> extern_device;
GHOST_TSuccess GHOST_SystemIOS::handleKeyEvent(void * /*eventPtr*/)
{
  /* TODO: Handle events (here or elsewhere). */
  GHOST_ASSERT(FALSE,"GHOST_SystemIOS::handleKeyEvent unsupported on iOS");
  return GHOST_kSuccess;
}
#endif

#pragma mark Clipboard get/set

char *GHOST_SystemIOS::getClipboard(bool /*selection*/) const
{
  __block char *result = nullptr;
  GHOST_ios_main_sync(^{
    @autoreleasepool {
      UIPasteboard *pasteBoard = [UIPasteboard generalPasteboard];
      NSString *textPasted = pasteBoard.string;

      if (textPasted == nil) {
        return;
      }

      const size_t pastedTextSize = [textPasted lengthOfBytesUsingEncoding:NSUTF8StringEncoding];

      char *temp_buff = (char *)malloc(pastedTextSize + 1);

      if (temp_buff == nullptr) {
        return;
      }

      memcpy(temp_buff, [textPasted cStringUsingEncoding:NSUTF8StringEncoding], pastedTextSize);
      temp_buff[pastedTextSize] = '\0';
      result = temp_buff;
    }
  });
  return result;
}

void GHOST_SystemIOS::putClipboard(const char *buffer, bool selection) const
{
  if (selection) {
    return; /* For copying the selection, used on X11. */
  }

  @autoreleasepool {
    NSString *textToCopy = [NSString stringWithCString:buffer encoding:NSUTF8StringEncoding];
    dispatch_async(dispatch_get_main_queue(), ^{
      UIPasteboard.generalPasteboard.string = textToCopy;
    });
  }
}

GHOST_IWindow *GHOST_SystemIOS::getWindowUnderCursor(int32_t /*x*/, int32_t /*y*/)
{
  GHOST_ASSERT(FALSE, "GHOST_SystemIOS::getWindowUnderCursor unsupported on iOS");
  return nullptr;
}
