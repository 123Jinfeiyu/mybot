#!/usr/bin/env python3
"""检查文档里的仓内路径引用与计数是否还与仓库实况一致。

文档是唯一一处「写下一个路径/数字，然后没人再回头看」的地方：文件改名、拆分、
删除之后，引用它的文档不会报错，只会静静地指向不存在的东西。同一条计数（模块数、
共享库数）还常在手写多份（README、docs/README.md、docs/drivers.md 的口径行），
于是三处各漂一点、彼此不一致，读者无从判断哪个是真的。

本脚本零编译、零第三方依赖，做五组检查（接入 test/run_all_selftest.sh）：

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
  5. 已删文件的引用：DELETED_FILES 里登记的名字（按基名匹配，故 `docs/x.md` 与
     链接形态 `x.md` 都算）在全仓任何文本文件里再出现即判失败。取材面比检查 1 宽
     （源码注释 / 脚本 / CI / 冻结 html 都算），因为死引用的成因恰恰是「引用它的
     地方不在任何守门的取材面里」；名单里的文件若又存在了同样判失败（名单过期）。

用法：python3 scripts/check_doc_paths.py
退出码：0=一致，1=发现不一致或解析失败（原因打到 stderr）。
"""

import os
import re
import sys
from pathlib import Path

from lib.protocol_sources import ROOT, rel
from lib.protocol_sources import read as _read

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
#   * 历史记录：正文明写「历史上」提到后来被删/改名的文件——把它们改成现名等于
#     篡改历史；
#   * 占位/示例：教程与查找顺序里写的假名（`modules/X.lua`），本来就不该存在。
# 登记在这里的条目若**又存在了**（比如文件被加回来），本脚本也判失败：那时它不再是
# 豁免，而说明这份名单过期了。
# 2026-10-04：docs/changelog.md（冻结变更账本）整篇删除后，本表原「历史记录」类
# 下七条——仅由该账本提及、仓内再无第二处引用的条目（LlmOpenAI.lua/
# check_sim_action_table.py/src/lib.rs/test_sessionfile.cpp/dump_consts.lua/
# test_llm_wire_contract.lua/robot.cpp）——随之失去唯一引用来源，按本脚本自身的
# 「过期即删」判据一并清掉；不等价于重建 changelog。
KNOWN_ABSENT = {
    # —— 历史记录（正文明写「历史上」）——
    "src/luaend.cpp": "历史：development.md 明写「历史上」，N-17 拆成 luasandbox.cpp 等",
    # —— 占位 / 示例 ——
    "./mybot.llm.conf.json": "占位：agent.md 列的是配置文件**查找顺序**候选名，不是仓内文件",
    "modules/X.lua": "占位：development.md「新增驱动」教程里的假名",
    "robots/xxx.sql": "占位：同上，结构脚本示例名",
    "scripts/xxx.mt": "占位：同上，动作脚本示例名",
    "test/test_xxx.lua": "占位：同上，回归用例示例名",
}

# 已删除且**刻意不重建**的文件：任何取材面里再出现指向它的引用即判失败（检查 5）。
# 与 KNOWN_ABSENT 方向相反——那张表登记「文档提到的历史名，允许不存在」，这张表登记
# 「这个名字不许再被当成现存出处引用」。为什么需要它，是一次真实的漏网：9480de3 删掉
# docs/changelog.md 后，d7ca619 只清掉了 docs/*.md 里**路径形态**的死引用。检查 1 的
# 取材面就是 README + docs/*.md，且只认 markdown 链接与反引号跨度，于是三类全都漏网：
# ① 冻结 html 横幅里的 `<a href="changelog.md">`（html 不在取材面）；② src/ scripts/
# test/ .gitlab-ci.yml 注释里的「见 docs/changelog.md」（同上）；③ docs/*.md 正文里的
# 裸词「changelog」（在取材面里，但不是链接也不是路径形态的跨度）。
# 2026-10-06 清点共 11 处，已全部改成提交号或中性说法。检查 1 的取材面**不该**扩到 html：
# 那是 2026-09-25 的冻结快照，逐条实测其中路径只会产出一堆刻意不修的差异。
# 匹配用**基名**：`docs/x.md` 与 markdown 链接里的相对形态 `x.md` 一并覆盖。
# 已知残留（刻意不追）：无扩展名的裸词不匹配。上面那 11 处里另有 4 处原文就是裸词
# 「changelog」（.gitlab-ci.yml、docs/agent.md 两处、docs/development.md），一并改成了
# 提交号或中性说法；仓内还剩 docs/architecture_analysis.html:386 一处裸词——那是
# 2026-09-25 冻结快照的正文，改它等于篡改快照。要连裸词一起禁，代价是冻结快照与日常
# 散文里的普通词都会误报，不值。
DELETED_FILES = {
    "docs/changelog.md": "变更账本自 2026-10-04 起就是提交信息（9480de3 删除）；出处请写提交号，如「见提交 fc1c66f」",
}
# 自排除：本脚本的表与头注必须能写出这个名字（与 KNOWN_ABSENT 的注释同理）。
DELETED_SCAN_SKIP = ("scripts/check_doc_paths.py",)
DELETED_SCAN_SUFFIXES = frozenset((
    ".md", ".html", ".sh", ".py", ".cpp", ".hpp", ".h", ".c", ".rs", ".lua",
    ".yml", ".yaml", ".txt", ".cmake", ".awk", ".mt", ".sql", ".scene",
))
DELETED_SCAN_PRUNE = frozenset((
    ".git", ".zcode", ".trae", ".tmp", "build", "target", "node_modules",
))


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
    """本脚本的读取包装：失败文案带脚本名前缀。"""
    return _read(path, PREFIX)


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


# ── 5) 已删文件的引用（全仓取材面）────────────────────────────────────


def check_deleted(errors):
    """扫全仓，找出仍把 DELETED_FILES 里的名字当现存出处引用的地方。

    取材面比检查 1 宽得多（源码注释、脚本、CI、冻结 html 都算）：死引用的成因正是
    「引用它的地方不在任何守门的取材面里」。反过来，名单里的文件若又存在了，说明名单
    过期——与 KNOWN_ABSENT 的「过期即失败」同一取向。
    """
    for name in DELETED_FILES:
        if (ROOT / name).exists():
            errors.append(f"DELETED_FILES 名单过期：{name} 又存在了（该条目不再是豁免）")
    pats = {n: re.compile(re.escape(Path(n).name)) for n in DELETED_FILES}
    skip = {ROOT / s for s in DELETED_SCAN_SKIP}
    n_files = 0
    for dirpath, dirnames, filenames in os.walk(ROOT):
        dirnames[:] = [
            d for d in dirnames
            if d not in DELETED_SCAN_PRUNE and not d.startswith("build-")
        ]
        for fn in sorted(filenames):
            p = Path(dirpath) / fn
            if p in skip:
                continue
            if p.suffix not in DELETED_SCAN_SUFFIXES:
                continue
            try:
                lines = p.read_text(encoding="utf-8").splitlines()
            except (UnicodeDecodeError, OSError):
                continue
            n_files += 1
            for name, pat in pats.items():
                for i, line in enumerate(lines, 1):
                    if pat.search(line):
                        errors.append(
                            f"{rel(p)}:{i} 仍引用已删除的 {name}"
                            f"——{DELETED_FILES[name]}"
                        )
    if not n_files:
        sys.exit(f"{PREFIX}: 已删文件扫描一个文件都没读到，取材面是不是变了？")
    return n_files


def main():
    errors = []
    n_paths = check_paths(errors)
    roster = drivers_roster()
    on_disk = modules_on_disk()
    n_modules = check_roster(errors, roster, on_disk)
    stated = check_counts(errors, roster, on_disk)
    n_docs = check_index(errors)
    n_scanned = check_deleted(errors)

    if errors:
        print(f"{PREFIX}: 文档与仓库实况不一致：", file=sys.stderr)
        for e in errors:
            print(f"  - {e}", file=sys.stderr)
        sys.exit(1)

    print(
        f"{PREFIX}: 一致（{n_paths} 条路径引用 / {n_modules} 个模块双向对齐"
        f" / 计数口径 {stated['total']}={stated['device']}+{stated['bus']}+{stated['tool']}"
        f"、lib {stated['lib']} / {n_docs} 篇文档全部挂上索引"
        f" / {n_scanned} 个文件里零处引用 {len(DELETED_FILES)} 个已删文件"
        f" / {len(KNOWN_ABSENT)} 条历史与占位豁免仍生效）"
    )


if __name__ == "__main__":
    main()
