#!/usr/bin/env python3
"""检查 sim/src/action_table.cpp 与 modules/Sim.lua 的动作码表是否一致。

两套构建系统（sim/ 的独立 CMake 与 mybot 的 Lua 驱动）之间刻意不做代码生成
（见 docs/sim.md「跨构建系统不做代码生成」），动作码表因此在两侧各手写一份：
  * C++ 侧单一事实源：sim/src/action_table.cpp 的 kActions[]
  * Lua 侧：modules/Sim.lua 的 CODES / PARAM_CODES / PARAM2_CODES / CODE_MAX

既有的交叉校验在 sim/tests/main.cpp 的 testActionTable()，但它只在开发者手动
跑 `bash scripts/build_sim.sh` 时才会执行——sim/ 不在任何 CI job 的构建范围内
（见 sim/CMakeLists.txt 顶部注释），所以那道安全网实际上从未被强制执行过。

本脚本是同一套校验逻辑的独立实现（纯文本解析，零编译依赖），接入
test/run_all_selftest.sh，让「漏改一侧」在主干守门（push 前）就被挡下，
而不必依赖人记得去构建 sim/。

用法：python3 scripts/check_sim_action_table.py
退出码：0=一致，1=发现不一致或解析失败（原因打到 stderr）。
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CPP_PATH = ROOT / "sim/src/action_table.cpp"
LUA_PATH = ROOT / "modules/Sim.lua"


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


def parse_cpp_actions():
    if not CPP_PATH.is_file():
        sys.exit(f"check_sim_action_table: 找不到 {CPP_PATH}")
    text = CPP_PATH.read_text(encoding="utf-8")

    body = brace_block(text, "kActions[] = {")
    if body is None:
        sys.exit(f"check_sim_action_table: 在 {CPP_PATH} 里没找到 kActions[] = {{ ... }}")

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
            f"check_sim_action_table: {CPP_PATH} 的 kActions[] 一条记录都没解析到，"
            "正则是不是跟表的格式脱节了？"
        )
    codes_seen = [a["code"] for a in actions]
    if len(codes_seen) != len(set(codes_seen)):
        sys.exit(f"check_sim_action_table: {CPP_PATH} 的 kActions[] 里码重复：{codes_seen}")
    return sorted(actions, key=lambda a: a["code"])


def parse_lua_table():
    if not LUA_PATH.is_file():
        sys.exit(f"check_sim_action_table: 找不到 {LUA_PATH}")
    text = LUA_PATH.read_text(encoding="utf-8")

    codes_block = brace_block(text, "local CODES =")
    if codes_block is None:
        sys.exit(f"check_sim_action_table: 在 {LUA_PATH} 里没找到 local CODES = {{ ... }}")
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


def main():
    actions = parse_cpp_actions()
    codes, params, params2, code_max = parse_lua_table()

    errors = []
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

    if errors:
        print(
            "check_sim_action_table: 动作表不一致"
            f"（{CPP_PATH.relative_to(ROOT)} vs {LUA_PATH.relative_to(ROOT)}）：",
            file=sys.stderr,
        )
        for e in errors:
            print(f"  - {e}", file=sys.stderr)
        sys.exit(1)

    print(f"check_sim_action_table: 一致（{len(actions)} 个动作码）")


if __name__ == "__main__":
    main()
