# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Thread handoff stress and real UIKit/Metal presentation during engine stalls."""

import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ResponsiveUITest(unittest.TestCase):
    def test_mixed_native_commands_keep_submission_order(self):
        source = (ROOT / "source/blender/windowmanager/intern/wm_apple_ios_ui.cc").read_text()
        start = source.index("enum class IOSUICommand")
        state = source[start:source.index("bool ios_ui_layout_ready", start)]
        start = source.index('extern "C" void WM_apple_ios_ui_command_enqueue')
        enqueue = source[start:source.index('extern "C" char *WM_apple_ios_ui_toolbar_copy_json', start)]
        start = source.index('extern "C" uint64_t WM_apple_ios_ui_menu_request')
        enqueue += source[start:source.index("/* UIKit only exchanges", start)]
        start = source.index("static void ios_ui_dispatch(bContext")
        dispatch = source[start:source.index("\n}\n", start) + 2]
        harness = r'''
#include "GHOST_IOSMessageQueue.hh"
#include <cassert>
#include <cstring>
#include <map>
#include <string>
#include <variant>
''' + state + source[source.index('struct IOSPencilHoverRequest'):source.index('struct IOSActionItem')] + r'''
struct bContext {};
struct IOSContextStore {};
static std::vector<std::string> executed;
static IOSContextStore ios_context_push_view3d(bContext *) { return {}; }
static void ios_context_pop(bContext *, IOSContextStore) {}
static void *CTX_wm_window(bContext *C) { return C; }
static void *CTX_wm_area(bContext *C) { return C; }
static void *CTX_wm_region(bContext *C) { return C; }
static void ios_command_execute(bContext *, IOSUICommand) { executed.push_back("mode"); }
static void ios_tool_set_by_id(bContext *, const std::string &id) { executed.push_back(id); }
static void ios_ui_dispatch_operator(bContext *, IOSOperatorCommand &c) { executed.push_back(c.op_name); }
static void ios_ui_dispatch_menu(bContext *, IOSMenuRequest &c) { executed.push_back(c.route); }
static void ios_brush_activate(bContext *, const std::string &key) { executed.push_back(key); }
static void ios_setting_execute(bContext *, const IOSSettingCommand &c) { executed.push_back(c.setting); }
static void ios_view_roll_angle(bContext *, float) { executed.push_back("roll"); }
static void ios_operator_call_with_enum_property(bContext *, const char *op, const char *, const char *) {
  executed.push_back(op);
}
''' + enqueue + dispatch + r'''
int main() {
  bContext C;
  char tool[] = "tool";
  WM_apple_ios_ui_operator_enum_enqueue("property", "type", "VALUE");
  WM_apple_ios_ui_setting_enqueue("setting", "LOCAL");
  WM_apple_ios_ui_view_roll_enqueue(0.5f);
  WM_apple_ios_ui_menu_request("menu", true);
  WM_apple_ios_ui_operator_enqueue("operator");
  WM_apple_ios_ui_tool_set_by_id(tool);
  tool[0] = 'X';
  WM_apple_ios_ui_command_enqueue(1);
  WM_apple_ios_ui_brush_activate("brush");
  WM_apple_ios_ui_operator_at_position_enqueue("pencil", 15, 30);
  for (auto &message : ios_ui_messages.take()) ios_ui_dispatch(&C, message);
  const std::vector<std::string> expected{
      "property", "setting", "roll", "menu", "operator", "tool", "mode", "brush", "pencil"};
  assert(executed == expected);
  for (int i = 0; i < 1000; i++) WM_apple_ios_ui_pencil_hover_set(true, i, i + 1);
  IOSPencilHoverRequest hover;
  assert(ios_hover_requests.take(hover) && hover.active && hover.x == 999 && hover.y == 1000);
  WM_apple_ios_ui_pencil_hover_set(false, 0, 0);
  assert(ios_hover_requests.take(hover) && !hover.active);
  assert(ios_ui_messages.take().empty());
}
'''
        with tempfile.TemporaryDirectory() as directory:
            code = Path(directory) / "commands.cc"
            executable = Path(directory) / "commands"
            code.write_text(harness)
            result = subprocess.run(["xcrun", "clang++", "-std=c++17", "-I", str(ROOT / "intern/ghost"),
                                     str(code), "-o", str(executable)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_mailbox_ownership_ordering_and_stalled_consumer(self):
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "mailbox"
            command = ["xcrun", "clang++", "-std=c++17", "-pthread", "-g",
                       "-I", str(ROOT / "intern/ghost"),
                       str(Path(__file__).with_name("responsive_mailbox_test.cc")),
                       "-o", str(executable)]
            if os.environ.get("IOS_TEST_TSAN"):
                command += ["-fsanitize=thread"]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_native_ui_and_last_completed_frame_during_stall(self):
        device = os.environ.get("IOS_TEST_SIMULATOR_UDID")
        if not device:
            self.skipTest("Set IOS_TEST_SIMULATOR_UDID to an isolated booted simulator")
        sdk = subprocess.check_output(
            ["xcrun", "--sdk", "iphonesimulator", "--show-sdk-path"], text=True).strip()
        system = (ROOT / "intern/ghost/intern/GHOST_SystemIOS.mm").read_text()
        start = system.index("static void run_engine()")
        runtime = system[start:system.index("\nUIWindowScene *GHOST_getIOSWindowScene", start)]
        harness = Path(__file__).with_name("responsive_ui_test.mm").read_text()
        harness = harness.replace("// ENGINE_LOOP", runtime)
        window = (ROOT / "intern/ghost/intern/GHOST_WindowIOS.mm").read_text()
        start = window.index("- (void)installViewportLoadingIndicator\n")
        indicator = window[start:window.index("- (void)detachBlender\n", start)]
        harness = harness.replace("// LOADING_INDICATOR", indicator)
        with tempfile.TemporaryDirectory() as directory:
            app = Path(directory) / "ResponsiveUI.app"
            app.mkdir()
            bundle = "org.blender.tests.responsive-ui"
            (app / "Info.plist").write_bytes(plistlib.dumps({
                "CFBundleIdentifier": bundle, "CFBundleExecutable": "ResponsiveUI",
                "CFBundleName": "ResponsiveUI", "CFBundlePackageType": "APPL",
                "CFBundleVersion": "1", "CFBundleShortVersionString": "1.0",
                "MinimumOSVersion": "26.0", "UIDeviceFamily": [1, 2], "UILaunchScreen": {},
                "UIApplicationSceneManifest": {"UISceneConfigurations": {
                    "UIWindowSceneSessionRoleApplication": [{
                        "UISceneConfigurationName": "Tests", "UISceneDelegateClassName": "ResponsiveScene",
                    }],
                }},
            }))
            code = Path(directory) / "responsive.mm"
            code.write_text(harness)
            result = subprocess.run([
                "xcrun", "--sdk", "iphonesimulator", "clang++", "-std=c++17", "-fblocks", "-fno-objc-arc",
                "-I", str(ROOT / "intern/ghost"), "-I", str(ROOT / "intern/ghost/intern"),
                "-isysroot", sdk, "-target", f"{os.uname().machine}-apple-ios26.0-simulator",
                "-framework", "UIKit", "-framework", "Foundation", "-framework", "Metal",
                "-framework", "QuartzCore", "-framework", "CoreGraphics", str(code),
                str(ROOT / "intern/ghost/intern/GHOST_IOSSurface.mm"),
                "-o", str(app / "ResponsiveUI"),
            ], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run(["codesign", "--force", "--sign", "-", str(app)], check=True, capture_output=True)
            subprocess.run(["xcrun", "simctl", "install", device, str(app)], check=True, capture_output=True)
            checker = Path(directory) / "check-pixels"
            subprocess.run(["xcrun", "swiftc", str(Path(__file__).with_name("responsive_pixels.swift")),
                            "-o", str(checker)], check=True, capture_output=True)
            try:
                container = Path(subprocess.check_output(
                    ["xcrun", "simctl", "get_app_container", device, bundle, "data"], text=True).strip())
                process = subprocess.Popen(["xcrun", "simctl", "launch", "--console-pty", device, bundle],
                                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                try:
                    for expected, channel in (("stall", "green"), ("after", "blue")):
                        deadline = time.monotonic() + 25
                        marker = container / "Documents/phase"
                        while time.monotonic() < deadline:
                            if marker.exists() and marker.read_text() == expected:
                                break
                            time.sleep(0.05)
                        else:
                            self.fail(f"Engine/UI did not reach {expected}")
                        time.sleep(0.3)
                        screenshot = container / f"Documents/responsive-{expected}.png"
                        display = os.environ.get("IOS_TEST_DISPLAY")
                        screen_options = [f"--display={display}"] if display else []
                        subprocess.run(["xcrun", "simctl", "io", device, "screenshot",
                                        *screen_options, str(screenshot)],
                                       check=True, capture_output=True)
                        pixels = subprocess.run([str(checker),
                            str(screenshot), channel], capture_output=True, text=True)
                        self.assertEqual(pixels.returncode, 0, pixels.stdout + pixels.stderr)
                    output = process.communicate(timeout=30)[0]
                    self.assertEqual(process.returncode, 0, output)
                    self.assertIn("PASS: responsive UIKit", output)
                    (container / "Documents/responsiveness.log").write_text(output)
                finally:
                    if process.poll() is None:
                        process.kill()
                        process.communicate()
                artifacts = os.environ.get("IOS_TEST_ARTIFACT_DIR")
                if artifacts:
                    shutil.copytree(container / "Documents", artifacts, dirs_exist_ok=True)
            finally:
                artifacts = os.environ.get("IOS_TEST_ARTIFACT_DIR")
                if artifacts and "container" in locals():
                    shutil.copytree(container / "Documents", artifacts, dirs_exist_ok=True)
                subprocess.run(["xcrun", "simctl", "uninstall", device, bundle], capture_output=True)


if __name__ == "__main__":
    unittest.main()
