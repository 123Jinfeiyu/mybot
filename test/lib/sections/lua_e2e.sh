# test/lib/sections/lua_e2e.sh —— 守门分段 runner（自 run_all.sh 抽出，F3-1）
#
# 抽取原则（零行为变化）：**只搬函数定义**。用例表、调用循环、记账与汇总
# 全部留在 run_all.sh——`count_calls` 数的是 run_all.sh 里的调用点，
# `$SELF` 孤儿检测也以它为准；这些函数只被 run_all.sh source，不单独执行。
# 它们依赖 run_all.sh 的全局量（$SELF/$bin/$tests_bin/$filter/record_*/用例表），
# 因此**只能在 source 之后调用**，且不要在本文件里读 $1 之类的入口参数。

run_one_e2e() {
	local name="$1" float_ere="$2" count_min="$3"
	echo "----- $name (mybot E2E) -----"
	# 动作时长约 1s，给 30s 上限防挂死；mybot 退出码原样透传
	if [ -n "$timeout_cmd" ]; then
		out="$(cat "scripts/test/$name.mt" | timeout 30 "$bin" --robot="robots/test/$name.sql" 2>&1)"
	else
		out="$(cat "scripts/test/$name.mt" | "$bin" --robot="robots/test/$name.sql" 2>&1)"
	fi
	local rc=$?
	echo "$out"
	local e2e_ok=true
	[ $rc -ne 0 ] && e2e_ok=false
	echo "$out" | grep -qF "VIRTUAL HARDWARE" || e2e_ok=false
	echo "$out" | grep -qE "chunk setup error|chunk load error|setup failed|tick error|cleanup error|解析失败" \
		&& e2e_ok=false
	echo "$out" | grep -qF "joined, total publishes=" || e2e_ok=false
	# 守护持续发布：出现 ≥2 个不同 count 且最大值达标
	local count_max count_uniq
	count_max="$(echo "$out" | grep -oE '"count":[0-9]+' | grep -oE '[0-9]+' | sort -n | tail -1)"
	count_uniq="$(echo "$out" | grep -oE '"count":[0-9]+' | sort -u | wc -l)"
	{ [ -z "$count_max" ] || [ "$count_max" -lt "$count_min" ] || [ "$count_uniq" -lt 2 ]; } && e2e_ok=false
	# raw 快照里 :f 浮点字段保持小数（未被 sign=16 取整补码）
	echo "$out" | grep -qE "$float_ere" || e2e_ok=false
	# EMA 已推进：至少一行 monf 快照 out ≠ raw（首拍种子相同，后续 α 混合必不同）。
	# JSON 键序不固定，逐行各自提取 out/raw 数值做比对。
	local mixed_n=0 o r
	while IFS= read -r line; do
		o="$(printf '%s' "$line" | grep -oE '"out":-?[0-9]+(\.[0-9]+)?' | head -1 | cut -d: -f2)"
		r="$(printf '%s' "$line" | grep -oE '"raw":-?[0-9]+(\.[0-9]+)?' | head -1 | cut -d: -f2)"
		if [ -n "$o" ] && [ -n "$r" ] && [ "$o" != "$r" ]; then
			mixed_n=$((mixed_n + 1))
		fi
	done < <(printf '%s' "$out" | grep -F '"raw"')
	[ "$mixed_n" -ge 1 ] || e2e_ok=false
	if $e2e_ok; then
		record_pass
	else
		record_fail "$name(E2E)"
	fi
}

run_one_parse() {
	local sql="$1" mt="$2" want_rc="$3" expect="$4" forbid="$5" via="${6:-}"
	echo "----- $sql/$mt (no-hw contract) -----"
	local sql_path="$sql" mt_path="$mt"
	[[ "$sql" != */* ]] && sql_path="robots/test/$sql"
	[[ "$mt" != */* ]] && mt_path="scripts/test/$mt"
	if [ "$via" = "I" ]; then
		# --input 文件通道：include 基准 = 脚本所在目录（片段夹具的正确基准）
		if [ -n "$timeout_cmd" ]; then
			out="$($timeout_cmd "$bin" --robot="$sql_path" --input="$mt_path" 2>&1)"
		else
			out="$("$bin" --robot="$sql_path" --input="$mt_path" 2>&1)"
		fi
	elif [ -n "$timeout_cmd" ]; then
		out="$(cat "$mt_path" | timeout 30 "$bin" --robot="$sql_path" 2>&1)"
	else
		out="$(cat "$mt_path" | "$bin" --robot="$sql_path" 2>&1)"
	fi
	local rc=$?
	echo "$out"
	local p_ok=true
	[ "$rc" -ne "$want_rc" ] && p_ok=false
	echo "$out" | grep -qF "$expect" || p_ok=false
	# 可选「禁止出现」断言（用例表第 5 字段）：用于「修复后不再出现的旧症状」
	# （如 $ 引用已释放变量的 Deactive warning、build 期不该发生的 spawned）
	if [ -n "$forbid" ]; then
		echo "$out" | grep -qF "$forbid" && p_ok=false
	fi
	if $p_ok; then
		record_pass
	else
		record_fail "$sql/$mt(no-hw)"
	fi
}

run_parse_cases() {
	local spec sql rest mt want_rc f expect forbid via
	for spec in "${parse_cases[@]}"; do
		sql="${spec%%|*}"
		rest="${spec#*|}"
		mt="${rest%%|*}"
		rest="${rest#*|}"
		want_rc="${rest%%|*}"
		rest="${rest#*|}"
		# 剩余字段：expect[|forbid[|via]]（via="I" = --input 文件通道，见用例表注释）
		IFS='|' read -ra f <<< "$rest"
		expect="${f[0]:-}"
		forbid="${f[1]:-}"
		via="${f[2]:-}"
		if [ -n "$filter" ] && [[ "$sql" != *"$filter"* ]] && [[ "$mt" != *"$filter"* ]]; then
			continue
		fi
		run_one_parse "$sql" "$mt" "$want_rc" "$expect" "$forbid" "$via"
	done
}

run_one_mtmerge() {
	local name="$1" want_rc="$2" expects="$3"; shift 3
	echo "----- $name (mtmerge) -----"
	if [ ! -x "$mtmerge_bin" ]; then
		if [ -z "$mtmerge_reported" ]; then
			mtmerge_reported=1
			artifact_missing "mtmerge 合成器" "$mtmerge_cases_n 条用例" "$mtmerge_bin" "$mtmerge_explicit"
		fi
		return 2
	fi
	local out rc ok=true e
	out="$("$@" 2>&1)"
	rc=$?
	echo "$out"
	[ "$rc" -ne "$want_rc" ] && ok=false
	local IFS='|'
	for e in $expects; do
		echo "$out" | grep -qF -- "$e" || ok=false
	done
	if $ok; then
		record_pass
	else
		record_fail "$name(mtmerge)"
	fi
}

run_include_observation() {
	if [ ! -x "$bin" ]; then
		artifact_missing "@Include 归并观察面" "2 条用例" "$bin" "$bot_explicit"
		return 2
	fi
	# ---- include_main ----
	echo "----- include-main (@Include 归并观察面) -----"
	local out rc i_ok v
	out="$(cat scripts/test/include_main.mt | timeout 30 "$bin" --robot=robots/test/include_demo.sql 2>&1)"
	rc=$?
	echo "$out"
	i_ok=true
	[ "$rc" -ne 0 ] && i_ok=false
	for v in 100 110 200 210 220; do
		echo "$out" | grep -qF "test=$v" || i_ok=false
	done
	echo "$out" | grep -qF "test=0" && i_ok=false
	echo "$out" | grep -qF '"slept":200' || i_ok=false
	if $i_ok; then
		record_pass
	else
		record_fail "include-main(@Include 归并)"
	fi
	# ---- diamond ----
	echo "----- include-diamond (include-once 观察面) -----"
	local dout d_ok skip_n
	dout="$(cat scripts/test/include_diamond.mt | timeout 30 "$bin" --robot=robots/test/include_demo.sql 2>&1)"
	rc=$?
	echo "$dout"
	d_ok=true
	[ "$rc" -ne 0 ] && d_ok=false
	for v in 100 200 300; do
		echo "$dout" | grep -qF "test=$v" || d_ok=false
	done
	skip_n="$(echo "$dout" | grep -cF "already included")"
	[ "$skip_n" -eq 1 ] || d_ok=false
	if $d_ok; then
		record_pass
	else
		record_fail "include-diamond(include-once)"
	fi
}

run_one_mtmerge_phase() {
	echo "----- mtmerge-phase (相位语义) -----"
	if [ ! -x "$mtmerge_bin" ]; then
		artifact_missing "mtmerge 相位回归" "该段（scripts/mtmerge_e2e.sh）" "$mtmerge_bin" "$mtmerge_explicit"
		return 2
	fi
	local out rc
	out="$(MTMERGE="$mtmerge_bin" BOT="$bin" timeout 60 bash scripts/mtmerge_e2e.sh 2>&1)"
	rc=$?
	echo "$out"
	if [ $rc -ne 0 ] || ! echo "$out" | grep -qF "mtmerge 端到端测试通过"; then
		record_fail "mtmerge-phase(相位语义)"
	else
		record_pass
	fi
}

