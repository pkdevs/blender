# iOS and visionOS Tests

Tests of the native iPad and Apple Vision Pro interface that need Apple's tools, a simulator or a
device, so they cannot run with Blender's own tests. The interface's Python side is covered there
instead: `script_native_actions`, `script_native_panels` and `script_native_timeline` in
`tests/python`.

There are three kinds:

- `test_*.py`: Python `unittest` modules, run on a Mac. Some run Blender scripts (set
  `BLENDER_BIN`), some build native harnesses with the iOS SDK and run them in a booted simulator
  (set `IOS_TEST_SIMULATOR_UDID`, otherwise a booted simulator is picked or the test is skipped).
  Tests build whole source files into their harnesses, never pieces cut out of them, so they
  survive refactoring.
- `*_device_test.py`: scripts run inside the app on a device, in a separate QA install, see
  "Full-App Device Probe" and "Full-App Functional Checks" below. They write a JSON report to the
  app's Documents directory.
- `*.mm`, `*.cc` and `*.swift`: harnesses and probes the tests above build, and
  `swiftui_panels_harness`, a standalone app hosting the SwiftUI panels with recorded snapshots.

Run the `unittest` modules with:

```sh
BLENDER_BIN=/path/to/blender \
python3 -m unittest discover -s tests/ios -p 'test_*.py' -v
```

## Responsiveness Tests

Use an isolated, booted iOS simulator:

```sh
DEVELOPER_DIR=/path/to/Xcode.app/Contents/Developer \
IOS_TEST_SIMULATOR_UDID=<simulator-uuid> \
IOS_TEST_ICON_BUNDLE=/path/to/build_ipad_device/bin/Release/Blender.app \
IOS_TEST_ARTIFACT_DIR=/tmp/blender-responsive-validation \
python3 -m unittest discover -s tests/ios -p 'test_*.py' -v
```

`test_responsive_ui.py` stress-tests the mailbox UIKit and Blender's thread exchange snapshots
through, including a stalled consumer. It supports `IOS_TEST_TSAN=1` for Thread Sanitizer.
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

## Native Dialogs

`test_native_dialogs.py` covers the native alert and sheet controls, including destructive
styling. It is part of the simulator suite above. For full-app checks, copy
`native_dialogs_device_test.py` into the separate QA app's Documents directory and
launch it using the same factory-startup procedure. Its report is
`Documents/native-dialogs-test.json`. Do not run it against a user's scene.

