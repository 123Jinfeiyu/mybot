import threading
from pathlib import Path

import pytest

from conftest import usable_symlink
from pymybot.errors import BudgetExceeded, DriverError, ScriptError
from pymybot.hardware import Budget, ensure_ok
from pymybot.runtime import LuaState


class NoHardware:
    def call(self, operation, *args):
        raise AssertionError("Test must not touch hardware")


@pytest.fixture
def state(tmp_path):
    runtime = LuaState([tmp_path], NoHardware(), threading.Event())
    yield runtime
    runtime.close()


def test_luajit_and_python_bridge_hidden(state):
    assert state.lua.eval("jit.version").startswith("LuaJIT")
    assert state.lua.eval("python == nil and debug == nil")
    assert state.lua.eval("os.execute == nil and io.open == nil and package.loadlib == nil")
    assert state.lua.eval("require('ffi') ~= nil")
    ok, _ = state.lua.eval("pcall(require, 'python')")
    assert not ok


def test_error_recovery_and_return_contract(state, tmp_path):
    path = tmp_path / "test.lua"
    path.write_text('function call(d) if d.bad then error("bad") end return {result="OK", value=2} end')
    state.load(path)
    with pytest.raises(DriverError, match="bad"):
        state.call("call", {"bad": True})
    assert state.call("call", {}) == {"result": "OK", "value": 2}
    assert ensure_ok({"delay": 1}) == {"delay": 1}
    for value in ({}, None, {"result": "maybe"}, {"result": 4}, {"result": " ERR "}):
        with pytest.raises(DriverError):
            ensure_ok(value)


def test_values_arrays_and_cycles(state):
    value = state.lua.eval('{result="OK", arr={[0]=10,[2]=20}, empty={}, mixed={[2]=3,a=4}}')
    assert state.from_lua(value, outer=True) == {"result": "OK", "arr": [10, 20], "empty": [], "mixed": {"2": 3, "a": 4}}
    with pytest.raises(ScriptError, match="NUL"):
        state.to_lua({"bad": "x\x00y"})
    cycle = state.lua.eval("(function() local t={} t.self=t return t end)()")
    with pytest.raises(DriverError, match="nesting"):
        state.from_lua(cycle)


def test_loader_rejects_escape_and_symlinks(state, tmp_path):
    with pytest.raises(DriverError, match="outside"):
        state.load(tmp_path.parent / "outside.lua")
    with pytest.raises(ScriptError, match="escapes"):
        state._resolve("../outside.lua")
    target = tmp_path / "valid.lua"
    target.write_text("return {}")
    link = tmp_path / "linked.lua"
    if not usable_symlink(link, target):
        pytest.skip("Platform cannot create usable symbolic links")
    with pytest.raises(DriverError, match="Symlink"):
        state.load(link)


def test_budget_interrupts_lua_and_runtime_is_reusable(state, tmp_path):
    path = tmp_path / "test.lua"
    path.write_text('function call(d) if d.spin then while true do end end return {result="OK"} end')
    state.load(path)
    with pytest.raises(BudgetExceeded):
        state.call("call", {"spin": True}, budget=Budget(timeout=0.02))
    assert state.call("call", {})["result"] == "OK"


def test_secret_environment_hidden(state, monkeypatch):
    monkeypatch.setenv("BOT4CJ_LLM_KEY", "not-a-real-key")
    assert state.lua.eval("os.getenv('BOT4CJ_LLM_KEY')") is None


@pytest.mark.parametrize("operation,args", [
    ("i2c_open", ["ftdi://ftdi:232h/1"]),
    ("spi_open", ["/dev/spidev0.0"]),
    ("uart_open", ["COM3", 115200]),
    ("camera_capture", [1, {}]),
])
def test_physical_hardware_requires_opt_in(state, operation, args):
    with pytest.raises(DriverError, match="allow-hardware"):
        state._hal_call(operation, *(state.to_lua(v) for v in args))


def test_result_null_is_not_an_absent_result():
    with pytest.raises(DriverError, match="string"):
        ensure_ok({"result": None})


def test_missing_optional_module_preserves_pcall_fallback(state, tmp_path):
    path = state._resolve("optional.lua")
    assert Path(path) == tmp_path / "optional.lua"
    ok, error = state.lua.eval("function(path) return pcall(dofile, path) end")(path)
    assert not ok and "optional.lua" in error


def test_timer_sleep_honors_budget_and_cleanup_ignores_stop(tmp_path):
    import time
    from pymybot.hal import HALManager

    stop = threading.Event()
    hal = HALManager()
    with_runtime = LuaState([tmp_path], hal, stop)
    path = tmp_path / "Timer.lua"
    path.write_text('''
function call(d) __hal_call("sleep_ms", 10000); return {result="OK"} end
function destroy(d) __hal_call("sleep_ms", 1); return {result="OK"} end
''')
    try:
        with_runtime.load(path)
        started = time.monotonic()
        with pytest.raises(BudgetExceeded):
            with_runtime.call("call", {}, budget=Budget(stop, .02))
        assert time.monotonic() - started < 1
        stop.set()
        assert with_runtime.call("destroy", {})["result"] == "OK"
    finally:
        with_runtime.close()
        hal.close()
