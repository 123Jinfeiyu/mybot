# test/lib/sections/accounting.sh —— 守门分段 runner（自 run_all.sh 抽出，F3-1）
#
# 抽取原则（零行为变化）：**只搬函数定义**。用例表、调用循环、记账与汇总
# 全部留在 run_all.sh——`count_calls` 数的是 run_all.sh 里的调用点，
# `$SELF` 孤儿检测也以它为准；这些函数只被 run_all.sh source，不单独执行。
# 它们依赖 run_all.sh 的全局量（$SELF/$bin/$tests_bin/$filter/record_*/用例表），
# 因此**只能在 source 之后调用**，且不要在本文件里读 $1 之类的入口参数。

record_pass() { pass=$((pass + 1)); }

record_fail() { fail=$((fail + 1)); failed="$failed $1"; }

record_skip() { skip=$((skip + 1)); skipped_list="$skipped_list $1"; }

count_calls() { grep -cE "^[[:space:]]*$1 ['\"]" "$SELF"; }

artifact_missing() {
	local phase="$1" count="$2" path="$3" explicit="$4"
	if [ "$ALLOW_SKIP" = "1" ] && [ "$explicit" != "1" ]; then
		echo "[SKIP] $phase —— $count未执行（缺编译产物 $path，ALLOW_SKIP=1 放行）"
		record_skip "$phase"
		return 0
	fi
	echo "[FAIL] 守门不完整：$phase —— $count未执行（缺可执行产物 $path）"
	echo "       先构建：cmake -S . -B build && cmake --build build"
	echo "       只想跑 Lua 段请显式声明：ALLOW_SKIP=1 bash test/run_all.sh"
	record_fail "$phase(缺产物)"
	return 1
}

count_e2e_declared() {
	local n=0 spec name
	for spec in "${e2e_cases[@]}"; do
		name="${spec%%|*}"
		if [ -z "$filter" ] || [[ "$name" == *"$filter"* ]]; then n=$((n + 1)); fi
	done
	echo "$n"
}

count_parse_declared() {
	local n=0 spec sql rest mt
	for spec in "${parse_cases[@]}"; do
		sql="${spec%%|*}"
		rest="${spec#*|}"
		mt="${rest%%|*}"
		if [ -z "$filter" ] || [[ "$sql" == *"$filter"* ]] || [[ "$mt" == *"$filter"* ]]; then
			n=$((n + 1))
		fi
	done
	echo "$n"
}

