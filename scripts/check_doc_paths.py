#!/usr/bin/env python3
"""检查文档里的仓内路径引用与计数是否还与仓库实况一致。

文档是唯一一处「写下一个路径/数字，然后没人再回头看」的地方：文件改名、拆分、
删除之后，引用它的文档不会报错，只会静静地指向不存在的东西。同一条计数（模块数、
共享库数）还常在手写多份（README、docs/README.md、docs/drivers.md 的口径行），
于是三处各漂一点、彼此不一致，读者无从判断哪个是真的。

本脚本零编译、零第三方依赖，做四组检查（接入 test/run_all_selftest.sh）：

  1. 路径存在性：README.md 与 docs/*.md 里的仓内路径引用逐个实测存在。
     * markdown 链接 `[文字](目标)` 的目标按**文档所在目录**解析（docs/x.md 里
       `](sim.md)` 指 docs/sim.md）；
     * 反引号代码跨度按**仓库根**解析（正文里写 `sim/src/main.cpp` 从来都是根相对）；
     * 只认「以已知顶层目录开头 / 带已知扩展名 / 不含通配与占位符」的跨度，
       否则 `set_word/get_sword` 这类函数名清单会被当成路径，全是假差异。
     * 历史与占位引用在 KNOWN_ABSENT 里逐条登记并写明理由（见该表）。
  2. 驱动清单双向对齐：docs/drivers.md 各节表格里点名的模块集合，与
     `ls modules/*.lua` **双向**相等——漏登记一个新驱动、或留着一个已删驱动都判失败。
  3. 计数口径自洽：drivers.md 开头「计数口径」那行的分项（器件 / 总线 / 框架工具、
     共享库数）与本页各节的实际条目数、与磁盘上的实际文件数三者一致。
  4. 文档索引覆盖：每个 docs/*.md 都被 README.md 或 docs/README.md 链到——
     新增一篇文档而忘了挂索引，等于它对读者不存在。

用法：python3 scripts/check_doc_paths.py
退出码：0=一致，1=发现不一致或解析失败（原因打到 stderr）。
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PREFIX = "check_doc_paths"

DOCS_DIR = ROOT / "docs"
MODULES_DIR = ROOT / "modules"
README = ROOT / "README.md"
DRIVERS_MD = DOCS_DIR / "drivers.md"

# 检查 1 的取材面：仓库根的 README + docs 下全部 md
DOC_GLOB = "docs/*.md"

# 只把这些开头/结尾的跨度当路径候选（见文件头「否则全是假差异」）
TOP_PREFIXES = (
    "src/", "sim/", "agent/", "modules/", "test/", "scripts/",
    "robots/", "docs/", "prompts/", "build/", "./",
)
KNOWN_EXTENSIONS = (
    ".md", ".lua", ".sql", ".mt", ".cpp", ".hpp", ".h", ".c", ".rs", ".py",
    ".sh", ".json", ".svg", ".html", ".txt", ".cmake", ".yml", ".yaml",
    ".scene", ".example", ".toml", ".supp",
)
# 通配符、占位符、shell 展开、空白：出现即不是「一个具体路径」
PLACEHOLDER_RE = re.compile(r"[*<>{}$\s]|\.\.\.")

LINK_RE = re.compile(r"\[[^\]]*\]\(([^)\s]+)\)")
CODE_RE = re.compile(r"`([^`\n]+)`")
URL_PREFIXES = ("http://", "https://", "mailto:", "#")

# 已知不存在、且**故意**不存在的引用。键是引用串，值是理由。
# 分两类：
#   * 历史记录：docs/changelog.md 是冻结的变更账本，它按定义要提到后来被删/改名的
#     文件——把它们改成现名等于篡改历史；
#   * 占位/示例：教程与查找顺序里写的假名（`modules/X.lua`），本来就不该存在。
# 登记在这里的条目若**又存在了**（比如文件被加回来），本脚本也判失败：那时它不再是
# 豁免，而说明这份名单过期了。
KNOWN_ABSENT = {
    # —— 历史记录（changelog 冻结账本 + 明写「历史上」的正文）——
    "modules/LlmOpenAI.lua": "历史：Lua 协议驱动轨道 2026-09-25 #8 删除，changelog 记录当时事实",
    "scripts/check_sim_action_table.py": "历史：已更名 scripts/check_sim_contract.py，changelog 记录更名本身",
    "src/lib.rs": "历史：反例引用（agent 是 bin-only crate，没有 src/lib.rs）",
    "test/cpp/test_sessionfile.cpp": "历史：会话持久化用例已并入他处，changelog 记录当时文件名",
    "test/dump_consts.lua": "历史：常量抽取改由 test/gen_const_check.cmake 在配置期做，该脚本已删",
    "test/test_llm_wire_contract.lua": "历史：随 Lua 协议驱动轨道一并删除",
    "src/luaend.cpp": "历史：development.md 明写「历史上」，N-17 拆成 luasandbox.cpp 等",
    "src/robot.cpp": "历史：RobotStruct 门面 TU 已按职责拆成 robot_lifecycle/actions/driver/hotsql 四个 TU 并删除，changelog 记录当时的行数",
    # —— 占位 / 示例 ——
    "./mybot.llm.conf.json": "占位：agent.md 列的是配置文件**查找顺序**候选名，不是仓内文件",
    "modules/X.lua": "占位：development.md「新增驱动」教程里的假名",
    "robots/xxx.sql": "占位：同上，结构脚本示例名",
    "scripts/xxx.mt": "占位：同上，动作脚本示例名",
    "test/test_xxx.lua": "占位：同上，回归用例示例名",
}

# drivers.md 里不参与「模块清单」的两节：前者讲入口函数契约、后者讲刻意保留的重复，
# 表格首列不是模块名。
NON_ROSTER_SECTIONS = ("驱动契约", "收敛豁免清单（刻意保留的重复实现）")

# 「计数口径」那行的抽取式。它是一整行长文本，各分项都用 **粗体** 标出，故按粗体锚定。
COUNT_LINE_RE = re.compile(
    r"`ls modules/\*\.lua` = (?P<total>\d+) = "
    r"\*\*(?P<device>\d+) 款器件/执行器/系统接口驱动\*\*.*?"
    r"\+ \*\*(?P<bus>\d+) 条总线\*\*.*?"
    r"\+ \*\*(?P<tool>\d+) 个框架/工具驱动\*\*.*?"
    r"另有 (?P<lib>\d+) 个共享库",
    re.S,
)
SECTION_RE = re.compile(r"^## (.+)$", re.M)
ROW_FIRST_CELL_RE = re.compile(r"^\|\s*(.+?)\s*\|", re.M)
NAME_RE = re.compile(r"`([A-Za-z_][A-Za-z0-9_]*)`")


def read(path):
    if not path.is_file():
        sys.exit(f"{PREFIX}: 找不到 {path}")
    return path.read_text(encoding="utf-8")


def rel(path):
    return path.relative_to(ROOT)


def doc_files():
    files = [README] + sorted(DOCS_DIR.glob("*.md"))
    if len(files) < 2:
        sys.exit(f"{PREFIX}: {DOC_GLOB} 一个都没找到，取材面是不是变了？")
    return files


# ── 1) 路径存在性 ──────────────────────────────────────────────────────


def path_candidates(doc):
    """返回 [(kind, ref)]：kind ∈ {link, code}，决定按哪个基准目录解析。"""
    text = read(doc)
    out = set()
    for m in LINK_RE.finditer(text):
        target = m.group(1)
        if not target.startswith(URL_PREFIXES):
            out.add(("link", target))
    for m in CODE_RE.finditer(text):
        ref = m.group(1)
        if not ref.startswith(TOP_PREFIXES):
            continue
        if PLACEHOLDER_RE.search(ref):
            continue
        if not ref.endswith(KNOWN_EXTENSIONS):
            continue
        out.add(("code", ref))
    return sorted(out)


def resolve(doc, kind, ref):
    ref = ref.split("#", 1)[0]
    if not ref:
        return None
    if ref.startswith("/"):
        return ROOT / ref.lstrip("/")
    base = doc.parent if kind == "link" else ROOT
    return base / ref


def check_paths(errors):
    n = 0
    seen_absent = set()
    for doc in doc_files():
        for kind, ref in path_candidates(doc):
            target = resolve(doc, kind, ref)
            if target is None:
                continue
            n += 1
            if target.exists():
                continue
            # 只按仓库根相对形态登记豁免（链接形态在 docs/ 下，同一文件两种写法都认）
            try:
                key = str(target.relative_to(ROOT))
            except ValueError:
                key = ref
            if key in KNOWN_ABSENT or ref in KNOWN_ABSENT:
                seen_absent.add(key if key in KNOWN_ABSENT else ref)
                continue
            errors.append(
                f"{rel(doc)} 引用的路径不存在：{ref}"
                f"（{'链接' if kind == 'link' else '代码跨度'}，解析为 {key}）"
            )
    stale = sorted(set(KNOWN_ABSENT) - seen_absent)
    for name in stale:
        errors.append(
            f"豁免名单里的 '{name}' 这次没被任何文档引用（或已存在）——"
            f"该条目过期了，请从 {rel(Path(__file__))} 的 KNOWN_ABSENT 删掉"
        )
    return n


# ── 2) drivers.md 的模块清单 ↔ modules/*.lua ──────────────────────────


def drivers_roster():
    """按节抽取 drivers.md 表格首列点名的模块名：{节标题: {名字, ...}}。"""
    text = read(DRIVERS_MD)
    roster = {}
    section = None
    for line in text.splitlines():
        m = SECTION_RE.match(line)
        if m:
            section = m.group(1).strip()
            if section not in NON_ROSTER_SECTIONS:
                roster.setdefault(section, set())
            continue
        if section is None or section in NON_ROSTER_SECTIONS:
            continue
        row = ROW_FIRST_CELL_RE.match(line)
        if not row:
            continue
        cell = row.group(1)
        if cell.startswith("---") or cell in ("驱动", "模块"):
            continue
        for name in NAME_RE.findall(cell):
            roster[section].add(name)
    if not roster:
        sys.exit(f"{PREFIX}: {rel(DRIVERS_MD)} 一个模块名都没抽到，表格格式变了？")
    return roster


def modules_on_disk():
    names = {p.stem for p in MODULES_DIR.glob("*.lua")}
    if not names:
        sys.exit(f"{PREFIX}: {rel(MODULES_DIR)}/*.lua 一个都没有，取材面是不是变了？")
    return names


def check_roster(errors, roster, on_disk):
    documented = set().union(*roster.values()) if roster else set()
    for name in sorted(documented - on_disk):
        errors.append(
            f"{rel(DRIVERS_MD)} 点名了 modules/{name}.lua，但磁盘上没有这个文件"
            "（改名或删除后漏改文档）"
        )
    for name in sorted(on_disk - documented):
        errors.append(
            f"modules/{name}.lua 存在，但 {rel(DRIVERS_MD)} 没有登记它"
            "（新增驱动漏进清单：读者按文档找不到它）"
        )
    return len(on_disk)


# ── 3) 计数口径自洽 ───────────────────────────────────────────────────


def check_counts(errors, roster, on_disk):
    text = read(DRIVERS_MD)
    m = COUNT_LINE_RE.search(text)
    if m is None:
        sys.exit(
            f"{PREFIX}: {rel(DRIVERS_MD)} 开头找不到「计数口径」那行，或它的措辞变了"
            "（本脚本按 **粗体分项** 锚定；改措辞请同步 COUNT_LINE_RE）"
        )
    stated = {k: int(v) for k, v in m.groupdict().items()}

    if stated["total"] != len(on_disk):
        errors.append(
            f"drivers.md 计数口径写 `ls modules/*.lua` = {stated['total']}，"
            f"实际 {len(on_disk)} 个"
        )

    sections = list(roster)
    device_sections = [s for s in sections if s != "框架 / 总线 / 工具"]
    tool_section = "框架 / 总线 / 工具"
    if tool_section not in roster:
        sys.exit(f"{PREFIX}: {rel(DRIVERS_MD)} 找不到「{tool_section}」一节，节名变了？")

    device_total = sum(len(roster[s]) for s in device_sections)
    if device_total != stated["device"]:
        errors.append(
            f"drivers.md 计数口径写 {stated['device']} 款器件/执行器/系统接口驱动，"
            f"前 {len(device_sections)} 节实际点名 {device_total} 个"
        )
    framework_total = len(roster[tool_section])
    if framework_total != stated["bus"] + stated["tool"]:
        errors.append(
            f"drivers.md 计数口径写 {stated['bus']} 条总线 + {stated['tool']} 个框架/工具驱动"
            f"（合计 {stated['bus'] + stated['tool']}），「{tool_section}」一节实际点名 "
            f"{framework_total} 个"
        )
    if stated["device"] + stated["bus"] + stated["tool"] != stated["total"]:
        errors.append(
            f"drivers.md 计数口径的分项加起来 {stated['device']}+{stated['bus']}"
            f"+{stated['tool']} = {stated['device'] + stated['bus'] + stated['tool']}，"
            f"与它自己写的总数 {stated['total']} 不符"
        )

    lib_on_disk = len(list((MODULES_DIR / "lib").rglob("*.lua")))
    if stated["lib"] != lib_on_disk:
        errors.append(
            f"drivers.md 计数口径写 modules/lib/ 有 {stated['lib']} 个共享库，"
            f"实际 {lib_on_disk} 个（find modules/lib -name '*.lua'）"
        )
    return stated


# ── 4) 文档索引覆盖 ───────────────────────────────────────────────────


def check_index(errors):
    indexed = set()
    for doc in (README, DOCS_DIR / "README.md"):
        for m in LINK_RE.finditer(read(doc)):
            target = resolve(doc, "link", m.group(1))
            if target is None or target.suffix != ".md":
                continue  # 网页与两张 SVG 的存在性由检查 1 管，索引只对齐 md 全集
            if target.parent == DOCS_DIR:
                indexed.add(target.name)
    on_disk = {p.name for p in DOCS_DIR.glob("*.md")}
    if not on_disk:
        sys.exit(f"{PREFIX}: {rel(DOCS_DIR)} 下没有 md 文件，取材面是不是变了？")
    for name in sorted(on_disk - indexed):
        errors.append(
            f"docs/{name} 没有被 README.md 或 docs/README.md 链到（读者从索引进不去）"
        )
    for name in sorted(indexed - on_disk):
        errors.append(f"索引链到 docs/{name}，但该文件不存在")
    return len(on_disk)


def main():
    errors = []
    n_paths = check_paths(errors)
    roster = drivers_roster()
    on_disk = modules_on_disk()
    n_modules = check_roster(errors, roster, on_disk)
    stated = check_counts(errors, roster, on_disk)
    n_docs = check_index(errors)

    if errors:
        print(f"{PREFIX}: 文档与仓库实况不一致：", file=sys.stderr)
        for e in errors:
            print(f"  - {e}", file=sys.stderr)
        sys.exit(1)

    print(
        f"{PREFIX}: 一致（{n_paths} 条路径引用 / {n_modules} 个模块双向对齐"
        f" / 计数口径 {stated['total']}={stated['device']}+{stated['bus']}+{stated['tool']}"
        f"、lib {stated['lib']} / {n_docs} 篇文档全部挂上索引"
        f" / {len(KNOWN_ABSENT)} 条历史与占位豁免仍生效）"
    )


if __name__ == "__main__":
    main()
