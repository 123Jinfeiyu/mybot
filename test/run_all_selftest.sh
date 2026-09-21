#!/bin/bash
# test/run_all_selftest.sh —— 守门自身的自检（计划 M0 的 C7 起步，零行为变化）
#
# 守门脚本是「声称的防线」与「实际执行」之间唯一的仲裁者，但它自己没有任何测试：
#   - 用例不叫 test_*.lua，run_all.sh 的 glob 与 CMake 的 glob 都不会收集它；
#   - 新加的 test/cpp/*.cpp 不写进 CMakeLists.txt 就永不编译；
#   - 脚本自身的语法错误要到推送那一刻才暴露。
# 本自检只做三件**便宜且能机械判定**的事，且**不进入 run_all.sh 的记账**
# （守门汇总口径保持不变），由 pre-push 在守门之前调用。
set -u
cd "$(dirname "$0")/.." || exit 1

fail=0
note() { echo "[FAIL] selftest: $*" >&2; fail=$((fail + 1)); }

# 1) 守门相关脚本的语法（bash -n 不执行，纯解析）
for f in test/run_all.sh test/run_san.sh test/lib/device_gate.sh scripts/git-hooks/pre-push; do
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
	grep -qE "^[[:space:]]*${r}\(\)" test/run_all.sh \
		|| note "count_calls 的参数不是本文件里定义的 runner：$r"
	strict="$(grep -cE "^[[:space:]]*${r} ['\"]" test/run_all.sh)"
	loose="$(grep -cE "^[[:space:]]*${r}[[:space:]]" test/run_all.sh)"
	if [ "$strict" -eq 0 ]; then
		note "runner $r 的调用点计数为 0（写法变了？引号/缩进/改名）"
	elif [ "$strict" -ne "$loose" ]; then
		note "runner $r 计数不一致：count_calls 判据 $strict != 宽松计数 $loose（有写法没被认出来）"
	fi
done

if [ "$fail" -ne 0 ]; then
	echo "run_all_selftest: $fail 项不合格" >&2
	exit 1
fi
echo "run_all_selftest: 通过（脚本语法 / C++ 用例登记 / Lua 用例命名 / count_calls 计数）"
