#!/usr/bin/env bash
# scripts/ci.sh — 本地 CI 门禁（构建 + 全量回归 + 环境自证）
#
# 定位：GitCode 尚无流水线可用（见 CHANGELOG P0-4），本机 .git/hooks/pre-push
# 是唯一门禁，但钩子不入库、换机器即失效。本脚本把「构建 → 回归 → 判定」收敛成
# 一条可粘贴、可复现、退出码诚实的命令；平台 CI 就绪后直接把它当唯一步骤即可。
#
# 用法：
#   bash scripts/ci.sh                 # 全量：构建 + 回归
#   bash scripts/ci.sh --no-build      # 只跑回归（用现有产物）
#   bash scripts/ci.sh --filter uart   # 只跑名字含 uart 的用例
#   CI_STALE_OK=1 bash scripts/ci.sh   # 容忍产物过期（默认视为失败，见下）
#
# 为什么产物过期要判失败：test/run_all.sh 里有 37+ 组仓颉端到端直接跑
# target/release/bin/main，源码比产物新时，绿灯证明的是【旧二进制】的行为，
# 而非当前 src/ ——这正是「改了仓颉代码、Lua 测试全绿」的经典假绿路径。
#
# 输出末行给出环境自证：哪些能力在场（仓颉 E2E / 真机 i2c / cc ABI 校验），
# 缺哪项就明说对应用例被跳过——绿不等于全绿，覆盖面必须随结果一起打印。

set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

do_build=1
filter=""
while [ $# -gt 0 ]; do
	case "$1" in
		--no-build) do_build=0 ;;
		--filter)
			[ -z "${2:-}" ] && { echo "用法：--filter <子串>"; exit 2; }
			filter="$2"; shift ;;
		-h|--help) sed -n '2,25p' "$0"; exit 0 ;;
		*) echo "未知参数：$1（--no-build / --filter <子串>）"; exit 2 ;;
	esac
	shift
done

BIN="target/release/bin/main"
rc_total=0

echo "=== bot4cj 本地门禁 ==="
echo "arch      : $(uname -m)  kernel $(uname -r)"
echo "luajit    : $(luajit -v 2>/dev/null | head -1 || echo '缺失（回归将无法运行）')"

have_cj=0
if command -v cjc >/dev/null 2>&1 || [ -n "${CANGJIE_HOME:-}" ] || [ -d "$HOME/cangjie" ]; then
	have_cj=1
fi
[ -z "${CANGJIE_HOME:-}" ] && [ -d "$HOME/cangjie" ] && export CANGJIE_HOME="$HOME/cangjie"
echo "仓颉工具链 : $([ $have_cj = 1 ] && echo '在场（cjc/CANGJIE_HOME/~/cangjie）' || echo '缺失 → 仓颉端到端与 @Alias/TCP E2E 将跳过')"

# ---------------- 构建 ----------------
if [ $do_build = 1 ]; then
	if [ $have_cj = 1 ] && command -v cjpm >/dev/null 2>&1; then
		echo "--- 构建 (bash _set.sh) ---"
		if ! bash _set.sh > /tmp/bot4cj_ci_build.log 2>&1; then
			echo "[FAIL] 构建失败，日志末 30 行："
			tail -30 /tmp/bot4cj_ci_build.log
			exit 1
		fi
		echo "[OK] 构建成功（警告见 /tmp/bot4cj_ci_build.log）"
	else
		echo "[SKIP] 无 cjpm/仓颉工具链，跳过构建（将使用既有产物，如有）"
	fi
fi

# ---------------- 产物新鲜度 ----------------
if [ -x "$BIN" ]; then
	stale="$(find src -name '*.cj' ! -name '*_test.cj' -newer "$BIN" -print -quit 2>/dev/null)"
	if [ -n "$stale" ]; then
		if [ "${CI_STALE_OK:-0}" = "1" ]; then
			echo "[WARN] 源码比产物新（$stale）——仓颉 E2E 证明的是旧二进制行为，已按 CI_STALE_OK=1 放行"
		else
			echo "[FAIL] 源码比产物新（$stale）：请先构建（去掉 --no-build），或确认在测旧产物时设 CI_STALE_OK=1"
			exit 1
		fi
	fi
elif [ $have_cj = 1 ]; then
	echo "[WARN] 无产物 $BIN 且未能构建：仓颉 E2E 会全部跳过"
fi

# ---------------- 回归 ----------------
RUN_LOG=/tmp/bot4cj_ci_run.log
echo "--- 回归 (bash test/run_all.sh${filter:+ --filter $filter}) ---"
if [ -n "$filter" ]; then
	bash test/run_all.sh "$filter" 2>&1 | tee "$RUN_LOG"
else
	bash test/run_all.sh 2>&1 | tee "$RUN_LOG"
fi
rc_total="${PIPESTATUS[0]}"

# ---------------- 覆盖面自证 ----------------
# run_all.sh 对跳过的用例打 [SKIP] 并附原因；这里回显计数与去重后的原因，
# 使「N 通过」不被脱离覆盖面单独引用（绿不等于全绿）。
skip_n="$(grep -cF '[SKIP]' "$RUN_LOG" 2>/dev/null)"
[ -n "$skip_n" ] || skip_n=0
echo ""
echo "=== 结论 ==="
echo "退出码    : $rc_total"
echo "跳过用例  : $skip_n"
if [ "$skip_n" != "0" ]; then
	grep -F '[SKIP]' "$RUN_LOG" | sed 's/^\[SKIP\]/  - /' | sort -u | head -5
fi
echo "环境能力  : 仓颉 E2E $([ $have_cj = 1 ] && echo 可用 || echo 不可用)" \
	"| 真机 i2c $([ -n "$(ls /dev/i2c-* 2>/dev/null | head -1)" ] && echo 可用 || echo 不可用)" \
	"| ABI 交叉校验 $(command -v cc >/dev/null 2>&1 && echo 可用 || echo 不可用)" \
	"| TCP E2E $(command -v python3 >/dev/null 2>&1 && echo 可用 || echo 不可用)"

if [ $rc_total -eq 0 ]; then
	echo "判定      : 通过"
else
	echo "判定      : 失败（详见上方 [FAIL] 与汇总行）"
fi
exit $rc_total
