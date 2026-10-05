#!/bin/bash
# test/run_all_selftest.sh —— 守门自身的自检（计划 M0 的 C7 起步，零行为变化）
#
# 守门脚本是「声称的防线」与「实际执行」之间唯一的仲裁者，但它自己没有任何测试：
#   - 用例不叫 test_*.lua，run_all.sh 的 glob 与 CMake 的 glob 都不会收集它；
#   - 新加的 test/cpp/*.cpp 不写进 CMakeLists.txt 就永不编译；
#   - 脚本自身的语法错误要到推送那一刻才暴露。
# 本自检只做**便宜且能机械判定**的事，且**不进入 run_all.sh 的记账**
# （守门汇总口径保持不变），由 pre-push 在守门之前调用、CI 的 gate:release 也把它
# 排在全量守门之前。两类，按段号分：
#   1–7  守门自身的可信度（脚本语法、用例是否真被 glob/编译收集、计数文案是否有出处、
#        用例表结构与两套计数互证（条数派生，tokenizer 在 test/lib/case_table.awk）、
#        TSan 名单与 test/lib 的两个单点）；
#   8–13 跨源一致性（同一条事实在两套构建系统里手写多份：仿真器契约、状态目录缺省名、
#        文档引用与计数、模型配置字段白名单、有状态 LLM 协议判据、LLM wire 可钉部分）
#        ——全部零编译，理由见第 8 段。
set -u
cd "$(dirname "$0")/.." || exit 1

fail=0
note() { echo "[FAIL] selftest: $*" >&2; fail=$((fail + 1)); }

# 1) 守门相关脚本的语法（bash -n 不执行，纯解析）
#    test/lib/ 下用 glob 而非逐个列名：本自检存在的理由就是「新文件忘了登记 ⇒ 静默
#    不受检」，列名清单自己也会犯这个错（2026-10-06 加 agent_stale.sh 时改成 glob）。
for f in test/run_all.sh test/run_san.sh test/lib/*.sh \
		test/lib/sections/*.sh scripts/git-hooks/pre-push; do
	if [ ! -f "$f" ]; then
		note "缺文件：$f"
		continue
	fi
	bash -n "$f" || note "$f 存在语法错误"
done

# 2) 每个 test/cpp/*.cpp 都必须登记进 CMakeLists.txt（显式源清单，漏登记即永不编译）
for f in test/cpp/*.cpp; do
	[ -e "$f" ] || continue
	base="$(basename "$f")"
	grep -qF "test/cpp/$base" CMakeLists.txt \
		|| note "C++ 用例未登记进 CMakeLists.txt（不会参与编译）：$f"
done

# 3) test/*.lua 必须匹配 test_*.lua——run_all.sh 与 CMakeLists 两侧的 glob 同此前缀，
#    改名即被两边同时静默漏掉。
for f in test/*.lua; do
	[ -e "$f" ] || continue
	base="$(basename "$f")"
	case "$base" in
		test_*) ;;
		*) note "Lua 用例不匹配 test_*.lua 命名（守门与 ctest 都不会收集）：$f" ;;
	esac
done

# 4) count_calls（「N 条用例」文案的唯一来源）必须真的数得到调用点。
#    它按「行首缩进 + runner 名 + 引号」匹配：引号种类、缩进或改名一变，计数静默
#    变 0，文案与真跑条数就一起漂移。这里用**独立实现**交叉核对（宽松计数不看引号）：
#    两者不等即说明 count_calls 的判据漏了某种写法。F3-4（2026-09-21）。
for r in $(grep -oE 'count_calls [a-zA-Z0-9_]+' test/run_all.sh | awk '{print $2}' | sort -u); do
	# 定义可能在 run_all.sh，也可能在 F3-1 抽出的分段文件里；调用点则必须仍在
	# run_all.sh（count_calls 数的就是它）。
	grep -qE "^[[:space:]]*${r}\(\)" test/run_all.sh test/lib/sections/*.sh \
		|| note "count_calls 的参数不是已定义的 runner（run_all.sh 与分段文件里都没有）：$r"
	strict="$(grep -cE "^[[:space:]]*${r} ['\"]" test/run_all.sh)"
	loose="$(grep -cE "^[[:space:]]*${r}[[:space:]]" test/run_all.sh)"
	if [ "$strict" -eq 0 ]; then
		note "runner $r 的调用点计数为 0（写法变了？引号/缩进/改名）"
	elif [ "$strict" -ne "$loose" ]; then
		note "runner $r 计数不一致：count_calls 判据 $strict != 宽松计数 $loose（有写法没被认出来）"
	fi
done

# 5) 两张用例表的结构与条数（F3-2；2026-10-06 重写：条数改**派生**、判据改 quote-aware）
#    旧写法有两个毛病，第二个是真 bug：
#      * 条数是硬编码检查点（e2e 37 / parse 96），全仓唯一一份手维护的重复数字——文档
#        刻意不写这两个总数（口径见 docs/virtbus.md「用例条数以单一事实源为准」），于是
#        加/删用例就得人记着改它，已经漂过一次（提交 9ebcf4a「同步 parse_cases 条数
#        检查点至 96」）；
#      * 抽取用 grep "^[[:space:]]*'"，**只认单引号**。而表里 expect 字段含单引号的条目
#        按约定整条改用双引号书写（parse_cases 的 vars_bad 行；同一约定在 fixture 孤儿
#        检测那边 2026-09-20 就踩过并修好，注释见 run_all.sh「fixture 孤儿检测」段，
#        这边一直没跟上）。实测：bash 展开 parse_cases 得 97 条，旧抽取只看见 96 条，
#        而检查点写的正是 96——那条用例的字段数 / 引号收尾 / (sql,mt) 重复三项检查全部
#        落空，且检查点反过来把盲区钉成「正确值」：修好抽取器会让自检转红。
#    现改两套独立实现互证（本文件第 4 段 count_calls 的同一手法）：
#      A 行计数：quote-agnostic 地数「以引号开头的行」；
#      B 元素计数：test/lib/case_table.awk 按 bash 引号语义逐字符 tokenize，数出真实
#        元素并吐出去掉外层引号的正文（引号未收尾、双引号内含展开语法在它那里判失败）。
#      A != B ⇒ 有写法逃过了行计数（一行两条 / 非常规缩进 / 新引号形态）。
#    结构判据全部吃 B 的输出，不再对原始文本猜。parse 的段数下界是 **4 不是 3**（旧注释
#    写的「至少三段，其余字段可选」正是这个坑）：消费循环 run_parse_cases
#    （test/lib/sections/lua_e2e.sh）连做四次 ${rest#*|}，三段行走到第四次时 rest 里
#    没有 '|'，bash 原样返回，于是 expect 静默变成期望退出码本身（"0"/"1"），
#    grep -qF 几乎必然命中——用例照跑、照记通过，却一条输出断言都没有。现表里没有
#    三段行，这条是把坑封死。
tok_file=test/lib/case_table.awk
[ -f "$tok_file" ] || note "缺用例表 tokenizer：$tok_file（第 5 段的 B 侧实现）"
q="['\"]"
for t in e2e_cases parse_cases; do
	block="$(sed -n "/^${t}=(/,/^)/p" test/run_all.sh)"
	if [ -z "$block" ]; then
		note "test/run_all.sh 里抽不到用例表 ${t}=( …… )（表名改了？sed 的块抽法失效？）"
		continue
	fi
	n_a="$(printf '%s\n' "$block" | grep -cE "^[[:space:]]*${q}")"
	if ! elems="$(printf '%s\n' "$block" | awk -v SQ="'" -v DQ='"' -f "$tok_file")"; then
		note "${t} 的条目引号有问题（详见上方 case_table.awk 的输出）"
	fi
	n_b="$(printf '%s\n' "$elems" | grep -c .)"
	[ "$n_b" -gt 0 ] || note "${t} 一条都没 tokenize 出来（表被删空？）"
	[ "$n_a" -eq "$n_b" ] || note "${t} 两套计数不一致：行计数 $n_a != tokenize 元素数 $n_b（有写法逃过了行计数：一行两条？缩进或引号形态变了？）"
	# 结构判据：按消费循环（run_one_e2e / run_one_parse）的实际字段语义逐条核。
	# awk 吐 "判据\t命中条数\t首条正文"，用 here-string 喂 while（管道会开子 shell，
	# note 的计数就丢了）。
	while IFS="$(printf '\t')" read -r msg cnt sample; do
		[ -n "$msg" ] || continue
		note "${t} 有 ${cnt} 条${msg}；首条：${sample}"
	done <<< "$(printf '%s\n' "$elems" | awk -F'|' -v T="$t" '
		function bad(m) { n[m]++; if (!(m in first)) first[m] = $0 }
		NF && T == "e2e_cases" {
			if (NF != 3) bad("段数 != 3（应为 name|浮点模式|最小条数）")
			else if ($3 !~ /^[0-9]+$/) bad("第 3 段（最小条数）不是非负整数")
			if (seen[$1]++) bad("名字重复")
		}
		NF && T == "parse_cases" {
			if (NF < 4 || NF > 6) bad("段数不在 4~6（sql|mt|期望退出码|expect[|forbid[|via]]；三段行会让 expect 静默退化成退出码）")
			else if ($3 !~ /^[0-9]+$/) bad("第 3 段（期望退出码）不是非负整数")
			if (NF >= 6 && $6 != "I") bad("第 6 段（via）不是 I（--input 文件通道）也不是空")
			k = $1 "|" $2
			if (seen[k]++) bad("(sql,mt) 组合重复")
		}
		END { for (m in n) printf "%s\t%d\t%s\n", m, n[m], first[m] }
	')"
done

# 6) TSan 用例名单单点（F3-5）：名单唯一来源 test/lib/tsan_cases.txt，CI 逐条跑。
#    这里机械核对每个名字在 test_daemon_concurrency.cpp 里至少命中一条 TH_TEST——
#    改过用例名而没改名单（或反之）会静默少跑并发回归。
tsan_file=test/lib/tsan_cases.txt
if [ ! -s "$tsan_file" ]; then
	note "缺 TSan 名单或为空：$tsan_file"
else
	while read -r c; do
		case "$c" in ''|\#*) continue ;; esac
		grep -qE "^TH_TEST\(${c}[A-Za-z0-9_]*\)" test/cpp/test_daemon_concurrency.cpp \
			|| note "TSan 名单里的 '$c' 在 test_daemon_concurrency.cpp 里没有对应用例（改过名？）"
	done < "$tsan_file"
fi

# 7) 守门单点（F3-5 余部）：常量与判据取材面各自只有一份。
#    TEST_TIMEOUT/SAN_DIR 默认值唯一来源
#    test/lib/params.sh（bash 直接 source、CMake 正则提取、CI 同源）。格式
#    漂移（如 CMake 侧正则依赖的 `: "${KEY:=v}"` 形态被改）会让 ctest TIMEOUT
#    在 configure 期 FATAL——这里再钉一层，让漂移在 selftest 就显形。
for k in TEST_TIMEOUT SAN_DIR; do
	grep -qE "^: \"\\\$\{${k}:=[0-9a-zA-Z._/-]+\}\"$" test/lib/params.sh \
		|| note "params 单点缺 ${k} 条目或格式漂移（CMake 正则提取会失败）：test/lib/params.sh"
done
#    同族第二个单点：test/lib/agent_stale.sh（mybot-agent 产物新鲜度的取材面），
#    消费方三处——net_agent.sh 的 agent-rust-e2e 段与 ica-e2e 段、git-hooks/pre-push。
#    钉四件事：①函数还定义着（改名 ⇒ 三处调用一起变哑，守门只会静默判「新鲜」）；
#    ②③两个消费方各自还在调用、且调用点数目对（ica 段那道守卫是 agent 段的同胞副本，
#    最容易被当成重复代码删掉一条）；④没人绕开单点自己手抄 `find agent/src`
#    （2026-10-06 之前正是两份手抄，取材面各漂各的，第三处又要再抄一遍）。
#    两个文件自排除：agent_stale.sh 是单点本身；本自检的 grep 模式里字面引用了
#    `find agent/src`，不排除就会判自己手抄（2026-10-06 首跑即中招，与
#    scripts/check_doc_paths.py 排除自己同一处置）。
grep -qE '^agent_stale_file\(\)' test/lib/agent_stale.sh \
	|| note "agent 新鲜度判据单点缺 agent_stale_file() 定义：test/lib/agent_stale.sh"
n_agent="$(grep -c 'agent_stale_file "' test/lib/sections/net_agent.sh)"
n_push="$(grep -c 'agent_stale_file "' scripts/git-hooks/pre-push)"
[ "$n_agent" -eq 2 ] || note "net_agent.sh 的 agent_stale_file 调用点 $n_agent != 2（agent-rust-e2e 与 ica-e2e 各一道陈旧守卫）"
[ "$n_push" -eq 1 ] || note "pre-push 的 agent_stale_file 调用点 $n_push != 1（新鲜度硬门被摘了？）"
for f in $(grep -rlE 'find agent/src' --include='*.sh' test scripts 2>/dev/null |
		grep -vE '^(test/lib/agent_stale\.sh|test/run_all_selftest\.sh)$'); do
	note "$f 自己手抄了 agent 取材面（应调用 test/lib/agent_stale.sh 的 agent_stale_file）"
done

# 8) 仿真器跨构建系统契约（sim/ vs 宿主 C++/Lua 的手写双份）：动作码表、三条
#    缺省 socket 路径、一问一答桥的请求关键字、默认场景（world.cpp 的内置回落 vs
#    sim/scenes/default.scene 模板）。sim/ 不在任何 CI job 的构建范围内
#    （见 sim/CMakeLists.txt 顶部注释），sim/tests 里的同款校验（testActionTable /
#    testSharedConstants）因此只在开发者手动跑 `bash scripts/build_sim.sh` 时才会
#    触发——这里用零编译依赖的独立实现把它下沉进主干守门，让漏改一侧在 push 前
#    就被挡下。
if command -v python3 >/dev/null 2>&1; then
	python3 scripts/check_sim_contract.py || note "scripts/check_sim_contract.py 发现跨构建契约不一致（详见上方输出）"
else
	note "缺 python3，无法跑 scripts/check_sim_contract.py（仿真器跨构建契约未受检查）"
fi

# 9) 状态目录取值链的三处实现（规则单点：docs/reference.md 的 MYBOT_HOME 行）。
#    同一条「MYBOT_HOME 非空 > $HOME/.mybot > 不可用」在 Rust agent、sim 的 C++、
#    demo 脚本里各手写一份（跨两套构建系统，刻意不做代码生成）。缺省目录名漂移
#    两侧都不会报错——agent 的会话历史与 sim 的地图各落一棵树，互不可见。
#    每处钉两件事：读 MYBOT_HOME 的形态还在（没被换成别的变量），且拼出的缺省
#    后缀仍是 /.mybot。
#    ⚠ 第三行（scripts/sim_agent_demo.sh）是**静态文本**比对，不是执行：本自检从头到尾
#    没跑过那个脚本，别的守门也没跑过——它要显示器、真模型凭据 mybot.llm.json、
#    以及先 bash scripts/build_sim.sh，三条都进不了自动守门，只能手动冒烟（详见
#    docs/sim.md「端到端链路」）。别把这一行读成「三层链路受守门覆盖」。
while read -r f pat; do
	[ -n "$f" ] || continue
	grep -qE "$pat" "$f" \
		|| note "$f 找不到 '$pat'——状态目录不再按 MYBOT_HOME > \$HOME/.mybot 取？"
	got="$(grep -oE '/\.mybot[A-Za-z0-9_.-]*' "$f" | sort -u | tr '\n' ' ')"
	if [ "$got" != "/.mybot " ]; then
		note "$f 的状态目录缺省后缀不是 /.mybot（实际：${got:-抽不到}）——三处须同拼法"
	fi
done <<'EOF'
agent/src/config/mod.rs env_var\("MYBOT_HOME"\)
sim/src/sim_paths.cpp getenv\("MYBOT_HOME"\)
scripts/sim_agent_demo.sh \$\{MYBOT_HOME:-
EOF

# 10) 文档与仓库实况的一致性（零编译）：路径引用逐个实测存在、drivers.md 的模块清单
#     与 modules/*.lua **双向**相等、drivers.md 开头「计数口径」那行的分项与各节实际
#     条目数和磁盘文件数三者自洽、每篇 docs/*.md 都挂在索引上、DELETED_FILES 里登记的
#     已删文件名在全仓任何文本文件里不再出现。
#     文档不会编译、不会失败，改名/删除/新增之后只会静静地指向不存在的东西；同一条
#     计数还手写三份（README、docs/README.md、drivers.md），各漂一点就没人知道哪个真。
#     历史与占位引用（正文明写「历史上」的旧文件名、教程里的 modules/X.lua）在脚本的
#     KNOWN_ABSENT 里逐条登记并写明理由；登记过的条目若又存在了，脚本同样判失败
#     （名单过期也是一份不一致）。
#     第五组的取材面刻意比第一组宽（源码注释 / 脚本 / CI / 冻结 html 都扫），判据是
#     **带扩展名的字面量**（故 `docs/` 前缀形态与 markdown 链接里的相对形态一并覆盖）。
#     2026-10-06 实测：那本冻结变更账本被 9480de3 删除后仓内还留着 11 处引用，第一组
#     一处都看不见——8 处不在它的取材面（src/ scripts/ test/ CI / 冻结 html），3 处在
#     docs/*.md 里但是裸词而非链接或路径跨度。死引用的成因恰恰是「引用它的地方不在任何
#     守门的取材面里」。
#     已知残留：无扩展名的裸词不匹配，冻结快照 docs/architecture_analysis.html:386 就是
#     一个裸词——那是 2026-09-25 快照的正文，改它等于篡改快照，刻意不动。
#     ⚠ 本段注释**故意不写出那个文件名**：写了就会被第五组自己扫到（.sh 在取材面里），
#     于是得把本文件也加进脚本的 DELETED_SCAN_SKIP——自排除名单每多一条，守门就少扫
#     一个文件。名字与完整清点记在 scripts/check_doc_paths.py 的 DELETED_FILES 表上。
# 11) 模型配置字段白名单的双端同集合：C++ src/llmconfig.cpp 的 kKnown vs Rust
#     agent/src/config/file.rs 的 PARSED_FIELD_NAMES（parsed_fields! 宏表）+
#     KNOWN_UNPARSED_FIELDS。两端读同一份 mybot.llm.json，任一侧漏登记一个字段，
#     那一侧就把操作者的合法配置判成「未知字段」拒读——两侧单测各验自己那份，
#     谁也发现不了对侧漏改，故在此零编译比对。
# 12) 有状态 LLM 协议判据的双端同集合：Rust `agent/src/session.rs` 的
#     `protocol_violation`（`--resume` 恢复会话前的最后一道判据）vs Python
#     `scripts/fake_llm_server.py` 的 `protocol_error`（假模型服务与真网关一样严）。
#     两份各有一套单测，却没有一道守门比对**判据集本身**：夹具少一条判据时，产线会
#     拒的历史在离线守门里照常全绿——正是夹具文档点名的「溜到真机才暴露」。脚本另钉
#     产线判据条数与判据锚点（产线加/删判据必须同步用例表），并逐条断言拒因归属。
#     C++ 侧不参与（不解析模型历史，如实差异见脚本头）。
# 13) LLM wire 契约里本仓自己手写的那部分：`agent/src/provider.rs` 的协议族表名
#     （`from_table`/`as_str` 同集合）与 `request_path`，必须能解释 Python 夹具
#     `scripts/fake_llm_server.py` 的 path 断言，且两个 E2E 脚本里出现的每个
#     `--llm-driver` 都是已知表名、每个 `--llm-endpoint` 路径都以某条 request_path
#     结尾（成对的族名/路径不许错配）。报文字段、SSE 帧名、鉴权头名归 vendored rig
#     crate，本仓无字面量可钉——如实差异逐条登记在脚本头；C++/Lua 自 2026-09-25 起
#     不参与网络（无 modules/Llm*.lua），故不涉及。
if command -v python3 >/dev/null 2>&1; then
	python3 scripts/check_doc_paths.py \
		|| note "scripts/check_doc_paths.py 发现文档与仓库实况不一致（详见上方输出）"
	python3 scripts/check_llm_field_whitelist.py \
		|| note "scripts/check_llm_field_whitelist.py 发现配置字段白名单双端不一致（详见上方输出）"
	python3 scripts/check_stateful_protocol.py \
		|| note "scripts/check_stateful_protocol.py 发现有状态协议判据双端不一致（详见上方输出）"
	python3 scripts/check_llm_wire.py \
		|| note "scripts/check_llm_wire.py 发现 LLM wire 契约不一致（详见上方输出）"
else
	note "缺 python3，无法跑 scripts/check_doc_paths.py、scripts/check_llm_field_whitelist.py、scripts/check_stateful_protocol.py 与 scripts/check_llm_wire.py（文档一致性/配置白名单/有状态协议判据/LLM wire 契约未受检查）"
fi

if [ "$fail" -ne 0 ]; then
	echo "run_all_selftest: $fail 项不合格" >&2
	exit 1
fi
echo "run_all_selftest: 通过（语法 / C++ 用例登记 / Lua 命名 / count_calls / 用例表结构 / TSan 名单 / test/lib 单点 / 仿真器跨构建契约 / 状态目录缺省名 / 文档与仓库实况 / 配置字段白名单双端 / 有状态协议判据双端 / LLM wire 契约）"
