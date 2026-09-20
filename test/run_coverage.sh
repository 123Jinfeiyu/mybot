#!/bin/bash
# test/run_coverage.sh —— C++ 侧行覆盖采集（CI 的 gate:coverage 复用）。
#
# 定位：覆盖率软门禁的**数据产出**入口。它不进 run_all.sh 的记账（守门汇总
# 口径不变），也不参与任何硬门禁——工具（gcovr/lcov）全都缺失时退化为 gcov
# 原生输出并照常产出报告，不会让既有硬门禁变红。
#
# 用法：bash test/run_coverage.sh
#   COV_BUILD_DIR（缺省 build-cov）/ COV_REPORT_DIR（缺省 build-cov/coverage，
#   随构建目录一并被 .gitignore 的 build-*/ 覆盖，不污染 git status）可覆盖。
set -u
cd "$(dirname "$0")/.." || exit 1

build_dir="${COV_BUILD_DIR:-build-cov}"
report_dir="${COV_REPORT_DIR:-$build_dir/coverage}"
mybot_tests="$build_dir/mybot_tests"

echo "===== 覆盖率构建（$build_dir，CMAKE_CXX_FLAGS=--coverage） ====="
# --coverage 同时作用于编译与链接（CMAKE_CXX_FLAGS 会进入 C++ 可执行文件的
# 链接行，EXE_LINKER_FLAGS 再显式兜一层），确保 libgcov 被链进来。
cmake -S . -B "$build_dir" -DCMAKE_BUILD_TYPE=Debug \
	-DCMAKE_CXX_FLAGS=--coverage -DCMAKE_EXE_LINKER_FLAGS=--coverage >/dev/null || exit 1
cmake --build "$build_dir" -j"$(nproc)" --target mybot_tests || exit 1

echo "===== 运行 mybot_tests 采集覆盖（CWD 必须是仓库根：用例读相对路径夹具） ====="
./"$mybot_tests" >/dev/null || exit 1

mkdir -p "$report_dir"
if command -v gcovr >/dev/null 2>&1; then
	echo "===== gcovr 摘要（src/） ====="
	gcovr --root . --filter 'src/' --print-summary \
		--txt "$report_dir/coverage.txt" --xml "$report_dir/coverage.xml"
elif command -v lcov >/dev/null 2>&1; then
	echo "===== lcov 摘要（src/） ====="
	lcov --capture --directory "$build_dir" --output-file "$report_dir/lcov.info" >/dev/null
	lcov --extract "$report_dir/lcov.info" '*/src/*' --output-file "$report_dir/lcov-src.info" >/dev/null
	lcov --summary "$report_dir/lcov-src.info"
else
	echo "===== 无 gcovr/lcov，退化为 gcov 原生输出（src/） ====="
	repo_root="$(pwd)"
	build_abs="$repo_root/$build_dir"
	(
		cd "$repo_root/$report_dir" || exit 1
		# .gcno 与源码同名（<objdir>/<src>.gcno），gcov 直接吃数据文件名即可；
		# .gcov 落到当前目录（report_dir）。
		while IFS= read -r gcno; do
			gcov -b -o "$(dirname "$gcno")" "$gcno" >/dev/null 2>&1 || true
		done < <(find "$build_abs" -path '*/src/*' -name '*.gcno' -print)
	)
	# 聚合 .gcov：只统计 Source 落在 src/ 的文件（一个 .gcno 会连带生成 libstdc++
	# 等系统头的 .gcov，必须按 Source 行过滤）；字段 1 是执行计数（`-` = 非可执行行，
	# 数字 = 已执行，`#####` = 未执行）。
	awk -F: '
		FNR == 1 { src_ok = 0 }
		FNR <= 8 && $3 == "Source" { if ($4 ~ /(^|\/)src\//) src_ok = 1; next }
		!src_ok { next }
		NF < 3 { next }
		{ cnt = $1; gsub(/^[ \t]+|[ \t]+$/, "", cnt) }
		cnt == "-" { next }
		{ total++; if (cnt ~ /^[0-9]+$/) exec++ }
		END {
			if (total > 0)
				printf "src/ 行覆盖：%.2f%%（%d/%d）\n", exec * 100 / total, exec, total
			else
				print "src/ 行覆盖：无数据（未生成 src/ 的 .gcov）"
		}
	' "$report_dir"/*.gcov
fi
echo "报告目录：$report_dir/"
