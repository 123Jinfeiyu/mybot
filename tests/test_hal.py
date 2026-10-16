"""Hardware-free tests of portable transports and the original Lua driver ABI."""
from __future__ import annotations

import base64
from concurrent.futures import ThreadPoolExecutor
import importlib
from pathlib import Path
import struct
import sys
import threading
import time
from types import SimpleNamespace
import zlib

import pytest

from pymybot.hal import HALManager

ROOT = Path(__file__).resolve().parents[1]
MODULES = ROOT / "modules"
I2C_URL = "ftdi://ftdi:232h:TEST/1"
SPI_URL = "ftdi://ftdi:2232h:TEST/2"


@pytest.fixture
def manager():
    m = HALManager()
    yield m
    m.close()


@pytest.fixture
def ftdi(monkeypatch):
    instances = []

    class Port:
        def __init__(self, controller, address):
            self.controller, self.address = controller, address
            self.mode, self.frequency = 0, 1000000

        def set_frequency(self, value):
            self.frequency = value

        def set_mode(self, value):
            self.mode = value

        def exchange(self, data, count=None, **kwargs):
            c = self.controller
            assert not c.active, "transactions on a shared bus must be serialized"
            c.active = True
            try:
                if c.pause:
                    time.sleep(.001)
                c.log.append(("exchange", self.address, list(data), count, kwargs,
                              self.mode, self.frequency))
                if c.fail:
                    raise IOError("USB disconnected")
                if c.short:
                    return b""
                return bytes(b ^ 0xFF for b in data) if kwargs.get("duplex") else bytes(range(count))
            finally:
                c.active = False

        def read(self, count):
            self.controller.log.append(("read", self.address, count))
            return bytes(range(count))

        def write(self, data):
            self.controller.log.append(("write", self.address, list(data)))

        def force_select(self, level):
            self.controller.log.append(("select", level))

    class Controller:
        def __init__(self, **kwargs):
            self.log, self.ports = [], {}
            self.active = self.pause = self.fail = self.short = False
            self.terminated = 0
            self.kwargs = kwargs
            instances.append(self)

        def configure(self, url):
            self.url = url
            if "BROKEN" in url:
                raise IOError("configure failed")

        def get_port(self, address=None, *, cs=None, freq=None, mode=None):
            key = cs if cs is not None else address
            if key not in self.ports:
                self.ports[key] = Port(self, key)
            return self.ports[key]

        def terminate(self):
            self.terminated += 1

    monkeypatch.setitem(sys.modules, "pyftdi.i2c", SimpleNamespace(I2cController=Controller))
    monkeypatch.setitem(sys.modules, "pyftdi.spi", SimpleNamespace(SpiController=Controller))
    return instances


def test_ftdi_i2c_atomic_shared_and_close(manager, ftdi):
    a = manager.call("i2c_open", I2C_URL)
    b = manager.call("i2c_open", I2C_URL)
    assert a >= 0 and a != b and len(ftdi) == 1
    assert manager.call("i2c_transfer", a, 0x40, [0xFF, 0], 3) == [0, 1, 2]
    assert ftdi[0].log[-1][:4] == ("exchange", 0x40, [255, 0], 3)
    assert manager.call("i2c_transfer", b, 0x41, [], 2) == [0, 1]
    assert manager.call("i2c_transfer", b, 0x41, [9], 0) == []
    assert manager.call("close_handle", a) == 0
    assert not ftdi[0].terminated
    assert manager.call("i2c_transfer", a, 0x40, [], 1) is None
    assert manager.call("i2c_transfer", b, 0x40, [], 1) == [0]
    manager.close()
    manager.close()
    assert ftdi[0].terminated == 1
    assert manager.call("i2c_open", I2C_URL) == -1


def test_i2c_limits_and_short_read(manager, ftdi):
    h = manager.call("i2c_open", I2C_URL)
    for addr, data, n in [(128, [], 1), (1, [], 17), (1, [0] * 259, 0),
                          (1, [256], 0), (1, [float("nan")], 0), (1, [], 0)]:
        assert manager.call("i2c_transfer", h, addr, data, n) is None
    assert not ftdi[0].log
    ftdi[0].short = True
    assert manager.call("i2c_transfer", h, 10, [1], 1) is None
    assert "short I2C" in manager.call("last_error")


def test_ftdi_bus_lock_and_protocol_conflict(manager, ftdi):
    a = manager.call("i2c_open", I2C_URL)
    b = manager.call("i2c_open", I2C_URL)
    assert manager.call("spi_open", I2C_URL) == -1
    assert "another bus protocol" in manager.call("last_error")
    ftdi[0].pause = True
    with ThreadPoolExecutor(max_workers=4) as pool:
        outputs = list(pool.map(lambda i: manager.call("i2c_transfer", (a, b)[i % 2], i % 127, [i], 2), range(40)))
    assert outputs == [[0, 1]] * 40


def test_spi_shared_controller_per_handle_setup_and_delay(manager, ftdi):
    a = manager.call("spi_open", SPI_URL)
    b = manager.call("spi_open", SPI_URL + "?cs=1")
    assert a >= 0 and b >= 0 and len(ftdi) == 1
    assert manager.call("spi_setup", a, 3, 8, 500000) == 0
    assert manager.call("spi_setup", b, 0, 8, 1000000) == 0
    assert manager.call("spi_transfer", a, [0, 255, 128], 1) == [255, 0, 127]
    exchange = ftdi[0].log[-2]
    assert exchange[1] == 0 and exchange[-2:] == (3, 500000)
    assert exchange[4] == dict(duplex=True, start=True, stop=False)
    assert ftdi[0].log[-1] == ("select", True)
    assert manager.call("spi_transfer", b, [1]) == [254]
    assert ftdi[0].log[-1][1] == 1
    ftdi[0].fail = True
    assert manager.call("spi_transfer", a, [0], 1) is None
    assert ftdi[0].log[-1] == ("select", True)


def test_spi_rejects_invalid_configuration(manager, ftdi):
    h = manager.call("spi_open", SPI_URL)
    for mode, bits, speed in [(1.5, 8, 1), (4, 8, 1), (0, 16, 1000), (0, 8, 0)]:
        assert manager.call("spi_setup", h, mode, bits, speed) == -1
    for data in [[], [0] * 65, [-1], [1.5], "not bytes"]:
        assert manager.call("spi_transfer", h, data) is None
    assert manager.call("spi_transfer", h, [1], 65536) is None


@pytest.mark.parametrize("path", ["ftdi://", "ftdi:///?", "ftdi://ftdi:232h/0", "ftdi://ftdi:232h/1?bad=1", "random", "/tmp/bus"])
def test_reject_bad_ftdi_routes(manager, ftdi, path):
    assert manager.call("i2c_open", path) == -1
    assert manager.call("spi_open", path) == -1
    assert not ftdi


def test_failed_configure_cleans_up(manager, ftdi):
    assert manager.call("i2c_open", "ftdi://ftdi:232h:BROKEN/1") == -1
    assert ftdi[0].terminated == 1
    assert not manager._handles and not manager._buses


def test_windows_uses_portable_routes_only(manager, ftdi, monkeypatch):
    monkeypatch.setattr(sys, "platform", "win32")
    assert manager.call("i2c_open", "/dev/i2c-1") == -1
    assert manager.call("spi_open", "/dev/spidev0.0") == -1
    assert manager.call("i2c_open", I2C_URL) >= 0
    assert manager.call("spi_open", SPI_URL) >= 0
    assert "smbus2" not in sys.modules and "spidev" not in sys.modules


def test_linux_legacy_backends_are_library_wrappers(manager, monkeypatch):
    monkeypatch.setattr(sys, "platform", "linux")
    class Message(list):
        @staticmethod
        def write(address, data):
            return Message(data)
        @staticmethod
        def read(address, count):
            return Message(range(count))
    class SMBus:
        def __init__(self, bus):
            self.bus, self.calls, self.closed = bus, [], False
        def i2c_rdwr(self, *messages):
            self.calls.append(messages)
        def close(self):
            self.closed = True
    class SPI:
        def open(self, bus, cs):
            self.bus, self.cs = bus, cs
        def xfer2(self, data, speed, delay, bits):
            return data
        def close(self):
            self.closed = True
    monkeypatch.setitem(sys.modules, "smbus2", SimpleNamespace(SMBus=SMBus, i2c_msg=Message))
    monkeypatch.setitem(sys.modules, "spidev", SimpleNamespace(SpiDev=SPI))
    i = manager.call("i2c_open", "/dev/i2c-1")
    s = manager.call("spi_open", "/dev/spidev0.1")
    assert i >= 0 and s >= 0
    assert manager.call("i2c_transfer", i, 0x40, [0xFF], 3) == [0, 1, 2]
    assert manager.call("spi_setup", s, 1, 16, 10000) == 0
    assert manager.call("spi_transfer", s, [128, 255], 1) == [128, 255]
    bus, spi = manager._get(i).device, manager._get(s).device
    assert len(bus.calls[-1]) == 2 and spi.cs == 1
    manager.close()
    assert bus.closed and spi.closed


def test_missing_libraries_fail_lazily(manager, monkeypatch):
    original = importlib.import_module
    def unavailable(name):
        if name in {"cv2", "serial", "pyftdi.i2c", "pyftdi.spi"}:
            raise ImportError("optional library missing")
        return original(name)
    monkeypatch.setattr(importlib, "import_module", unavailable)
    h = manager.call("camera_open")
    assert h >= 0
    assert manager.call("camera_capture", h, {})["result"] == "ERR"
    assert manager.call("uart_open", "COM3", 9600) == -1
    assert manager.call("i2c_open", I2C_URL) == -1
    assert manager.call("spi_open", SPI_URL) == -1
    assert "optional library missing" in manager.call("last_error")


@pytest.fixture
def serial_fake(monkeypatch):
    devices = []

    class Serial:
        def __init__(self, path, **kwargs):
            self.path, self.options = path, kwargs
            self.data = bytearray()
            self.closed = 0
            self.fail = self.short = False
            devices.append(self)

        def write(self, data):
            self.data.extend(data)
            return 0 if self.short else len(data)

        def read(self, n):
            if self.fail:
                raise IOError("unplugged")
            out = bytes(self.data[:n])
            del self.data[:n]
            return out

        def reset_input_buffer(self):
            self.data.clear()

        def close(self):
            self.closed += 1

    monkeypatch.setitem(sys.modules, "serial", SimpleNamespace(serial_for_url=Serial))
    return devices


@pytest.mark.parametrize("path", ["COM3", "com12", r"\\.\COM33", "loop://"])
def test_uart_com_urls_binary_and_sharing(manager, serial_fake, path):
    a = manager.call("uart_open", path, 115200)
    b = manager.call("uart_open", path, 115200)
    assert a >= 0 and b >= 0 and len(serial_fake) == 1
    assert serial_fake[0].options["timeout"] == 0
    assert serial_fake[0].options["write_timeout"] == 1
    assert manager.call("uart_open", path, 9600) == -1
    assert manager.call("uart_write", a, [0, 255, 128, 10]) == 4
    assert manager.call("uart_read", b, 9, 2) == [0, 255, 128, 10]
    assert manager.call("uart_read", a, 10, 4) == []  # empty is timeout, not EOF
    assert manager.call("uart_write", a, [1]) == 1
    assert manager.call("uart_flush", b) == 0
    assert manager.call("uart_read", a, 1, 0) == []
    assert manager.call("close_handle", a) == 0 and serial_fake[0].closed == 0
    assert manager.call("close_handle", b) == 0 and serial_fake[0].closed == 1


def test_uart_error_limits(manager, serial_fake):
    h = manager.call("uart_open", "COM4", 9600)
    for n, timeout in [(0, 0), (1, 60001), (1, float("inf")), (1, float("nan"))]:
        assert manager.call("uart_read", h, n, timeout) is None
    assert manager.call("uart_write", h, [0] * 513) == -1
    assert manager.call("uart_open", "socket://untrusted:1234", 9600) == -1
    assert manager.call("uart_open", "COM0", 9600) == -1
    assert manager.call("uart_open", "COM4", 42) == -1
    serial_fake[0].short = True
    assert manager.call("uart_write", h, [1]) == -1
    serial_fake[0].fail = True
    assert manager.call("uart_read", h, 1, 10) is None
    assert "unplugged" in manager.call("last_error")


def test_real_pyserial_loopback(manager):
    pytest.importorskip("serial")
    h = manager.call("uart_open", "loop://", 115200)
    assert h >= 0, manager.call("last_error")
    assert manager.call("uart_write", h, [0, 127, 128, 255]) == 4
    assert manager.call("uart_read", h, 4, 200) == [0, 127, 128, 255]
    assert manager.call("uart_read", h, 1, 5) == []
    assert manager.call("uart_flush", h) == 0


def png_bytes(width=2, height=1):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress((b"\x00" + b"\x00\x80\xff" * width) * height))
            + chunk(b"IEND", b""))


def mock_jpeg():
    # Container fixture for the fake encoder; actual decoding is tested with real cv2.
    return (b"\xff\xd8\xff\xc0\x00\x0b\x08\x00\x01\x00\x02\x01\x01\x11\x00"
            b"\xff\xda\x00\x08\x01\x01\x00\x00\x3f\x00\x12\xff\xd9")


@pytest.fixture
def cv2_fake(monkeypatch):
    captures = []
    frame = SimpleNamespace(shape=(1, 2, 3))

    class Capture:
        def __init__(self, device):
            self.device, self.settings = device, []
            self.released = 0
            self.available = True
            self.read_ok = True
            captures.append(self)

        def isOpened(self):
            return self.available

        def set(self, key, value):
            self.settings.append((key, value))
            return True

        def read(self):
            return self.read_ok, frame

        def release(self):
            self.released += 1

    module = SimpleNamespace(VideoCapture=Capture, CAP_PROP_FRAME_WIDTH=3,
                             CAP_PROP_FRAME_HEIGHT=4, IMWRITE_JPEG_QUALITY=1,
                             IMREAD_COLOR=1, captures=captures)
    module.imencode = lambda extension, image, options: (True, SimpleNamespace(tobytes=mock_jpeg))
    module.imdecode = lambda data, flags: frame
    monkeypatch.setitem(sys.modules, "cv2", module)
    monkeypatch.setitem(sys.modules, "numpy", SimpleNamespace(frombuffer=lambda data, dtype: data, uint8=int))
    return module


def test_camera_lazy_capture_fields_reconfigure_close(manager, cv2_fake):
    h = manager.call("camera_open", 0, 640, 480)
    assert h >= 0 and not cv2_fake.captures
    shot = manager.call("camera_capture", h, {})
    assert shot["result"] == "OK", shot
    assert shot["format"] == "jpeg" and shot["mime"] == "image/jpeg"
    assert shot["b64"] == shot["base64"]
    assert base64.b64decode(shot["b64"]) == mock_jpeg()
    assert (shot["width"], shot["height"], shot["bytes"]) == (2, 1, len(mock_jpeg()))
    assert manager.call("camera_capture", h, {"device": 1})["result"] == "OK"
    assert len(cv2_fake.captures) == 2 and cv2_fake.captures[0].released == 1
    assert manager.call("close_handle", h) == 0
    assert cv2_fake.captures[1].released == 1
    assert manager.call("camera_capture", h, {})["result"] == "ERR"


def test_camera_capture_failures_release_and_retry(manager, cv2_fake):
    h = manager.call("camera_open", 0, 640, 480)
    original = cv2_fake.VideoCapture

    def unavailable(device):
        cap = original(device)
        cap.available = False
        return cap

    cv2_fake.VideoCapture = unavailable
    assert manager.call("camera_capture", h, {})["result"] == "ERR"
    assert cv2_fake.captures[0].released == 1
    cv2_fake.VideoCapture = original
    assert manager.call("camera_capture", h, {})["result"] == "OK"
    cv2_fake.captures[-1].read_ok = False
    assert manager.call("camera_capture", h, {})["result"] == "ERR"
    assert cv2_fake.captures[-1].released == 1
    cv2_fake.imencode = lambda *args: (False, None)
    assert manager.call("camera_capture", h, {})["result"] == "ERR"
    assert cv2_fake.captures[-1].released == 1


def test_camera_source_sandbox_validation_and_ceiling(tmp_path, cv2_fake):
    root = tmp_path / "allowed"
    root.mkdir()
    (root / "photo.png").write_bytes(png_bytes())
    outside = tmp_path / "outside.png"
    outside.write_bytes(png_bytes())
    with_manager = HALManager(root)
    h = with_manager.call("camera_open")
    try:
        shot = with_manager.call("camera_capture", h, {"src": "photo.png"})
        assert shot["result"] == "OK" and shot["mime"] == "image/png"
        assert not cv2_fake.captures
        for src in ["../outside.png", str(outside), "missing.png"]:
            assert with_manager.call("camera_capture", h, {"src": src})["result"] == "ERR"
        (root / "bad.png").write_bytes(png_bytes()[:-2])
        assert with_manager.call("camera_capture", h, {"src": "bad.png"})["result"] == "ERR"
        (root / "bad.png").write_bytes(b"not an image")
        assert with_manager.call("camera_capture", h, {"src": "bad.png"})["result"] == "ERR"
        with_manager.MAX_IMAGE_BYTES = 10
        assert "ceiling" in with_manager.call("camera_capture", h, {"src": "photo.png"})["message"]
    finally:
        with_manager.close()
    denied = HALManager()
    try:
        h = denied.call("camera_open")
        response = denied.call("camera_capture", h, {"src": str(outside), "src_root": str(tmp_path)})
        assert response["result"] == "ERR" and "camera_src_root" in response["message"]
    finally:
        denied.close()


def test_camera_source_symlink_escape(tmp_path, cv2_fake):
    root = tmp_path / "root"
    root.mkdir()
    outside = tmp_path / "image.png"
    outside.write_bytes(png_bytes())
    try:
        (root / "escape.png").symlink_to(outside)
    except OSError:
        pytest.skip("symlinks require privileges on this platform")
    m = HALManager(root)
    try:
        h = m.call("camera_open")
        assert m.call("camera_capture", h, {"src": "escape.png"})["result"] == "ERR"
    finally:
        m.close()


def test_camera_validation_and_no_silent_simulator_fallback(manager, cv2_fake, monkeypatch):
    assert manager.call("camera_open", "https://untrusted/video", 1, 1) == -1
    assert manager.call("camera_open", 0, 0, 1) == -1
    h = manager.call("camera_open")
    assert manager.call("camera_capture", h, {"width": 10})["result"] == "ERR"
    monkeypatch.setenv("MYBOT_SIM_CAPTURE_SOCK", "/missing/socket")
    assert "not supported" in manager.call("camera_capture", h, {})["message"]
    assert not cv2_fake.captures


def test_real_cv2_image_validation(tmp_path):
    cv2 = pytest.importorskip("cv2")
    np = pytest.importorskip("numpy")
    m = HALManager(tmp_path)
    h = m.call("camera_open")
    try:
        for extension in [".png", ".jpg"]:
            ok, encoded = cv2.imencode(extension, np.zeros((8, 12, 3), dtype=np.uint8))
            assert ok
            file = tmp_path / ("image" + extension)
            file.write_bytes(encoded.tobytes())
            shot = m.call("camera_capture", h, {"src": str(file)})
            assert shot["result"] == "OK", shot
            assert (shot["width"], shot["height"]) == (12, 8)
            file.write_bytes(encoded.tobytes()[:-8])
            assert m.call("camera_capture", h, {"src": str(file)})["result"] == "ERR"
    finally:
        m.close()


def test_portable_sleep_cancellation_and_unsupported(manager):
    assert manager.call("sleep_ms", 0) == 0
    assert manager.call("sleep_ms", float("nan")) == 0
    assert manager.call("speaker_open", "default")["result"] == "ERR"
    assert manager.call("__dict__")["result"] == "ERR"
    def cancelled():
        raise InterruptedError("cancelled")
    m = HALManager(cancel_check=cancelled)
    with pytest.raises(InterruptedError, match="cancelled"):
        m.call("sleep_ms", 1000)
    m.close()


def lua_runtime(manager, monkeypatch):
    lupa = pytest.importorskip("lupa.luajit21")
    monkeypatch.setenv("BOT4CJ_MODULE_PATH", str(MODULES))
    lua = lupa.LuaRuntime(encoding="utf-8", unpack_returned_tuples=True,
                          register_eval=False, register_builtins=False)
    def from_lua(value):
        if lupa.lua_type(value) == "table":
            keys = list(value.keys())
            if keys and set(keys) == set(range(1, len(keys) + 1)):
                return [from_lua(value[i]) for i in range(1, len(keys) + 1)]
            return {k: from_lua(v) for k, v in value.items()}
        return value
    def to_lua(value):
        if isinstance(value, dict):
            return lua.table_from({k: to_lua(v) for k, v in value.items()})
        if isinstance(value, list):
            return lua.table_from([to_lua(v) for v in value])
        return value
    lua.globals()["__hal_call"] = lambda operation, *args: to_lua(manager.call(operation, *(from_lua(v) for v in args)))
    # __mpath is the host's path resolver; keeps Windows drive letters out of
    # the original source mpath.lua's colon-separated environment parsing.
    lua.globals()["__module_root"] = str(MODULES)
    lua.execute('__mpath = {resolve=function(rel) return __module_root .. "/" .. rel end}')
    lua.execute(f'dofile({str(MODULES / "lib/HAL.lua")!r})')
    lua.globals()["__hal_call"] = None
    return lua


def test_host_runtime_camera_bridge(manager, cv2_fake):
    pytest.importorskip("lupa.luajit21")
    from pymybot.runtime import LuaState
    # cv2_fake is installed: this opt-in exercises dispatch, never real hardware.
    state = LuaState([MODULES], manager, threading.Event(), allow_hardware=True)
    try:
        state.load(MODULES / "Camera.lua")
        assert state.globals["__hal_call"] is None
        assert state.call("build", {})["result"] == "OK"
        result = state.call("call", {})
        assert result["result"] == "OK" and result["b64"]
        assert state.call("destroy", {})["result"] == "OK"
    finally:
        state.close()


def test_lua_original_i2c_spi_buffers_and_utf8_bridge(manager, ftdi, monkeypatch):
    lua = lua_runtime(manager, monkeypatch)
    lua.globals()["i2c_url"] = I2C_URL
    lua.globals()["spi_url"] = SPI_URL
    lua.execute('''
        assert(__hal_call == nil)
        assert(type(__BOT4CJ_HAL.camera_capture) == "function")
        local f = i2c_open(i2c_url)
        assert(i2c_set_addr(f, 0x40))
        assert(i2c_write_reg(f, 0xFF, 0x80) == true)
        assert(i2c_write_multi(f, 1, 255, 128) == 0)
        assert(i2c_write_reg16(f, 0x1234, 128, 255) == 0)
        assert(i2c_read_reg(f, 1) == 0)
        assert(i2c_read_multi(f, 1, 3) == 258)
        assert(i2c_read_reg16(f, 0x1234, 3) == 258)
        assert(i2c_read(f, 3) == 258)
        assert(#i2c_read_bytes(f, 100) == 16)
        assert(#i2c_read_multi_bytes(f, 1, 0) == 1)
        assert(i2c_read_multi(f, 1, 7) == -1)
        assert(i2c_write_reg(f, 256, 0) == false)
        local s = spi_open(spi_url)
        assert(spi_setup(s, 0, 8, 1000000, 0) == 0)
        assert(spi_write(s, 128) == 0)
        assert(spi_read(s) == 255)
        assert(spi_write3(s, 255, 0, 128) == 0)
        assert(spi_write_multi(s, {0, 128, 255}) == 0)
        assert(spi_read_multi(s, 2) == 65535)
        local bytes = spi_xfer_full(s, {0, 128, 255})
        assert(bytes[1] == 255 and bytes[2] == 127 and bytes[3] == 0)
        assert(spi_xfer_full(s, {1.5}) == nil)
        i2c_close(f); spi_close(s)
        assert(not pcall(gpio_request_line, "anything", 1, 1))
        assert(not pcall(pwm_export, 0, 0))
    ''')
    assert all(c.terminated == 1 for c in ftdi)


def test_lua_uart_binary_transport(manager, serial_fake, monkeypatch):
    lua = lua_runtime(manager, monkeypatch)
    lua.execute('''
        local f = uart_open("COM9", 115200)
        assert(f >= 0)
        assert(uart_write(f, {0, 128, 255}) == 3)
        local b = uart_read(f, 3, 10)
        assert(b[1] == 0 and b[2] == 128 and b[3] == 255)
        assert(#uart_read(f, 1, 2) == 0)
        assert(uart_read(f, 1, math.huge) == nil)
        assert(uart_flush(f) == 0)
        assert(uart_close(f) == 0)
    ''')


def test_lua_virtual_sensor_operations_and_mixed_buses(manager, ftdi, monkeypatch):
    lua = lua_runtime(manager, monkeypatch)
    lua.globals()["i2c_url"] = I2C_URL
    lua.execute('''
        local v = __BOT4CJ_VIRTBUS
        v.attach("registers", 0x40, {
            write = function(b) assert(b[1] == 0x20) end,
            read = function(n) return {0x80, 0xFF} end,
        })
        local virtual = i2c_open("virt-i2c-0")
        local physical = i2c_open(i2c_url)
        assert(i2c_set_addr(virtual, 0x40))
        assert(i2c_set_addr(physical, 0x40))
        assert(i2c_read_multi(virtual, 0x20, 2) == 0x80FF)
        assert(i2c_read_multi(physical, 0x20, 2) == 1)
        v.attach_spi("spi", "virt-spi-0", {xfer=function(b) return b end})
        local spi = spi_open("virt-spi-0")
        assert(spi_setup(spi, 0, 8, 1000000) == 0)
        assert(spi_xfer_full(spi, {128, 255})[2] == 255)
        v.attach_uart("uart", "virt-uart-0", {on_write=function(b) return b end})
        local uart = uart_open("virt-uart-0", 9600)
        assert(uart_write(uart, {128, 255}) == 2)
        assert(uart_read(uart, 2, 0)[1] == 128)
        i2c_close(virtual); i2c_close(physical); spi_close(spi); uart_close(uart)
        assert(v.fd_count() == 0)
    ''')


def test_lua_synthetic_i2c_sensor_and_nack(manager, monkeypatch, synthetic_modules):
    lua = lua_runtime(manager, monkeypatch)
    lua.execute(f'dofile({str(synthetic_modules[0] / "SyntheticI2C.lua")!r})')
    lua.execute('''
        local model = SyntheticI2C_VIRT.new({count=40961})
        local vbus = __BOT4CJ_VIRTBUS
        vbus.attach("synthetic", 0x31, model)
        local fd = i2c_open("virt-i2c-synthetic")
        local data = {fd=fd, address=0x31, tag=19}
        assert(build(data).result == "OK")
        local value = call(data)
        assert(value.result == "OK" and value.COUNT == 40961)
        assert(value.VALUE == 10240.25 and value.TAG == 19)
        assert(model.registers[4] == 19)
        model.nack = true
        assert(build(data).message == "synthetic I2C write NACK")
        assert(call(data).message == "synthetic I2C read NACK")
        assert(i2c_write_reg(fd, 4, 1) == false)
        assert(i2c_write_multi(fd, 4, 1) == -1)
        assert(i2c_read_multi_bytes(fd, 0, 2) == nil)
        model.nack = false
        model.read_nack = true
        assert(call(data).result == "ERR")
        model.read_nack = false
        assert(call(data).COUNT == 40961)
        assert(destroy(data).result == "OK")
        i2c_close(fd)
        assert(vbus.fd_count() == 0)
    ''')


def test_windows_style_module_paths_and_cross_state_virtual_sensor(manager, monkeypatch, synthetic_modules):
    bus_lua = lua_runtime(manager, monkeypatch)
    sensor_lua = lua_runtime(manager, monkeypatch)
    for lua in (bus_lua, sensor_lua):
        lua.execute('''
            local getenv = os.getenv
            os.getenv = function(name)
                if name == "BOT4CJ_MODULE_PATH" then return "C:/robots/modules;D:/extra" end
                return getenv(name)
            end
        ''')
    bus_lua.execute(f'dofile({str(MODULES / "I2C.lua")!r})')
    data = bus_lua.table_from(dict(path="virt-i2c-0", proto="virt"))
    assert bus_lua.globals().build(data)["result"] == "OK"
    fd = bus_lua.globals().call(data)["fd"]
    sensor_lua.execute(f'dofile({str(synthetic_modules[0] / "SyntheticI2C.lua")!r})')
    data = sensor_lua.table_from(dict(fd=fd, address=0x31, proto="virt"))
    try:
        assert sensor_lua.globals().build(data)["result"] == "OK"
        assert sensor_lua.globals().call(data)["COUNT"] == 40961
        sensor_lua.execute('''
            local v = __BOT4CJ_VIRTBUS
            local adopted = false
            for _, entry in ipairs(v.txlog) do
                if entry.dir == "adopt" then adopted = true end
            end
            assert(adopted and v.fd_count() == 1)
        ''')
    finally:
        assert sensor_lua.globals().destroy(data)["result"] == "OK"
        assert bus_lua.globals().destroy(bus_lua.table())["result"] == "OK"
    for lua in (bus_lua, sensor_lua):
        lua.execute('assert(__BOT4CJ_VIRTBUS.fd_count() == 0)')


def test_lua_hal_speaker_open_is_unsupported(manager, monkeypatch):
    lua = lua_runtime(manager, monkeypatch)
    lua.execute('''
        local ok, message = pcall(speaker_open, "default")
        assert(not ok)
        assert(message:find("speaker_open is not supported", 1, true))
    ''')
    assert not manager._handles


def test_lua_existing_bus_modules_and_camera(manager, ftdi, serial_fake, cv2_fake, monkeypatch):
    for module, data in [("I2C", dict(path=I2C_URL)),
                         ("SPI", dict(path=SPI_URL, spi_mode=0, bits=8, speed=1000000, delay=0)),
                         ("UART", dict(path="COM9", baud=9600)),
                         ("Camera", dict(device=0))]:
        lua = lua_runtime(manager, monkeypatch)
        lua.execute(f'dofile({str(MODULES / (module + ".lua"))!r})')
        assert lua.globals().build(lua.table_from(data))["result"] == "OK"
        result = lua.globals().call(lua.table())
        assert result["result"] == "OK"
        if module == "Camera":
            assert result["b64"] and result["format"] == "jpeg"
            assert lua.globals().read(lua.table())["result"] == "OK"
        assert lua.globals().destroy(lua.table())["result"] == "OK"
