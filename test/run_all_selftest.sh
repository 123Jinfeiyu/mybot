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
#        用例表条数检查点、TSan 名单与 params 常量单点）；
#   8–12 跨源一致性（同一条事实在两套构建系统里手写多份：仿真器契约、状态目录缺省名、
#        文档引用与计数、模型配置字段白名单、有状态 LLM 协议判据）——全部零编译，
#        理由见第 8 段。
set -u
cd "$(dirname "$0")/.." || exit 1

fail=0
note() { echo "[FAIL] selftest: $*" >&2; fail=$((fail + 1)); }

# 1) 守门相关脚本的语法（bash -n 不执行，纯解析）
for f in test/run_all.sh test/run_san.sh test/lib/device_gate.sh \
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

# 5) 两张用例表的结构与条数（F3-2）：条数是**显式检查点**——加/删用例必须同时
#    改这里（以及文档），否则「守门到底跑多少条」只能靠人记；字段数错（少一个
#    分隔符）会让消费循环把模式当成条数用，静默错判而不报错。
e2e_rows="$(sed -n '/^e2e_cases=(/,/^)/p' test/run_all.sh | grep -E "^[[:space:]]*'")"
parse_rows="$(sed -n '/^parse_cases=(/,/^)/p' test/run_all.sh | grep -E "^[[:space:]]*'")"
e2e_n="$(printf '%s\n' "$e2e_rows" | grep -c .)"
parse_n="$(printf '%s\n' "$parse_rows" | grep -c .)"
e2e_bad="$(printf '%s\n' "$e2e_rows" | grep -vcE "^[[:space:]]*'[^'|]*\|[^|']*\|[^|']*'$")"
# parse 行至少三段（sql|mt|期望退出码），其余字段可选，故不锚行尾
parse_bad="$(printf '%s\n' "$parse_rows" | grep -vcE "^[[:space:]]*'[^'|]*\|[^'|]*\|[^'|]*")"
e2e_unterm="$(printf '%s\n' "$e2e_rows" | grep -vcE "'$")"
parse_unterm="$(printf '%s\n' "$parse_rows" | grep -vcE "'$")"
[ "$e2e_n" -eq 37 ] || note "e2e_cases 条数变了（$e2e_n != 37）：加/删用例后请同步本检查点"
[ "$parse_n" -eq 96 ] || note "parse_cases 条数变了（$parse_n != 96）：加/删用例后请同步本检查点"
[ "$e2e_bad" -eq 0 ] || note "e2e_cases 有 $e2e_bad 条字段数不符（应为 name|浮点模式|最小条数）"
[ "$parse_bad" -eq 0 ] || note "parse_cases 有 $parse_bad 条字段数不符（应为 sql|mt|期望退出码[|expect[|forbid[|via]]]）"
[ "$e2e_unterm" -eq 0 ] || note "e2e_cases 有 $e2e_unterm 条未以引号收尾（漏了 ' 会吞掉后续行）"
[ "$parse_unterm" -eq 0 ] || note "parse_cases 有 $parse_unterm 条未以引号收尾（漏了 ' 会吞掉后续行）"
dup_e2e="$(printf '%s\n' "$e2e_rows" | sed "s/^[[:space:]]*'//; s/|.*//" | sort | uniq -d | tr '\n' ' ')"
[ -z "$dup_e2e" ] || note "e2e_cases 名字重复：$dup_e2e"
dup_parse="$(printf '%s\n' "$parse_rows" | sed "s/^[[:space:]]*'//; s/|/ /; s/|.*//" | sort | uniq -d | tr '\n' ' ')"
[ -z "$dup_parse" ] || note "parse_cases 的 (sql, mt) 组合重复：$dup_parse"

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

# 7) 守门常量单点（F3-5 余部）：TEST_TIMEOUT/SAN_DIR 默认值唯一来源
#    test/lib/params.sh（bash 直接 source、CMake 正则提取、CI 同源）。格式
#    漂移（如 CMake 侧正则依赖的 `: "${KEY:=v}"` 形态被改）会让 ctest TIMEOUT
#    在 configure 期 FATAL——这里再钉一层，让漂移在 selftest 就显形。
for k in TEST_TIMEOUT SAN_DIR; do
	grep -qE "^: \"\\\$\{${k}:=[0-9a-zA-Z._/-]+\}\"$" test/lib/params.sh \
		|| note "params 单点缺 ${k} 条目或格式漂移（CMake 正则提取会失败）：test/lib/params.sh"
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
#     条目数和磁盘文件数三者自洽、每篇 docs/*.md 都挂在索引上。
#     文档不会编译、不会失败，改名/删除/新增之后只会静静地指向不存在的东西；同一条
#     计数还手写三份（README、docs/README.md、drivers.md），各漂一点就没人知道哪个真。
#     历史与占位引用（changelog 是冻结账本、教程里的 modules/X.lua）在脚本的
#     KNOWN_ABSENT 里逐条登记并写明理由；登记过的条目若又存在了，脚本同样判失败
#     （名单过期也是一份不一致）。
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
if command -v python3 >/dev/null 2>&1; then
	python3 scripts/check_doc_paths.py \
		|| note "scripts/check_doc_paths.py 发现文档与仓库实况不一致（详见上方输出）"
	python3 scripts/check_llm_field_whitelist.py \
		|| note "scripts/check_llm_field_whitelist.py 发现配置字段白名单双端不一致（详见上方输出）"
	python3 scripts/check_stateful_protocol.py \
		|| note "scripts/check_stateful_protocol.py 发现有状态协议判据双端不一致（详见上方输出）"
else
	note "缺 python3，无法跑 scripts/check_doc_paths.py、scripts/check_llm_field_whitelist.py 与 scripts/check_stateful_protocol.py（文档一致性/配置白名单/有状态协议判据未受检查）"
fi

if [ "$fail" -ne 0 ]; then
	echo "run_all_selftest: $fail 项不合格" >&2
	exit 1
fi
echo "run_all_selftest: 通过（语法 / C++ 用例登记 / Lua 命名 / count_calls / 用例表结构 / TSan 名单 / params 单点 / 仿真器跨构建契约 / 状态目录缺省名 / 文档与仓库实况 / 配置字段白名单双端 / 有状态协议判据双端）"
