#!/usr/bin/env python3
"""检查 sim/ 与宿主侧（C++ / Lua）之间三组手写双份的契约是否一致。

两套构建系统（sim/ 的独立 CMake 与 mybot 的 Lua 驱动）之间刻意不做代码生成
（见 docs/sim.md「跨构建系统不做代码生成」），于是同一份契约在两侧各手写一份。
既有的交叉校验在 sim/tests/main.cpp（testActionTable / testSharedConstants），
但它只在开发者手动跑 `bash scripts/build_sim.sh` 时才会执行——sim/ 不在任何
CI job 的构建范围内（见 sim/CMakeLists.txt 顶部注释），所以那道安全网实际上
从未被强制执行过。

本脚本是同一批校验的独立实现（纯文本解析，零编译依赖），接入
test/run_all_selftest.sh，让「漏改一侧」在主干守门（push 前）就被挡下，
而不必依赖人记得去构建 sim/。三组契约：

  1. 动作码表（testActionTable）
     * C++ 侧单一事实源：sim/src/action_table.cpp 的 kActions[]
     * Lua 侧：modules/Sim.lua 的 CODES / PARAM_CODES / PARAM2_CODES / CODE_MAX
  2. 三条 AF_UNIX 缺省 socket 路径（testSharedConstants）
     * C++ 侧：sim/src/sim_config.h 的 kDefault*SocketPath
     * Lua 侧：modules/Sim.lua 的 kDefaultSock / kSensorDefaultSock
       （Camera.lua 的拍照路径只取自 env、无硬编码缺省，故不参与比对）
  3. 一问一答桥的请求关键字
     * sim 侧：sim/src/{sensor,capture}.cpp 传给 RequestResponseBridge 的 keyword
     * 宿主侧：modules/Sim.lua 的 range()、src/agentimage.cpp、
       modules/Camera.lua 的 capture_live() 里发出的 "<word>\\n"

用法：python3 scripts/check_sim_contract.py
退出码：0=一致，1=发现不一致或解析失败（原因打到 stderr）。
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CPP_PATH = ROOT / "sim/src/action_table.cpp"
LUA_PATH = ROOT / "modules/Sim.lua"
SIM_CONFIG_PATH = ROOT / "sim/src/sim_config.h"

PREFIX = "check_sim_contract"


def read(rel_or_path):
    p = Path(rel_or_path) if isinstance(rel_or_path, Path) else ROOT / rel_or_path
    if not p.is_file():
        sys.exit(f"{PREFIX}: 找不到 {p}")
    return p.read_text(encoding="utf-8")


def rel(path):
    return Path(path).relative_to(ROOT) if Path(path).is_absolute() else Path(path)


def brace_block(text: str, marker: str):
    """取 marker 之后第一个平衡的 { ... } 块内部内容；找不到返回 None。
    与 sim/tests/main.cpp 的 braceBlock() 同一个算法，两边各自独立实现，
    用于交叉印证「脚本本身没写错」。
    """
    m = text.find(marker)
    if m == -1:
        return None
    open_idx = text.find("{", m)
    if open_idx == -1:
        return None
    depth = 0
    for i in range(open_idx, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[open_idx + 1 : i]
    return None


# ── 1) 动作码表 ────────────────────────────────────────────────────────


def parse_cpp_actions():
    text = read(CPP_PATH)

    body = brace_block(text, "kActions[] = {")
    if body is None:
        sys.exit(f"{PREFIX}: 在 {rel(CPP_PATH)} 里没找到 kActions[] = {{ ... }}")

    # 每行形如：
    #   {0, "stop", nullptr, ActionKind::Continuous, false, false, SimCommand::Stop, SimEvent::Type::Pick},
    #   {2, "backward", "back", ActionKind::Continuous, false, false, ...},
    row_re = re.compile(
        r"\{\s*(\d+)\s*,\s*\"([^\"]+)\"\s*,\s*(?:nullptr|\"[^\"]*\")\s*,"
        r"\s*ActionKind::(Continuous|OneShot)\s*,\s*(true|false)\s*,\s*(true|false)\s*,"
    )
    actions = []
    for rm in row_re.finditer(body):
        actions.append(
            {
                "code": int(rm.group(1)),
                "wire": rm.group(2),
                "kind": rm.group(3),
                "needsParam": rm.group(4) == "true",
                "needsParam2": rm.group(5) == "true",
            }
        )
    if not actions:
        sys.exit(
            f"{PREFIX}: {rel(CPP_PATH)} 的 kActions[] 一条记录都没解析到，"
            "正则是不是跟表的格式脱节了？"
        )
    codes_seen = [a["code"] for a in actions]
    if len(codes_seen) != len(set(codes_seen)):
        sys.exit(f"{PREFIX}: {rel(CPP_PATH)} 的 kActions[] 里码重复：{codes_seen}")
    return sorted(actions, key=lambda a: a["code"])


def parse_lua_table():
    text = read(LUA_PATH)

    codes_block = brace_block(text, "local CODES =")
    if codes_block is None:
        sys.exit(f"{PREFIX}: 在 {rel(LUA_PATH)} 里没找到 local CODES = {{ ... }}")
    codes = {}
    for m in re.finditer(r"\[(\d+)\]\s*=\s*\"([^\"]+)\"", codes_block):
        codes[int(m.group(1))] = m.group(2)

    params_block = brace_block(text, "local PARAM_CODES =") or ""
    params = {int(m.group(1)) for m in re.finditer(r"\[(\d+)\]\s*=\s*true", params_block)}

    params2_block = brace_block(text, "local PARAM2_CODES =") or ""
    params2 = {int(m.group(1)) for m in re.finditer(r"\[(\d+)\]\s*=\s*true", params2_block)}

    cm = re.search(r"CODE_MAX\s*=\s*(\d+)", text)
    code_max = int(cm.group(1)) if cm else None

    return codes, params, params2, code_max


def check_action_table(errors):
    actions = parse_cpp_actions()
    codes, params, params2, code_max = parse_lua_table()

    cpp_max = max(a["code"] for a in actions)
    if code_max != cpp_max:
        errors.append(f"CODE_MAX 不一致：Lua CODE_MAX={code_max}，C++ 表最大码={cpp_max}")
    if len(codes) != len(actions):
        errors.append(
            f"CODES 条目数不一致：Lua CODES 有 {len(codes)} 条，C++ 表有 {len(actions)} 条"
        )

    for a in actions:
        code, wire = a["code"], a["wire"]
        lua_name = codes.get(code)
        if lua_name is None:
            errors.append(f"code={code}：Lua CODES 缺这一项（C++ wireName='{wire}'）")
        elif lua_name != wire:
            errors.append(f"code={code}：wireName 不一致（C++='{wire}'，Lua='{lua_name}'）")

        lua_needs_param = code in params
        if lua_needs_param != a["needsParam"]:
            errors.append(
                f"code={code}（{wire}）：needsParam 不一致"
                f"（C++={a['needsParam']}，Lua PARAM_CODES={lua_needs_param}）"
            )

        lua_needs_param2 = code in params2
        if lua_needs_param2 != a["needsParam2"]:
            errors.append(
                f"code={code}（{wire}）：needsParam2 不一致"
                f"（C++={a['needsParam2']}，Lua PARAM2_CODES={lua_needs_param2}）"
            )

    extra = sorted(set(codes) - {a["code"] for a in actions})
    if extra:
        errors.append(f"Lua CODES 里有 C++ 表里不存在的码：{extra}")

    return len(actions)


# ── 2) 三条缺省 socket 路径 ────────────────────────────────────────────

# C++ 侧常量名 → (Lua 侧变量名或 None)。拍照路径在 Lua 侧无硬编码缺省
# （Camera.lua 只从 MYBOT_SIM_CAPTURE_SOCK 取），故只参与「互不相同 / 均为
# /tmp 绝对路径」两条自检，不做跨语言比对。
SOCKET_PATHS = [
    ("kDefaultSocketPath", "kDefaultSock"),
    ("kDefaultSensorSocketPath", "kSensorDefaultSock"),
    ("kDefaultCaptureSocketPath", None),
]


def check_socket_paths(errors):
    cpp_text = read(SIM_CONFIG_PATH)
    lua_text = read(LUA_PATH)

    paths = {}
    for cpp_name, lua_name in SOCKET_PATHS:
        m = re.search(
            r"inline constexpr const char\*\s+%s\s*=\s*\"([^\"]+)\";" % cpp_name, cpp_text
        )
        if not m:
            sys.exit(f"{PREFIX}: {rel(SIM_CONFIG_PATH)} 里找不到 {cpp_name} 的定义")
        cpp_val = m.group(1)
        paths[cpp_name] = cpp_val

        if lua_name is None:
            continue
        lm = re.search(r"local\s+%s\s*=\s*\"([^\"]+)\"" % lua_name, lua_text)
        if not lm:
            sys.exit(f"{PREFIX}: {rel(LUA_PATH)} 里找不到 local {lua_name} 的定义")
        if lm.group(1) != cpp_val:
            errors.append(
                f"缺省 socket 路径不一致（{cpp_name} vs {lua_name}）："
                f"C++='{cpp_val}'，Lua='{lm.group(1)}'"
            )

    vals = list(paths.values())
    if len(set(vals)) != len(vals):
        errors.append(f"三条缺省 socket 路径有重复（复制粘贴串台）：{paths}")
    for name, v in paths.items():
        if not v.startswith("/tmp/"):
            errors.append(f"{name} 不是 /tmp 下的绝对路径：'{v}'")

    return len(vals)


# ── 3) 一问一答桥的请求关键字 ──────────────────────────────────────────

# 每座桥：sim 侧的 Bridge 构造函数把 keyword 传给 RequestResponseBridge
# （第三个实参）；宿主侧的发送点各写一份 "<word>\n" 字面量。发送点用
# (文件, 定位锚) 描述——锚是所在函数头，避免同文件里多个 req 字面量串台；
# 锚为 None 表示全文件只允许出现一处。
REQUEST_BRIDGES = [
    {
        "bridge": "sim/src/sensor.cpp",
        "senders": [
            ("modules/Sim.lua", "function range("),
            ("src/agentimage.cpp", None),
        ],
    },
    {
        "bridge": "sim/src/capture.cpp",
        "senders": [("modules/Camera.lua", "local function capture_live(")],
    },
]

# 发送点形态：Lua 的 `local req = "range\n"` 与 C++ 的 `const char* req = "range\n";`
# 共用这个模式（源码里是真反斜杠 + n 两个字符）。
SEND_RE = re.compile(r"req\s*=\s*\"([A-Za-z]+)\\n\"")
KEYWORD_RE = re.compile(r"RequestResponseBridge\(\s*socketPath\s*,\s*\"[^\"]*\"\s*,\s*\"([^\"]+)\"")


def check_request_words(errors):
    checked = 0
    for spec in REQUEST_BRIDGES:
        bridge_text = read(spec["bridge"])
        km = KEYWORD_RE.search(bridge_text)
        if not km:
            sys.exit(
                f"{PREFIX}: {spec['bridge']} 里找不到 RequestResponseBridge 的 keyword 实参"
                "（构造函数形态变了？请同步本脚本的 KEYWORD_RE）"
            )
        keyword = km.group(1)

        for sender, anchor in spec["senders"]:
            text = read(sender)
            start = 0
            if anchor is not None:
                start = text.find(anchor)
                if start == -1:
                    sys.exit(f"{PREFIX}: {sender} 里找不到定位锚 '{anchor}'")
            m = SEND_RE.search(text, start)
            if not m:
                sys.exit(
                    f"{PREFIX}: {sender}（锚 '{anchor}'）之后找不到 "
                    'req = "<word>\\n" 发送点——形态变了请同步本脚本的 SEND_RE'
                )
            if m.group(1) != keyword:
                errors.append(
                    f"请求关键字不一致：{spec['bridge']} 的 keyword='{keyword}'，"
                    f"{sender} 发的是 '{m.group(1)}'"
                )
            checked += 1
    return checked


def main():
    errors = []
    n_actions = check_action_table(errors)
    n_paths = check_socket_paths(errors)
    n_senders = check_request_words(errors)

    if errors:
        print(f"{PREFIX}: 跨构建系统契约不一致：", file=sys.stderr)
        for e in errors:
            print(f"  - {e}", file=sys.stderr)
        sys.exit(1)

    print(
        f"{PREFIX}: 一致（{n_actions} 个动作码 / {n_paths} 条缺省 socket 路径"
        f" / {n_senders} 个请求关键字发送点）"
    )


if __name__ == "__main__":
    main()
