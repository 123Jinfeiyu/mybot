import json
import os
from pathlib import Path
import re
import subprocess
import sys

import pytest

from pymybot.errors import ControllerError
from pymybot.robot import Robot

ROOT = Path(__file__).resolve().parents[1]
SKILL = ROOT / ".qoder" / "skills" / "robot-control"


def test_skill_metadata_and_scope():
    text = (SKILL / "SKILL.md").read_text(encoding="utf-8")
    front, body = text.removeprefix("---\n").split("\n---\n", 1)
    name = re.search(r"^name: (.+)$", front, re.M).group(1)
    description = re.search(r"^description: (.+)$", front, re.M).group(1)
    assert name == SKILL.name and re.fullmatch(r"[a-z0-9-]+", name)
    assert len(description) <= 1024
    assert len(body.splitlines()) < 500
    assert "TODO:" not in text
    assert "--allow-hardware" in body and "确认" in body
    assert not (SKILL / "README.md").exists()


def test_skill_wrapper_runs_from_another_directory(tmp_path):
    result = subprocess.run([sys.executable, str(SKILL / "scripts" / "control.py"), "--help"],
                            cwd=tmp_path, capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stderr
    assert "check" in result.stdout and "run" in result.stdout
    assert "--mcp-backend" not in result.stdout


def test_skill_authoring_references_are_self_contained():
    text = (SKILL / "SKILL.md").read_text(encoding="utf-8")
    references = re.findall(r"\]\((references/[^)]+)\)", text)
    assert len(references) == 3
    for relative in references:
        reference = SKILL / relative
        content = reference.read_text(encoding="utf-8")
        assert content.strip() and "TODO:" not in content
        assert "/home/qing/" not in content
    assert "不预置" in text and "按需" in text


def test_initial_assets_are_chip_free():
    expected = {"I2C", "SPI", "UART", "Camera", "Timer", "Relay", "dummy",
                "Ema", "Kalman", "Monitor", "bitcompat"}
    assert {path.stem for path in (ROOT / "modules").glob("*.lua")} == expected
    assert {path.relative_to(ROOT / "robots").as_posix()
            for path in (ROOT / "robots").rglob("*.sql")} == {"relay-test.sql"}
    assert {path.relative_to(ROOT / "scripts").as_posix()
            for path in (ROOT / "scripts").rglob("*.mt")} == {"relay-test.mt"}
    for name in ("bmi270_config.lua", "hal_backend_glibc.lua", "asound.lua", "unix_socket_ffi.lua"):
        assert not (ROOT / "modules" / "lib" / name).exists()


@pytest.fixture
def authoring_example(tmp_path):
    text = (SKILL / "references" / "authoring-example.md").read_text(encoding="utf-8")
    blocks = dict(re.findall(r"```(lua|sql|mt)\n(.*?)\n```", text, re.S))
    assert set(blocks) == {"lua", "sql", "mt"}
    for language, name in (("lua", "LoopbackProbe.lua"), ("sql", "loopback.sql"), ("mt", "loopback.mt")):
        (tmp_path / name).write_text(blocks[language], encoding="utf-8")
    return tmp_path, blocks


def test_authoring_example_generates_and_runs_through_cli(authoring_example):
    directory, _ = authoring_example
    args = ["--robot", str(directory / "loopback.sql"), "--input", str(directory / "loopback.mt"),
            "--module-path", str(directory), "--module-path", str(ROOT / "modules")]
    results = []
    for command in ("check", "run"):
        result = subprocess.run([sys.executable, str(SKILL / "scripts" / "control.py"), command, *args],
                                cwd=directory, capture_output=True, text=True, timeout=10)
        assert result.returncode == 0, result.stderr + result.stdout
        results.append(json.loads(result.stdout))
    assert results[0]["valid"] and results[0]["activated"] is False
    assert [output["rx"] for output in results[1]["outputs"]] == [0, 127, 128, 255]


@pytest.fixture
def external_workspace(authoring_example):
    directory, blocks = authoring_example
    workspace = directory / "home" / "robots" / "task"
    for name, language in (("modules/LoopbackProbe.lua", "lua"), ("robots/loopback.sql", "sql"),
                           ("scripts/loopback.mt", "mt")):
        target = workspace / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(blocks[language], encoding="utf-8")
    return workspace


def run_control(workspace, *extra, env=None):
    return subprocess.run([sys.executable, str(SKILL / "scripts" / "control.py"), "run",
                           "--robot", "robots/loopback.sql", "--input", "scripts/loopback.mt",
                           "--timeout", "5", *extra],
                          cwd=workspace, capture_output=True, text=True, timeout=10, env=env)


def test_external_workspace_runs_with_explicit_module_paths(external_workspace):
    result = run_control(external_workspace, "--module-path", "modules", "--module-path", str(ROOT / "modules"))
    assert result.returncode == 0, result.stderr + result.stdout
    assert [output["rx"] for output in json.loads(result.stdout)["outputs"]] == [0, 127, 128, 255]


def test_external_workspace_runs_with_module_path_env(external_workspace):
    env = {**os.environ, "BOT4CJ_MODULE_PATH": os.pathsep.join([str(external_workspace / "modules"), str(ROOT / "modules")])}
    result = run_control(external_workspace, env=env)
    assert result.returncode == 0, result.stderr + result.stdout
    assert [output["rx"] for output in json.loads(result.stdout)["outputs"]] == [0, 127, 128, 255]


def test_external_workspace_without_base_modules_fails_clearly(external_workspace):
    result = run_control(external_workspace, "--module-path", "modules")
    assert result.returncode == 1
    assert "Lua module not found" in json.loads(result.stdout)["error"]["message"]


def test_skill_documents_external_workspace():
    text = (SKILL / "SKILL.md").read_text(encoding="utf-8")
    assert "## 在项目外目录工作" in text
    assert "BOT4CJ_MODULE_PATH" in text
    assert "control.py" in text and "import pymybot" in text


@pytest.mark.parametrize("value", ["-1", "256", "1.5", "bad"])
def test_authoring_example_rejects_invalid_bytes_without_writing(authoring_example, monkeypatch, value):
    directory, blocks = authoring_example
    with Robot(blocks["sql"], module_paths=[directory, ROOT / "modules"]) as robot:
        call = robot.hal.call

        def observed(operation, *args):
            assert operation != "uart_write", "Invalid bytes must be rejected before I/O"
            return call(operation, *args)

        monkeypatch.setattr(robot.hal, "call", observed)
        with pytest.raises(ControllerError, match="integer byte"):
            robot.run("@Action\necho.value;\n$(input);\n@", vars={"input": value})
    assert robot.state == "Inactive"
    assert not robot.hal._handles and not robot.hal._buses


def test_camera_authoring_example_with_real_opencv(tmp_path, monkeypatch):
    cv2 = pytest.importorskip("cv2")
    np = pytest.importorskip("numpy")
    text = (SKILL / "references" / "script-authoring.md").read_text(encoding="utf-8")
    sql = re.findall(r"```sql\n(.*?)\n```", text, re.S)[-1]
    action = re.findall(r"```mt\n(.*?)\n```", text, re.S)[-1]
    ok, encoded = cv2.imencode(".png", np.zeros((8, 12, 3), dtype=np.uint8))
    assert ok
    (tmp_path / "photo.png").write_bytes(encoded.tobytes())

    def forbidden(*args):
        pytest.fail("File-input example must not open a camera")

    monkeypatch.setattr(cv2, "VideoCapture", forbidden)
    with Robot(sql, camera_src_root=tmp_path) as robot:
        result = robot.run(action)[-1]
        assert result["result"] == "OK" and result["mime"] == "image/png"
        assert (result["width"], result["height"]) == (12, 8)
        assert result["b64"]
        with pytest.raises(ControllerError):
            robot.run(action.replace("photo.png", "missing.png"))
    assert not robot.hal._handles


def test_asset_build_discards_stale_drivers(tmp_path):
    stale = tmp_path / "pymybot" / "assets" / "modules" / "LegacyDevice.lua"
    stale.parent.mkdir(parents=True)
    stale.write_text("old build artifact", encoding="utf-8")
    code = '''
import sys
from setuptools import Distribution
from build_support import BuildPy
command = BuildPy(Distribution({"packages": ["pymybot"], "script_name": "setup.py"}))
command.build_lib = sys.argv[1]
command.ensure_finalized()
command.run()
'''
    result = subprocess.run([sys.executable, "-c", code, str(tmp_path)], cwd=ROOT,
                            capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stderr
    assert not stale.exists()
    expected = {path.relative_to(ROOT).as_posix()
                for directory, suffix in (("modules", "*.lua"), ("robots", "*.sql"), ("scripts", "*.mt"))
                for path in (ROOT / directory).rglob(suffix)}
    assets = tmp_path / "pymybot" / "assets"
    assert {path.relative_to(assets).as_posix() for path in assets.rglob("*") if path.is_file()} == expected
