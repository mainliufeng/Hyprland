"""Run the complete upstream integration runner against the explicit build."""
import os
import pathlib
import shutil
import sys
from runtime import Runtime

source = pathlib.Path(__file__).resolve().parent.parent
runtime = Runtime("single-seat")
try:
    assert pathlib.Path(os.environ["MULTISEAT_HYPRTESTER"]).is_file()
    assert pathlib.Path(os.environ["MULTISEAT_PLUGIN"]).is_file()
    # Upstream clients write relative debug logs and Lua exercises both
    # config-relative and working-directory-relative require paths.
    working = runtime.base / "upstream"
    working.mkdir()
    shutil.copy2(source / "test.lua", working / "test.lua")
    shutil.copytree(source / "lua-require", working / "lua-require")
    # Keep terminal behavior deterministic without changing host settings.
    # The tests close windows directly and type shell commands using raw keys.
    kitty = pathlib.Path(runtime.env["XDG_CONFIG_HOME"]) / "kitty"
    kitty.mkdir(parents=True)
    (kitty / "kitty.conf").write_text("shell /bin/sh\nconfirm_os_window_close 0\nshell_integration disabled\n"
                                     "remember_window_size no\ninitial_window_width 640\ninitial_window_height 400\n")
    env = runtime.env | {"PWD": str(working), "KITTY_CONFIG_DIRECTORY": str(kitty)}
    runner = runtime.start([os.environ["MULTISEAT_HYPRTESTER"], "-c", str(working / "test.lua"),
                            "-b", os.environ["MULTISEAT_HYPRLAND"], "-p", os.environ["MULTISEAT_PLUGIN"], *sys.argv[1:]],
                           "upstream-hyprtester", env=env, cwd=working)
    assert runner.wait(timeout=1000) == 0, "upstream single-seat integration failed; see upstream-hyprtester.log"
    print("PASS upstream single-seat integration", flush=True)
finally:
    runtime.cleanup()
