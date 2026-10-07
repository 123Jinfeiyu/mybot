#!/usr/bin/env python3
"""检查后端工具注册表（makeAgentTools 装配的工具集）与 docs/agent.md 工具表是否同集合。

`docs/agent.md` 的「给智能体的工具」一节是**手工维护**的总表（无自动生成，同
`docs/sim.md`「跨构建系统不做代码生成」的取舍一致）；工具本身的权威定义是四个
`make*Tool` 所在 TU 里 `return AgentTool{"<name>", ...}` 的第一个字段
（`src/agenttools.cpp` / `src/agenttools_wait.cpp` / `src/agenttools_fs.cpp` /
`src/agentimage.cpp`，见 `src/agenttools.cpp:560 makeAgentTools` 的装配序）。此前没有任何守门比对
这两份名单：加一个工具（或改名/删除）只要忘了同步 docs 表，就只能靠人工评审
发现——而 ICA 侧的姊妹风险已经有 `agent/src/ica/specs.rs` 的 `all.len() == 8`
自钉（见该文件测试注释），后端工具这一半一直是空的。

本脚本零编译、零第三方依赖，接入 `test/run_all_selftest.sh`，让「代码加了工具、
文档没跟上」（或反过来，文档写了代码里不存在的工具）在 push 前就被挡下。

两组断言：
  1. 代码侧抽到的工具名集合 == 文档表第一列的工具名集合（双向，各自列出对侧没有的）；
  2. 抽取本身有效：代码侧与文档侧都至少抽到 1 个、且各自无重复项
     （抽到 0 个或有重复都说明写法变了、本脚本的抽取式脱节，宁可报错也不让
     「两边都空」或「两边都重复同一个」悄悄变成假绿）。

注意：工具是否**实际注册**取决于运行期能力门控（`--agent-shell` 等，见
`src/agenttools.cpp:560-576`）——本脚本比对的是「代码里一共定义了哪些工具」，
不是「某次运行实际暴露了哪些」，这与 docs 表「列出全部工具、门控写在第三列」
的既有体例一致。

用法：python3 scripts/check_agent_tool_table.py
退出码：0=一致，1=发现不一致或解析失败（原因打到 stderr）。
"""

import re
import sys
from collections import Counter

from lib.protocol_sources import (
    AGENT_IMAGE_CPP,
    AGENTTOOLS_CPP,
    AGENTTOOLS_FS_CPP,
    AGENTTOOLS_WAIT_CPP,
    DOCS_AGENT_MD,
    rel,
    resolve,
    strip_comments,
)
from lib.protocol_sources import read as _read

PREFIX = "check_agent_tool_table"

CODE_PATHS = [
    resolve(AGENTTOOLS_CPP),
    resolve(AGENTTOOLS_WAIT_CPP),
    resolve(AGENTTOOLS_FS_CPP),
    resolve(AGENT_IMAGE_CPP),
]
DOCS_PATH = resolve(DOCS_AGENT_MD)

# 工具名的第一个字段：`AgentTool{` 之后第一个字符串字面量就是 name
# （三个 TU 里全部如此，见头注；不用 balanced_block 是因为一个文件里有多个
# `AgentTool{` 块，而我们只要每块的**第一个**字符串，正则 findall 更直接）。
TOOL_NAME_RE = re.compile(r'AgentTool\{\s*"([a-zA-Z_][a-zA-Z0-9_]*)"')

# docs 表第一列：`| \`name\` |`（表头/分隔行不匹配——它们不是反引号包着的标识符）
DOC_ROW_RE = re.compile(r'^\|\s*`([a-zA-Z_][a-zA-Z0-9_]*)`\s*\|', re.M)


def read(path):
    return _read(path, PREFIX)


def code_tool_names(errors):
    names = []
    for path in CODE_PATHS:
        text = strip_comments(read(path))
        found = TOOL_NAME_RE.findall(text)
        if not found:
            errors.append(f"{rel(path)} 里一个 `AgentTool{{\"...\"}}` 都没抽到——"
                "本脚本的抽取式跟写法脱节了，还是该文件确实不再定义工具？")
        names.extend(found)
    for name, n in Counter(names).items():
        if n > 1:
            errors.append(f"代码侧工具名 `{name}` 出现 {n} 次（同名工具重复定义？）")
    return names


def doc_tool_names(errors):
    text = read(DOCS_PATH)
    marker = "## 给智能体的工具"
    start = text.find(marker)
    if start == -1:
        sys.exit(f"{PREFIX}: 在 {rel(DOCS_PATH)} 里没找到 `{marker}` 小节标题")
    end = text.find("\n## ", start + len(marker))
    if end == -1:
        end = len(text)
    section = text[start:end]
    names = DOC_ROW_RE.findall(section)
    if not names:
        errors.append(f"{rel(DOCS_PATH)} 的「{marker}」小节一行 `| \\`name\\` |` 都没抽到"
            "——表格写法变了，还是表空了？")
    for name, n in Counter(names).items():
        if n > 1:
            errors.append(f"文档侧工具名 `{name}` 在工具表里出现 {n} 次")
    return names


def main():
    errors = []
    code_names = code_tool_names(errors)
    doc_names = doc_tool_names(errors)
    code_set, doc_set = set(code_names), set(doc_names)

    for name in sorted(code_set - doc_set):
        errors.append(
            f"工具 `{name}` 在代码里定义了（{', '.join(rel(p) for p in CODE_PATHS)}），"
            f"但 {rel(DOCS_PATH)} 的工具表没有这一行——用户看文档会漏掉这个工具"
        )
    for name in sorted(doc_set - code_set):
        errors.append(
            f"{rel(DOCS_PATH)} 的工具表里有 `{name}` 这一行，但代码里找不到同名 "
            f"`AgentTool{{\"...\"}}`——文档在说一个不存在（或已改名/已删除）的工具"
        )

    if errors:
        print(f"{PREFIX}: 后端工具注册表与 docs/agent.md 工具表不一致：", file=sys.stderr)
        for e in errors:
            print(f"  - {e}", file=sys.stderr)
        sys.exit(1)

    print(f"{PREFIX}: 一致（{len(code_set)} 个工具：代码定义 == docs/agent.md 工具表）")


if __name__ == "__main__":
    main()
