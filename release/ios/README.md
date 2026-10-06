Buildbot Configuration
======================

Files used by Buildbot's `package-code-binaires` step for the darwin platform.

## Source and Dependency Baseline

The native iOS interface is integrated with the official Blender `ios` branch at
`a1de44dd54af75a4c8c4a29a5fed2a1334a87446`, based on Blender 5.1.2. The previous
native interface used a Blender 5.0 alpha base.

Build the application, runtime libraries and macOS code generators from matching
versions. The older iOS precompiled-library checkout is not sufficient for this
source: in particular, this baseline needs Python 3.13, NumPy 2.3, fmt 12 and the
updated image, geometry and rendering libraries.

For an iOS-configured `build_files/build_environment` build, the
`apple_runtime_dependencies` target builds the app's runtime dependencies without
the unsupported desktop compiler backends. Set `HARVEST_TARGET` outside the source
checkout. With CMake 4, set `CMAKE_POLICY_VERSION_MINIMUM=3.5` for dependencies that
still declare older policy versions.

The host dependency build must provide Python 3.13 with Cython and Meson. Host
tools (`makesdna`, `makesrna`, `datatoc`, `msgfmt` and `shader_tool`) must also come
from the current source. Set `CROSSCOMPILE_TOOLDIR` when configuring the app to use
a separate host-tool build directory, and set `LIBDIR` to the rebuilt iOS
libraries. Keep device and simulator library/build directories separate: both
use ARM64, but their binaries are not interchangeable.

## Native UI Threading

The iOS port has three independent execution paths:

- UIKit owns windows, native controls, gestures, menus, scrolling and animations.
  It never calls the Blender event loop or waits for Blender or a Metal drawable.
- The `Blender Engine` thread initializes Blender (including Python and Blender's
  logical main-thread identity), processes events and commands, evaluates scenes,
  and submits rendering. Blender's existing internal job system remains available.
- `org.blender.viewport.presentation` acquires drawables and presents completed
  viewport frames. It never accesses a UIView or live Blender state.

This separation follows [Apple's responsiveness guidance](https://developer.apple.com/documentation/xcode/improving-app-responsiveness).

`GHOST_IOSMessageQueue` transfers owned commands/events without a consumer-held
lock. All native command types share one FIFO inbox. `GHOST_IOSLatest` transfers
the latest immutable UI snapshot, replacing obsolete unpublished snapshots.
UIKit keeps its last snapshot while the engine is busy. Native menu queries and
file-picker results are asynchronous too.

The toolbar menu and iPadOS menu bar project Blender's registered top-bar and
viewport menus. The viewport Actions menu and Pencil radial menu resolve
Blender's live context menu on demand. All of them preserve the original command
routes, properties, submenus and polling; none uses a hand-written fallback list.

`GHOST_IOSSurface` keeps a bounded three-texture copy pool. Copies are submitted
after the engine's render commands on the same Metal queue, and published only
after GPU completion. Presentation retains its frame until its own GPU work
finishes. A pool slot cannot be overwritten while either consumer holds it.
When the pool is busy, presentation copies are skipped; the last completed frame
remains available, including during a live resize. The engine retains the latest
pending swap and retries it, so its final update is not lost when rendering goes
idle. Offscreen contexts do not create UIKit views.

Window geometry and pointer positions cross the boundary as atomic values.
Input carries a window-lifetime token: closing a window first detaches native
callbacks on UIKit, then invalidates queued input before destroying engine state.
Scene inactivity pauses new engine iterations and viewport presentation; scene
reconnection reattaches the retained native windows.

The engine may synchronously ask UIKit to create/destroy a native resource or
answer a legacy OS query (keyboard/clipboard). These short blocks must not call
Blender. The reverse dependency is forbidden. Normal controls, UI snapshots and
frame presentation do not use this synchronous path. File coordination, bookmark
resolution and filesystem checks run outside UIKit.

## Responsiveness Tests

Use an isolated, booted iOS simulator and Xcode 27.1 beta:

```sh
DEVELOPER_DIR=/Applications/XcodeBeta.app/Contents/Developer \
IOS_TEST_SIMULATOR_UDID=<simulator-uuid> \
IOS_TEST_ICON_BUNDLE=/path/to/build_ipad_device/bin/Release/Blender.app \
IOS_TEST_ARTIFACT_DIR=/tmp/blender-responsive-validation \
python3 -m unittest discover -s release/ios -p 'test_*.py' -v
```

`test_responsive_ui.py` runs the production engine-loop body and Metal presenter
in a UIKit harness. It blocks the mock engine for six seconds, verifies native
animation/scrolling and queued commands, resizes while blocked, and checks actual
simulator screenshot pixels before accepting a recovered frame. This is a
native integration harness, not a full Blender simulator build. The mailbox
stress test also supports `IOS_TEST_TSAN=1` for Thread Sanitizer.
For the Duo's exterior screen, set `IOS_TEST_DISPLAY=primary`; `simctl` can
otherwise capture its inactive interior display. Use `simctl io <uuid> enumerate`
to inspect the currently available display identifiers.

Physical-device validation should additionally use Main Thread Checker and
Instruments Hangs/Animation Hitches while loading large scenes, sculpting,
rendering, resizing, and opening/closing native menus and file pickers. Confirm
the scene is saved before replacing or relaunching an existing device install.

### Full-App Device Probe

`responsive_device_probe.mm` is a test-only dylib, not part of the application
target. Compile it with the iPhoneOS SDK using `clang++ -arch arm64
-miphoneos-version-min=26.0 -std=c++17 -fobjc-arc -dynamiclib -framework UIKit
-framework Foundation -framework QuartzCore -framework CoreGraphics`. Place the resulting
`ResponsiveDeviceProbe.dylib` in `Frameworks` inside a temporary copy of the app,
sign the dylib and that app with the same development identity, and install it
on an explicitly approved test device. Prefer a separate QA bundle identifier
covered by the provisioning profile, with matching signing entitlements, so the
test cannot replace the user's application or its Documents container.

Copy `responsive_device_test.py` and `native_menubar_device_test.py` into that
app's `Documents` directory. Launch with `--factory-startup --python-expr` and:

```python
from pathlib import Path
exec(compile((Path.home() / 'Documents/responsive_device_test.py').read_text(),
             'responsive_device_test.py', 'exec'))
```

The test first exercises real Blender menu projection and dispatch. It then
blocks the actual Blender/Python thread for six seconds while a native display
link expands and scrolls the existing toolbar, reads UI snapshots, and submits
20 operators. It verifies command execution after recovery and writes
`Documents/responsive-device-test.json`. The probe consumes menu results on
UIKit, respecting the production single-consumer contract. Remove the separate
QA application after collecting its reports; never distribute the probe. If
testing in place, restore the normal signed app afterward. These tests do not
synthesize physical Pencil or multitouch input, which still requires hands-on
testing.

### Full-App Functional Checks

Also copy `deep_device_test.py` into the QA app's `Documents` directory. Launch
with `--factory-startup --enable-event-simulate --python-expr` and:

```python
from pathlib import Path
exec(compile((Path.home() / 'Documents/deep_device_test.py').read_text(),
             'deep_device_test.py', 'exec'))
```

This runs the menu and responsiveness checks first, then verifies the visible
native menu roots, mode changes, XYZ visibility across idle tools and live
transforms, actual native axis-button toggles, undo/redo, shading and orientation
snapshots, native Open picker cancellation, and a real `.blend` save/load round
trip. It writes `Documents/deep-device-test.json` and a disposable test scene in
the QA container. Event simulation is test-only and disables normal GHOST input;
do not use this launch for hands-on input testing.

`undo_device_test.py` provides a smaller native Add/Undo/Redo regression check,
writing `Documents/undo-device-test.json`. Both scripts require factory startup
and must never run against a user's scene. Undo checks must perform edits through
the native command bridge: direct Python operators do not reproduce the native
operator undo-recording path.

### Native Warnings

iOS uses UIKit alerts for operator confirmations (including Delete and Revert),
unsaved-file choices, overwrite warnings, report popups and general alerts.
Warnings with options use a native scrolling sheet with switches, including
modified images, asset catalogs and the blocked-script security prompt. Script
execution remains opt-in; dismissing the security sheet does not grant permission.
Complex operator property forms are separate from confirmations and remain in
Blender's UI.

The engine owns pending actions and their original context. UIKit receives copied
dialog content and returns queued results only after dismissal, allowing an action
to open a file picker safely. Closing a window or replacing its editor cancels
stale requests. Cancellation never executes the pending destructive operation.

`test_native_dialogs.py` covers native controls, dismissal ordering, destructive
styling, report deduplication, context restoration and request lifetime. It is part
of the simulator suite above. For full-app checks, copy
`native_dialogs_device_test.py` into the separate QA app's Documents directory and
launch it using the same factory-startup procedure. Its report is
`Documents/native-dialogs-test.json`. Do not run it against a user's scene.
