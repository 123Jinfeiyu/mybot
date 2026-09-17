#!/usr/bin/env bash
# scripts/mtmerge_e2e.sh — mtmerge 离线合成器回归
#
# 覆盖：
#   1. 相位 0 / 正 / 负 的逐格归并（规范化文本精确比对）
#   2. 设备集相交 → 退出码 2 且点名冲突设备
#   3. 三输入链式归并
#   4. 负相位丢内容告警（含全部丢弃）
#   5. 合并产物回喂 mybot：相位前导 "-" 的设备必须被跳过（不得 play 0.0）
#
# 用法：./scripts/mtmerge_e2e.sh
# 依赖：mtmerge / mybot 可执行文件（MTMERGE / BOT 覆盖，缺省 ./build/*），
#       modules/Timer.lua、modules/Relay.lua
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1
MTMERGE="${MTMERGE:-./build/mtmerge}"
BOT="${BOT:-./build/mybot}"

echo "=== mtmerge 离线合成器回归 ==="
echo "mtmerge: $MTMERGE  mybot: $BOT"

if [ ! -x "$MTMERGE" ]; then
	echo "错误：找不到可执行文件 '$MTMERGE'（需先编译）"
	exit 1
fi
if [ ! -x "$BOT" ]; then
	echo "错误：找不到可执行文件 '$BOT'（回喂冒烟需要）"
	exit 1
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/mtmerge_e2e.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

pass=0
fail=0
failed=""

# 规范化 .mt 文本：折叠空白、去掉分号前空格与行首尾空格、删空行。
# 这样期望文本可以手写（不受 MatrixSaver 的尾随空格影响），又保留
# 列序 / 行序 / 单元格值 / "-" 占位的精确比对。
norm() {
	sed -e 's/[[:space:]]\+/ /g' -e 's/ ;/;/g' -e 's/^ //' -e 's/ $//' -e '/^$/d'
}

assert_eq() { # name expected actual
	local name="$1" exp="$2" act="$3"
	if [ "$exp" = "$act" ]; then
		echo "[PASS] $name"
		pass=$((pass + 1))
	else
		echo "[FAIL] $name"
		diff <(printf '%s\n' "$exp") <(printf '%s\n' "$act") || true
		fail=$((fail + 1))
		failed="$failed $name"
	fi
}

assert_rc() { # name want actual
	local name="$1" want="$2" act="$3"
	if [ "$want" = "$act" ]; then
		echo "[PASS] $name"
		pass=$((pass + 1))
	else
		echo "[FAIL] $name（期望 rc=$want，实际 rc=$act）"
		fail=$((fail + 1))
		failed="$failed $name"
	fi
}

assert_contains() { # name needle haystack
	local name="$1" needle="$2" hay="$3"
	if printf '%s\n' "$hay" | grep -qF -- "$needle"; then
		echo "[PASS] $name"
		pass=$((pass + 1))
	else
		echo "[FAIL] $name（未见 '$needle'）"
		printf '%s\n' "$hay" | tail -8
		fail=$((fail + 1))
		failed="$failed $name"
	fi
}

assert_not_contains() { # name needle haystack
	local name="$1" needle="$2" hay="$3"
	if printf '%s\n' "$hay" | grep -qF -- "$needle"; then
		echo "[FAIL] $name（不应出现 '$needle'）"
		printf '%s\n' "$hay" | tail -8
		fail=$((fail + 1))
		failed="$failed $name"
	else
		echo "[PASS] $name"
		pass=$((pass + 1))
	fi
}

# ---------------- 夹具 ----------------
cat > "$WORK/a.mt" <<'EOF'
@Action
s1.A s2.B;
  10   20;
  11   -;
@
EOF

cat > "$WORK/b.mt" <<'EOF'
@Action
s3.C;
  30;
  31;
  32;
@
EOF

cat > "$WORK/c.mt" <<'EOF'
@Action
s1.A;
  99;
@
EOF

cat > "$WORK/d.mt" <<'EOF'
@Action
s4.D;
  40;
@
EOF

# ---------------- 1. 相位归并 ----------------
got="$( "$MTMERGE" "$WORK/a.mt" "$WORK/b.mt" | norm )"
exp="$(norm <<'EOF'
@Action
s1.A s2.B s3.C;
10 20 30;
11 - 31;
- - 32;
@
EOF
)"
assert_eq "phase0" "$exp" "$got"

got="$( "$MTMERGE" --phase=1 "$WORK/a.mt" "$WORK/b.mt" | norm )"
exp="$(norm <<'EOF'
@Action
s1.A s2.B s3.C;
10 20 -;
11 - 30;
- - 31;
- - 32;
@
EOF
)"
assert_eq "phase_pos1" "$exp" "$got"

got="$( "$MTMERGE" --phase=-1 "$WORK/a.mt" "$WORK/b.mt" | norm )"
exp="$(norm <<'EOF'
@Action
s1.A s2.B s3.C;
10 20 31;
11 - 32;
@
EOF
)"
assert_eq "phase_neg1" "$exp" "$got"

# ---------------- 2. 设备集相交 ----------------
out="$( "$MTMERGE" "$WORK/a.mt" "$WORK/c.mt" 2>&1 )"
rc=$?
assert_rc "conflict_rc" 2 "$rc"
assert_contains "conflict_msg" "device 's1' appears in both scripts" "$out"

# ---------------- 3. 三输入链式归并 ----------------
got="$( "$MTMERGE" "$WORK/a.mt" "$WORK/b.mt" "$WORK/d.mt" | norm )"
exp="$(norm <<'EOF'
@Action
s1.A s2.B s3.C s4.D;
10 20 30 40;
11 - 31 -;
- - 32 -;
@
EOF
)"
assert_eq "chain3" "$exp" "$got"

# ---------------- 4. 负相位丢内容告警 ----------------
err="$( "$MTMERGE" --phase=-1 "$WORK/a.mt" "$WORK/b.mt" 2>&1 >/dev/null )"
assert_contains "warn_partial" "'$WORK/b.mt' 的前 1 行被丢弃" "$err"

err="$( "$MTMERGE" --phase=-5 "$WORK/a.mt" "$WORK/b.mt" 2>&1 >/dev/null )"
assert_contains "warn_all" "全部内容" "$err"

got="$( "$MTMERGE" --phase=-5 "$WORK/a.mt" "$WORK/b.mt" 2>/dev/null | norm )"
exp="$(norm <<'EOF'
@Action
s1.A s2.B s3.C;
10 20 -;
11 - -;
@
EOF
)"
assert_eq "phase_neg_all" "$exp" "$got"

# ---------------- 5. 回喂 mybot：前导 "-" 必须跳过 ----------------
cat > "$WORK/two_relay.sql" <<'EOF'
CREATE TABLE Timer ( name String );
INSERT INTO Timer (name) VALUES ('timer-0');

CREATE TABLE Relay ( name String, controller String, note String );
INSERT INTO Relay (name, controller, note) VALUES ('relayA', 'timer-0', 'A');
INSERT INTO Relay (name, controller, note) VALUES ('relayB', 'timer-0', 'B');
EOF

cat > "$WORK/mA.mt" <<'EOF'
@Action
relayA.test;
   100;
   101;
@
EOF

cat > "$WORK/mB.mt" <<'EOF'
@Action
relayB.test;
   50;
@
EOF

merged="$( "$MTMERGE" --phase=1 "$WORK/mA.mt" "$WORK/mB.mt" )"
bot_out="$(printf '%s' "$merged" | "$BOT" --robot="$WORK/two_relay.sql" 2>&1)"
bot_rc=$?
assert_rc "feed_mybot_rc" 0 "$bot_rc"
assert_contains "feed_mybot_relayA" "Relay: test=100 note=A" "$bot_out"
assert_contains "feed_mybot_relayB" "Relay: test=50 note=B" "$bot_out"
# 修复前：relayB 在相位前导行被 play(0.0)，会打印 test=0
assert_not_contains "feed_mybot_leadin_skipped" "test=0 note" "$bot_out"
assert_not_contains "feed_mybot_no_error" "Aborted" "$bot_out"

# ---------------- 汇总 ----------------
echo "-----------------------------------------------"
if [ "$fail" -eq 0 ]; then
	echo "=== mtmerge 端到端测试通过（$pass 项）==="
	exit 0
fi
echo "[FAIL] mtmerge 回归：$pass 通过, $fail 失败"
echo "失败项:$failed"
exit 1
