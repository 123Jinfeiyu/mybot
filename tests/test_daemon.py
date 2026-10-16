import threading
import time
from pathlib import Path

import pytest

from pymybot.daemon import DaemonVariable, Slot, validate_config
from pymybot.errors import DriverError, ScriptError
from pymybot.hardware import Registry

MODULES = Path(__file__).resolve().parents[1] / "modules"


class FakeRuntime:
    def __init__(self, daemon, *, setup=None, tick=None, cleanup=None):
        self.daemon = daemon
        self.setup = setup
        self.tick = tick
        self.cleanup = cleanup
        self.thread_ids = [threading.get_ident()]
        self.calls = []
        self.closed = threading.Event()
        self.globals = {"__mpath": {"resolve": lambda path: str(MODULES / path)}}

    def load(self, path):
        self.thread_ids.append(threading.get_ident())
        assert isinstance(path, Path)
        if self.setup:
            self.setup(self.daemon)

    def has_function(self, name):
        return name == "daemon_tick" or (name == "daemon_cleanup" and self.cleanup is not None)

    def call(self, name, data=None, budget=None):
        self.thread_ids.append(threading.get_ident())
        self.calls.append((name, data))
        assert data is None  # Both daemon functions receive zero Lua arguments.
        if budget:
            budget.check()
        if name == "daemon_tick":
            if self.tick:
                self.tick(self.daemon)
            else:
                self.daemon.slot.publish([1.5, -2.25])
        elif self.cleanup:
            self.cleanup(self.daemon)

    def close(self):
        self.thread_ids.append(threading.get_ident())
        self.closed.set()


def fake_daemon(name="probe", config=None, registry=None, stop_event=None, **hooks):
    registry = registry if registry is not None else Registry()
    runtimes = []

    def factory(**kwargs):
        runtime = FakeRuntime(**kwargs, **hooks)
        runtimes.append(runtime)
        return runtime

    daemon = DaemonVariable(name, {"chunk": "lib/daemon_probe.lua", **(config or {})},
                            registry, factory, stop_event or threading.Event())
    registry.add(daemon)
    return daemon, runtimes


@pytest.mark.parametrize("key,value", [
    ("maxvals", "0"), ("maxvals", "1025"), ("maxvals", "1.5"), ("maxvals", "NaN"),
    ("interval", "0"), ("interval", "3600001"), ("interval", "5ms"), ("interval", True),
    ("interval", ""), ("interval", " 5"), ("proto", "i2"), ("sign", "16x"),
    ("fields", "A||B"), ("fields", "|A"), ("fields", "A|"), ("fields", "A:bad"),
    ("fields", "A:f:f"), ("fields", ""), ("fields", ":f"), ("extra", "a;proto=i2c"),
    ("extra", "a\x00b"), ("bad=key", "1"), ("mode", "3x"), ("speed", "NaN"),
    ("sources", ""), ("sources", "a||b"), ("sources", "a,a"), ("sources", "probe"),
    ("chunk", "../escape.lua"), ("chunk", "/tmp/escape.lua"), ("chunk", "C:\\escape.lua"),
    ("devices", "SyntheticI2C@0x23@VALUE||SyntheticI2C@0x24@VALUE"),
    ("devices", "SyntheticI2C@0x80@VALUE"), ("devices", "SyntheticI2C@wat@VALUE"),
    ("devices", "SyntheticI2C@0x23@VALUE@no_equal"),
])
def test_invalid_configuration_precedes_runtime(key, value):
    with pytest.raises(ScriptError):
        fake_daemon(config={key: value})


def test_config_defaults_capacity_and_equals():
    daemon, _ = fake_daemon(config={"sign": "-", "extra": "a=b=c", "fields": "A:s16|B:f,C:float"})
    assert daemon.slot.capacity == 64
    assert daemon.interval_ms == 5
    assert "sign=" not in daemon.globals()["DAEMON_CONFIG"]
    assert "extra=a=b=c" in daemon.globals()["DAEMON_CONFIG"]
    assert validate_config({"chunk": "x.lua", "maxvals": "1024", "interval": "3600000"})
    with pytest.raises(ScriptError, match="field count"):
        validate_config({"chunk": "x.lua", "maxvals": "1", "fields": "a,b"})
    with pytest.raises(ScriptError, match="total field count"):
        validate_config({"chunk": "x.lua", "maxvals": "1", "devices": "SyntheticI2C@35@VALUE|SyntheticI2C@92@VALUE"})
    result = validate_config({"chunk": "lib/sensor_daemon_multi.lua", "proto": "virt",
                              "devices": "SyntheticI2C@35@VALUE@extra=a=b"})
    assert result["devices"].endswith("extra=a=b")


def test_sources_must_be_previous_daemons_and_keep_strong_references():
    registry = Registry()
    a, _ = fake_daemon("a", registry=registry)
    b, _ = fake_daemon("b", registry=registry)
    agg, _ = fake_daemon("agg", {"sources": " a, b "}, registry)
    assert agg.config["sources"] == "a|b"
    assert agg.globals()["DAEMON_PEER_SLOTS"] == {"a": a.slot.id, "b": b.slot.id}
    registry.remove("a")
    assert agg.peers["a"] is a
    with pytest.raises(ScriptError, match="previously"):
        fake_daemon("missing", {"sources": "unknown"}, registry)
    registry.add(type("Scalar", (), {"name": "scalar", "kind": "vars"})())
    with pytest.raises(ScriptError, match="previously"):
        fake_daemon("notdaemon", {"sources": "scalar"}, registry)
    with pytest.raises(DriverError, match="read-only"):
        agg.bridge("publish", b.slot.id, [99])
    with pytest.raises(DriverError, match="unauthorized"):
        agg.bridge("capacity", 999999999)


def test_slot_snapshot_validation_and_counter_wrap():
    slot = Slot(2)
    slot.publish([1, 2, 3])
    sample = slot.snapshot()
    assert sample["values"] == [1, 2]
    sample["values"][0] = 100
    assert slot.snapshot()["values"] == [1, 2]
    for value in (True, "1", float("inf"), float("nan"), None):
        with pytest.raises(DriverError):
            slot.publish([value])
    slot.publish([])
    assert slot.snapshot()["nvals"] == 0
    with slot.condition:
        slot.count = 0x7FFFFFFE
    slot.publish([3])
    assert slot.snapshot()["count"] == 0
    slot.fail(-8)
    slot.fail(0)
    assert slot.snapshot()["status"] == -8
    with pytest.raises(DriverError, match="permanently"):
        slot.publish([5])


def test_concurrent_publish_snapshots_are_atomic():
    slot = Slot(128)
    start = threading.Barrier(3)
    finished = threading.Event()
    failures = []

    def writer():
        start.wait()
        for n in range(1, 2001):
            slot.publish([n] * 128)
        finished.set()

    def reader():
        start.wait()
        while not finished.is_set():
            snapshot = slot.snapshot()
            values = snapshot["values"]
            if values and (len(set(values)) != 1 or values[0] != snapshot["count"]):
                failures.append(snapshot)

    threads = [threading.Thread(target=writer), threading.Thread(target=reader)]
    for thread in threads:
        thread.start()
    start.wait()
    for thread in threads:
        thread.join(3)
        assert not thread.is_alive()
    assert not failures
    assert slot.snapshot()["count"] == 2000


def test_worker_owns_runtime_and_cleanup_is_not_quiesced():
    cleaned = []
    daemon, runtimes = fake_daemon(config={"interval": "3600000"},
                                   cleanup=lambda d: cleaned.append((d.stopping, d.quiesced, d.cleaning_up)))
    main_thread = threading.get_ident()
    daemon.build()
    daemon.build()  # idempotent, no second Lua state
    try:
        sample = daemon.wait_for_sample(timeout=2)
        assert sample == {"result": "OK", "status": "OK", "count": 1, "nvals": 2,
                          "values": [1.5, -2.25], "v0": 1.5, "v1": -2.25}
        daemon.request_stop()
        assert not daemon.quiesced
        with pytest.raises(DriverError, match="stopped"):
            daemon.read()
    finally:
        before = time.monotonic()
        daemon.destroy()
    assert time.monotonic() - before < 0.5
    assert cleaned == [(True, False, True)]
    assert len(runtimes) == 1 and runtimes[0].closed.is_set()
    assert len(set(runtimes[0].thread_ids)) == 1
    assert runtimes[0].thread_ids[0] != main_thread
    daemon.destroy()
    with pytest.raises(DriverError, match="stopped"):
        daemon.build()


@pytest.mark.parametrize("phase", ["setup", "tick", "cleanup"])
def test_worker_exceptions_are_permanent_and_runtime_closed(phase):
    def fail(_):
        raise RuntimeError(f"broken {phase}")

    daemon, runtimes = fake_daemon(**{phase: fail})
    daemon.build()
    if phase == "cleanup":
        daemon.wait_for_sample(timeout=2)
        daemon.request_stop()
    assert daemon._done.wait(2)
    try:
        with pytest.raises(DriverError, match=f"broken {phase}"):
            daemon.read()
        assert runtimes[0].closed.is_set()
    finally:
        daemon.destroy()


def test_negative_status_never_returns_stale_sample():
    def tick(daemon):
        daemon.slot.publish([7])
        daemon.bridge("set_status", daemon.slot.id, -9)
        daemon.bridge("set_status", daemon.slot.id, 0)

    daemon, _ = fake_daemon(config={"interval": "3600000"}, tick=tick)
    daemon.build()
    try:
        assert daemon._done.wait(2)
        with pytest.raises(DriverError, match="status -9"):
            daemon.play({})
    finally:
        daemon.destroy()


def test_wait_is_interrupted_by_global_stop():
    stop = threading.Event()
    daemon, _ = fake_daemon(stop_event=stop, tick=lambda _: None)
    daemon.build()
    try:
        with pytest.raises(TimeoutError):
            daemon.wait_for_sample(timeout=0.01)
        stop.set()
        with pytest.raises(DriverError, match="stopped"):
            daemon.wait_for_sample(timeout=10)
    finally:
        daemon.destroy()


@pytest.mark.parametrize("phase", ["setup", "tick", "cleanup"])
def test_teardown_is_bounded_and_never_closes_running_runtime(phase):
    entered, release = threading.Event(), threading.Event()

    def block(_):
        entered.set()
        assert release.wait(3)

    daemon, runtimes = fake_daemon(**{phase: block})
    daemon.destroy_timeout = 0.04
    daemon.build()
    if phase == "cleanup":
        daemon.wait_for_sample(timeout=2)
        daemon.request_stop()
    assert entered.wait(2)
    try:
        before = time.monotonic()
        with pytest.raises(DriverError, match="teardown timed out"):
            daemon.destroy()
        assert time.monotonic() - before < 0.5
        assert daemon.quiesced
        assert not runtimes[0].closed.is_set()
        with pytest.raises(DriverError, match="teardown timed out"):
            daemon.destroy()  # does not grant a fresh grace period
    finally:
        release.set()
        assert daemon._done.wait(2)
        daemon.destroy()
    assert runtimes[0].closed.is_set()
    assert len(set(runtimes[0].thread_ids)) == 1


@pytest.fixture
def lua_factory(synthetic_modules):
    pytest.importorskip("lupa.luajit21")
    from pymybot.hal import HALManager
    from pymybot.runtime import LuaState

    hal = HALManager()
    stop = threading.Event()
    created = []

    def make(name, config, registry):
        def runtime_factory(**kwargs):
            return LuaState(synthetic_modules, hal, stop, **kwargs)
        daemon = DaemonVariable(name, config, registry, runtime_factory, stop)
        registry.add(daemon)
        created.append(daemon)
        return daemon

    yield make
    try:
        for daemon in created:
            daemon.request_stop()
        for daemon in reversed(created):
            daemon.destroy()
    finally:
        hal.close()


def test_synthetic_virtual_sensor_and_multi_peer_aggregation(lua_factory):
    registry = Registry()
    sensor = lua_factory("single", {"chunk": "lib/sensor_daemon.lua", "driver": "SyntheticI2C",
                                    "proto": "virt", "addr": "0x31", "sign": "16",
                                    "fields": "VALUE:float", "interval": "3600000"}, registry)
    multi = lua_factory("pair", {"chunk": "lib/sensor_daemon_multi.lua", "proto": "virt", "sign": "16",
                                 "devices": "SyntheticI2C@0x31@VALUE:f,TAG:f@tag=11|SyntheticI2C@0x32@VALUE:f,TAG:f@tag=29"}, registry)
    agg = lua_factory("agg", {"chunk": "lib/sensor_aggregate.lua", "sources": "single,pair",
                              "maxvals": "5"}, registry)
    sensor.build()
    multi.build()
    assert sensor.wait_for_sample(timeout=5)["values"] == [10240.25]
    assert multi.wait_for_sample(timeout=5)["values"] == [10240.25, 11, 10240.75, 29]
    agg.build()
    sample = agg.wait_for_sample(timeout=5)
    assert sample["nvals"] == 5
    assert sample["values"] == [sample[f"v{i}"] for i in range(5)]
    assert sample["values"] == [10240.25, 10240.25, 11, 10240.75, 29]
    assert all(isinstance(v, float) for v in sample["values"])
    sensor.request_stop()
    assert agg._done.wait(2)
    with pytest.raises(DriverError, match="permanently failed"):
        agg.read()


def test_original_aggregate_overflow_and_latest_not_synchronized(lua_factory):
    registry = Registry()
    a = lua_factory("a", {"chunk": "lib/daemon_probe.lua", "interval": "3600000"}, registry)
    b = lua_factory("b", {"chunk": "lib/daemon_probe.lua", "interval": "3600000"}, registry)
    # No synchronization barrier: preexisting samples with different counts work.
    a.slot.publish([1, 2])
    b.slot.publish([0])
    b.slot.publish([3])
    agg = lua_factory("agg", {"chunk": "lib/sensor_aggregate.lua", "sources": "a|b", "maxvals": "3"}, registry)
    agg.build()
    assert agg.wait_for_sample(timeout=3)["values"] == [1, 2, 3]
    small = lua_factory("small", {"chunk": "lib/sensor_aggregate.lua", "sources": "a|b", "maxvals": "2"}, registry)
    small.build()
    assert small._done.wait(2)
    with pytest.raises(DriverError, match="status -2"):
        small.read()


def test_lua_bridge_only_exposes_tables_and_numbers(lua_factory):
    registry = Registry()
    daemon = lua_factory("probe", {"chunk": "lib/daemon_probe.lua", "maxvals": "128"}, registry)
    # This test owns this separate, never-started runtime in the calling thread.
    runtime = daemon.runtime_factory(daemon=daemon)
    try:
        runtime.lua.execute('''
            local P = dofile(__mpath.resolve("lib/pthread.lua"))
            assert(type(__daemon_call) == "function")
            assert(type(DAEMON_SLOT_ADDR) == "number")
            assert(type(DAEMON_SLOT_LAYOUT) == "table")
            local slot = P.slot_attach(DAEMON_SLOT_ADDR)
            assert(type(slot) == "table" and type(slot[0]) == "table")
            assert(slot[0].capacity == 128 and slot.capacity == 128)
            assert(P.addr(P.ptr(P.addr(slot))) == P.addr(slot))
            local values = {}
            for i=1,128 do values[i] = i / 2 end
            P.publish(slot, values)
            local sample, count = P.latest(slot)
            assert(type(sample) == "table" and #sample == 128 and count == 1)
            assert(sample[128] == 64 and slot[0].values[0] == 0.5)
            local extra = P.slot_new(2)
            P.publish(extra, {1, 2, 3})
            assert(#P.latest(extra) == 2)
            assert(not pcall(P.publish, extra, {"bad"}))
            assert(not pcall(P.slot_attach, -1))
            assert(not pcall(P.slot_attach, 9007199254740992))
            P.signal_stop(extra)
            assert(extra.stop == 1 and not P.quiesced(extra))
            P.init_slot(extra)
            assert(P.should_stop(extra))
            assert(not pcall(P.latest, extra))
            P.quiesce(extra)
            assert(P.quiesced(extra))
            local big = P.slot_new(1024)
            for i=1,1024 do values[i] = i end
            P.publish(big, values)
            local large = P.latest(big)
            assert(#large == 1024 and large[1024] == 1024)
            assert(not pcall(P.slot_new, 1025))
        ''')
    finally:
        runtime.close()


def test_unmodified_runtime_embedded_model_integration(synthetic_modules):
    pytest.importorskip("lupa.luajit21")
    from pymybot.hal import HALManager
    from pymybot.runtime import LuaState

    assert all(not (root / "lib/virtbus/synthetici2c.lua").exists() for root in synthetic_modules)
    hal, stop, registry = HALManager(), threading.Event(), Registry()
    daemon = DaemonVariable("real", {"chunk": "lib/sensor_daemon.lua", "driver": "SyntheticI2C",
                                     "proto": "virt", "addr": "49", "fields": "COUNT:f|VALUE:f"},
                            registry, lambda **kw: LuaState(synthetic_modules, hal, stop, **kw), stop)
    registry.add(daemon)
    daemon.build()
    try:
        sample = daemon.wait_for_sample(timeout=3)
        count = 40960 + sample["count"]
        assert sample["values"] == [count, count / 4]
    finally:
        daemon.destroy()
        hal.close()
    assert daemon._done.is_set() and not daemon._thread.is_alive()
    assert not hal._handles and not hal._buses
