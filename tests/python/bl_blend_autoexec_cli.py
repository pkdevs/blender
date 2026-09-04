#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Test the "Trusted Source" state when opening blend-files from the command line.

This test runs outside of Blender. Blender is run to create the blend-files &
preferences, then run again to open those files with different command line
arguments, checking ``bpy.app.autoexec`` once the file is open.

Command to run this test directly:
   python3 tests/python/bl_blend_autoexec_cli.py --blender <blender-binary>

Command to run a single test:
   python3 tests/python/bl_blend_autoexec_cli.py --blender <blender-binary> \
       TestAutoExecCLI.test_pref_enabled_untrusted
"""

import argparse
import json
import os
import shlex
import subprocess
import sys
import tempfile
import unittest

BASE_TRUSTED = "trusted_dir"
BASE_UNTRUSTED = "untrusted_dir"
BASE_UNTRUSTED_SUB = "sub_dir"

BLEND_TRUSTED = "trusted_file.blend"
BLEND_UNTRUSTED = "untrusted_file.blend"
BLEND_UNTRUSTED_SUB = "untrusted_sub_file.blend"

# A registered text block, run when the blend-file is loaded with scripts trusted.
TEXT_SCRIPT = "script_maybe_executes.py"
SCENE_SCRIPT_RAN = "Scene [modified by script]"
TEXT_SCRIPT_BODY = (
    "import bpy\n"
    "bpy.data.scenes[0].name = {!r}\n"
).format(SCENE_SCRIPT_RAN)

# The state is printed on a single line with this prefix so it can be found in Blender's output.
STATE_PREFIX = "AUTOEXEC_STATE:"

USERPREF_FILE = "userpref.blend"

# Preference configurations, each saved by a setup run
# into its own `BLENDER_USER_RESOURCES` directory.
CONFIG_AUTOEXEC_ON = "autoexec_on"
CONFIG_AUTOEXEC_OFF = "autoexec_off"
CONFIG_AUTOEXEC_ON_GLOB = "autoexec_on_glob"

# The options each configuration is created with: (use_autoexec, use_glob).
CONFIGS = {
    CONFIG_AUTOEXEC_ON: (True, False),
    CONFIG_AUTOEXEC_OFF: (False, False),
    CONFIG_AUTOEXEC_ON_GLOB: (True, True),
}

# Blender runs this file too, see `main_blender`.
FILEPATH_SELF = os.path.abspath(__file__)

# The Blender binary to test, set from `--blender`.
BLENDER_BIN = ""


# -----------------------------------------------------------------------------
# Utilities

def output_as_text(output):
    return output.decode("utf-8", errors="replace")


def cmd_as_text(cmd):
    """
    Return the command as text, so a failing run can be repeated by hand.
    """
    return " ".join(shlex.quote(arg) for arg in cmd)


def blend_filepaths(dirpath_temp):
    """
    Return the blend-files in ``dirpath_temp``.
    """
    return {
        BLEND_TRUSTED: os.path.join(dirpath_temp, BASE_TRUSTED, BLEND_TRUSTED),
        BLEND_UNTRUSTED: os.path.join(dirpath_temp, BASE_UNTRUSTED, BLEND_UNTRUSTED),
        BLEND_UNTRUSTED_SUB: os.path.join(
            dirpath_temp, BASE_UNTRUSTED, BASE_UNTRUSTED_SUB, BLEND_UNTRUSTED_SUB),
    }


def dirpath_resources(dirpath_temp, config):
    """
    Return the ``BLENDER_USER_RESOURCES`` directory for ``config``.
    """
    return os.path.join(dirpath_temp, "resources_" + config)


def dirpath_config(dirpath_temp, config):
    """
    Return Blender's configuration directory for ``config``.
    """
    return os.path.join(dirpath_resources(dirpath_temp, config), "config")


# -----------------------------------------------------------------------------
# Run Inside Blender
#
# NOTE: these run inside Blender, invoked by the tests as `--python <this file>`.
# Splitting them into their own script means duplicating the constants & state
# they share with the tests or moving them into a third module. The paths, the
# scene name & the output prefix must agree exactly for the tests to prove
# anything, so keep them here while there are only two modes.

def blender_setup(dirpath_temp, config):
    """
    Save the blend-files in ``dirpath_temp`` & the preferences for ``config``.
    """
    import bpy

    use_autoexec, use_glob = CONFIGS[config]

    # Saving writes the preferences, an empty directory proves they land in the
    # temporary `BLENDER_USER_RESOURCES`, never the user's own configuration.
    dirpath = bpy.utils.user_resource('CONFIG', create=True)
    if os.listdir(dirpath):
        raise Exception("Expected an empty user configuration in {!r}".format(dirpath))

    prefs = bpy.context.preferences
    prefs.use_preferences_save = False
    prefs.filepaths.use_scripts_auto_execute = use_autoexec
    prefs.filepaths.file_preview_type = 'NONE'

    # The "+" button in the preferences creates an empty entry, it must never match.
    prefs.autoexec_paths.new()

    path_cmp = prefs.autoexec_paths.new()
    if use_glob:
        path_cmp.use_glob = True
        # No path separator, so the same pattern matches on all platforms.
        path_cmp.path = "*" + BASE_UNTRUSTED + "*"
    else:
        path_cmp.path = os.path.join(dirpath_temp, BASE_UNTRUSTED) + os.sep

    # Saved into every file, running it renames the scene so it can be detected.
    text = bpy.data.texts.new(TEXT_SCRIPT)
    text.write(TEXT_SCRIPT_BODY)
    text.use_module = True

    for filepath in blend_filepaths(dirpath_temp).values():
        os.makedirs(os.path.dirname(filepath), exist_ok=True)
        bpy.ops.wm.save_as_mainfile(filepath=filepath)

    bpy.ops.wm.save_userpref()


def blender_state_print():
    """
    Print the state of the blend-file which was opened.
    """
    import bpy

    filepath = bpy.data.filepath
    sys.stdout.write("{:s}{:s}\n".format(STATE_PREFIX, json.dumps({
        "filepath": filepath,
        "autoexec": bpy.app.autoexec,
        "autoexec_fail": bpy.app.autoexec_fail,
        "autoexec_fail_message": bpy.app.autoexec_fail_message,
        "scene_name": bpy.data.scenes[0].name,
    })))


def main_blender(argv):
    mode, *args = argv
    if mode == "setup":
        blender_setup(*args)
    elif mode == "state":
        blender_state_print()
    else:
        raise Exception("Unknown mode {!r}".format(mode))


# -----------------------------------------------------------------------------
# Tests

class TestAutoExecCLI(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.dirpath_temp_context = tempfile.TemporaryDirectory(prefix="bl_blend_autoexec_cli_")
        # Clean up even if the setup below raises, `tearDownClass` isn't called then.
        cls.addClassCleanup(cls.dirpath_temp_context.cleanup)
        # Resolve symbolic links, Blender canonicalizes paths lexically while the OS
        # resolves the working directory, see `test_pref_enabled_untrusted_relative`.
        cls.dirpath_temp = os.path.realpath(cls.dirpath_temp_context.name)

        for config in CONFIGS:
            os.makedirs(dirpath_resources(cls.dirpath_temp, config))
            proc = cls.blender_run(
                [
                    "--factory-startup",
                    "--python", FILEPATH_SELF,
                    "--",
                    "setup", cls.dirpath_temp, config,
                ],
                config=config,
            )
            if proc.returncode != 0:
                raise Exception("Setup failed: {:s}\n{:s}".format(cmd_as_text(proc.args), output_as_text(proc.stdout)))

            # The tests are meaningless when the preferences weren't written.
            filepath = os.path.join(dirpath_config(cls.dirpath_temp, config), USERPREF_FILE)
            if not os.path.exists(filepath):
                raise Exception("Setup did not write {!r}".format(filepath))

    @classmethod
    def blender_run(cls, args, *, config, cwd=None):
        """
        Run Blender with the preferences for ``config``.

        The user configuration is isolated, the users own preferences are never
        read or written.
        """
        # NOTE: "--factory-startup" is deliberately not one of the shared arguments,
        # the preferences the setup run saved must load. Only setup runs pass it.
        cmd = [
            BLENDER_BIN,
            "--background",
            "--console-crash-handler",
            "--debug-exit-on-error",
            "--python-exit-code", "1",
            *args,
        ]
        env = {
            **os.environ,
            "BLENDER_USER_RESOURCES": dirpath_resources(cls.dirpath_temp, config),
        }
        # These take precedence over `BLENDER_USER_RESOURCES` so the user's own
        # configuration would be used, see `BKE_appdir_folder_id_ex`.
        for varname in ("BLENDER_USER_CONFIG", "BLENDER_USER_SCRIPTS", "BLENDER_USER_EXTENSIONS"):
            env.pop(varname, None)
        return subprocess.run(
            cmd,
            env=env,
            cwd=cwd,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )

    def blend_file_open(self, args, *, config, cwd=None):
        """
        Open a blend-file from the command line, returning the state once it's open.

        The arguments which print the state come last, so they run after the file is opened.
        """
        proc = self.blender_run(
            [*args, "--python", FILEPATH_SELF, "--", "state"],
            config=config,
            cwd=cwd,
        )
        output = output_as_text(proc.stdout)
        self.assertEqual(proc.returncode, 0, "{:s}\n{:s}".format(cmd_as_text(proc.args), output))

        for line in output.splitlines():
            if line.startswith(STATE_PREFIX):
                state = json.loads(line[len(STATE_PREFIX):])
                # Not part of the state, included so a failure can be repeated by hand.
                state["cmd"] = cmd_as_text(proc.args)
                return state
        self.fail("Auto-execution state not found in: {:s}\n{:s}".format(cmd_as_text(proc.args), output))

    def blend_filepath(self, filename):
        return blend_filepaths(self.dirpath_temp)[filename]

    def assert_blend_file_opened(self, state, filename, *, is_trusted, message=None):
        """
        Check ``filename`` opened with the expected trust
        & that its registered text block ran (or didn't).
        """
        message = "{:s}\nCommand: {:s}".format(message or "", state["cmd"])

        self.assertEqual(os.path.basename(state["filepath"]), filename, message)
        self.assertEqual(state["autoexec"], is_trusted, message)
        # The file's registered text block only runs when the file is trusted.
        self.assertEqual(state["scene_name"] == SCENE_SCRIPT_RAN, is_trusted, message)
        self.assertEqual(state["autoexec_fail"], not is_trusted, message)

    # -------------------------------------------------------------------------
    # Tests with Auto-Execution Preference "Enabled"

    def test_pref_enabled_trusted(self):
        """
        Open a trusted file with auto-exec preference enabled, scripts must run.

        Proves a path which isn't excluded is trusted without being asked.
        """
        state = self.blend_file_open([self.blend_filepath(BLEND_TRUSTED)], config=CONFIG_AUTOEXEC_ON)

        self.assert_blend_file_opened(
            state,
            BLEND_TRUSTED,
            is_trusted=True,
            message="A path which isn't excluded must run scripts from the command line",
        )

    def test_pref_enabled_untrusted(self):
        """
        Open an excluded file with auto-exec pre enabled, scripts must not run.

        Proves the excluded paths apply when opening from the command line.
        """
        state = self.blend_file_open([self.blend_filepath(BLEND_UNTRUSTED)], config=CONFIG_AUTOEXEC_ON)

        self.assert_blend_file_opened(
            state,
            BLEND_UNTRUSTED,
            is_trusted=False,
            message="An excluded path must not run scripts from the command line",
        )

    def test_pref_enabled_untrusted_sub_dir(self):
        """
        Open a file below an excluded directory, auto-exec preference enabled, scripts must not run.

        Proves excluding a directory excludes everything under it.
        """
        state = self.blend_file_open([self.blend_filepath(BLEND_UNTRUSTED_SUB)], config=CONFIG_AUTOEXEC_ON)

        self.assert_blend_file_opened(
            state,
            BLEND_UNTRUSTED_SUB,
            is_trusted=False,
            message="A sub-directory of an excluded path must not run scripts",
        )

    def test_pref_enabled_untrusted_relative(self):
        """
        Open an excluded file by a relative path, auto-exec preference enabled, scripts must not run.

        Proves the path is made absolute before the excluded paths are checked.
        """
        state = self.blend_file_open(
            [BLEND_UNTRUSTED],
            config=CONFIG_AUTOEXEC_ON,
            cwd=os.path.dirname(self.blend_filepath(BLEND_UNTRUSTED)),
        )

        self.assert_blend_file_opened(
            state,
            BLEND_UNTRUSTED,
            is_trusted=False,
            message="An excluded path must not run scripts when opened by a relative path",
        )

    def test_pref_enabled_untrusted_path_escape(self):
        """
        Open an excluded file by a path which steps into it, auto-exec preference enabled, scripts must not run.

        Proves the path is normalized before matching, an excluded path can't be spelled around.
        """
        filepath = os.path.join(self.dirpath_temp, BASE_TRUSTED, "..", BASE_UNTRUSTED, BLEND_UNTRUSTED)
        state = self.blend_file_open([filepath], config=CONFIG_AUTOEXEC_ON)

        self.assert_blend_file_opened(
            state,
            BLEND_UNTRUSTED,
            is_trusted=False,
            message="An excluded path must not run scripts when it is stepped into",
        )

    def test_pref_enabled_untrusted_glob(self):
        """
        Open a file excluded by a glob, auto-exec preference enabled, scripts must not run.

        Proves excluded paths with wild-cards are matched.
        """
        state = self.blend_file_open(
            [self.blend_filepath(BLEND_UNTRUSTED)],
            config=CONFIG_AUTOEXEC_ON_GLOB,
        )

        self.assert_blend_file_opened(
            state,
            BLEND_UNTRUSTED,
            is_trusted=False,
            message="A path excluded by a glob must not run scripts",
        )

    def test_pref_enabled_trusted_glob(self):
        """
        Open a trusted file while a glob excludes another directory, auto-exec preference enabled, scripts must run.

        Proves the glob only matches the directory it's meant to.
        """
        state = self.blend_file_open(
            [self.blend_filepath(BLEND_TRUSTED)],
            config=CONFIG_AUTOEXEC_ON_GLOB,
        )

        self.assert_blend_file_opened(
            state,
            BLEND_TRUSTED,
            is_trusted=True,
            message="A glob must not exclude paths it doesn't match",
        )

    def test_pref_enabled_untrusted_enable_autoexec(self):
        """
        Open an excluded file with "--enable-autoexec", auto-exec preference enabled, scripts must run.

        Proves the command line overrides the excluded paths.
        """
        state = self.blend_file_open(
            ["--enable-autoexec", self.blend_filepath(BLEND_UNTRUSTED)],
            config=CONFIG_AUTOEXEC_ON,
        )

        self.assert_blend_file_opened(
            state,
            BLEND_UNTRUSTED,
            is_trusted=True,
            message="\"--enable-autoexec\" must run scripts from an excluded path",
        )

    def test_pref_enabled_untrusted_enable_autoexec_after_filepath(self):
        """
        Open an excluded file with "--enable-autoexec" after the file path, scripts must run.

        Proves the override applies wherever it is on the command line.
        """
        state = self.blend_file_open(
            [self.blend_filepath(BLEND_UNTRUSTED), "--enable-autoexec"],
            config=CONFIG_AUTOEXEC_ON,
        )

        self.assert_blend_file_opened(
            state,
            BLEND_UNTRUSTED,
            is_trusted=True,
            message="\"--enable-autoexec\" must apply when it follows the file to open",
        )

    def test_pref_enabled_trusted_disable_autoexec(self):
        """
        Open a trusted file with "--disable-autoexec", auto-exec preference enabled, scripts must not run.

        Proves the command line overrides the preference.
        """
        state = self.blend_file_open(
            ["--disable-autoexec", self.blend_filepath(BLEND_TRUSTED)],
            config=CONFIG_AUTOEXEC_ON,
        )

        self.assert_blend_file_opened(
            state,
            BLEND_TRUSTED,
            is_trusted=False,
            message="\"--disable-autoexec\" must not run scripts from a trusted path",
        )

    # -------------------------------------------------------------------------
    # Tests with Auto-Execution Preference "Disabled"

    def test_pref_disabled_trusted(self):
        """
        Open a trusted file with auto-exec preference disabled, scripts must not run.

        Proves the preference alone stops them, a trusted path rules out the excluded paths.
        """
        state = self.blend_file_open([self.blend_filepath(BLEND_TRUSTED)], config=CONFIG_AUTOEXEC_OFF)

        self.assert_blend_file_opened(
            state,
            BLEND_TRUSTED,
            is_trusted=False,
            message="The preference not to run scripts must be respected from the command line",
        )

    def test_pref_disabled_trusted_enable_autoexec(self):
        """
        Open a trusted file with "--enable-autoexec", auto-exec preference disabled, scripts must run.

        Proves the command line overrides the preference.
        """
        state = self.blend_file_open(
            ["--enable-autoexec", self.blend_filepath(BLEND_TRUSTED)],
            config=CONFIG_AUTOEXEC_OFF,
        )

        self.assert_blend_file_opened(
            state,
            BLEND_TRUSTED,
            is_trusted=True,
            message="\"--enable-autoexec\" must override the preference not to run scripts",
        )

    def test_pref_disabled_untrusted_enable_autoexec(self):
        """
        Open an excluded file with "--enable-autoexec", auto-exec preference disabled, scripts must run.

        Proves the excluded paths only apply when the preference is enabled.
        """
        state = self.blend_file_open(
            ["--enable-autoexec", self.blend_filepath(BLEND_UNTRUSTED)],
            config=CONFIG_AUTOEXEC_OFF,
        )

        self.assert_blend_file_opened(
            state,
            BLEND_UNTRUSTED,
            is_trusted=True,
            message="Excluded paths must not apply when the preference is disabled",
        )


# -----------------------------------------------------------------------------
# Entry Point

def main():
    global BLENDER_BIN

    parser = argparse.ArgumentParser()
    parser.add_argument("--blender", required=True)
    args, remaining = parser.parse_known_args()

    # Make absolute, tests which set the working directory would fail on a relative path.
    BLENDER_BIN = os.path.abspath(args.blender)

    unittest.main(argv=sys.argv[0:1] + remaining)


if __name__ == '__main__':
    if "bpy" in sys.modules:
        # Running inside Blender, its own arguments come before "--".
        main_blender(sys.argv[sys.argv.index("--") + 1:])
    else:
        main()
