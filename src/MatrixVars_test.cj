/*  硬件变量基类纯逻辑单元测试（MatrixVars_Hardware）
*
*  覆盖三类此前只被端到端间接扫过的语义：
*   ① `-` 占位符继承链（getMt 与 parseMt 对首行的处理不同，易被误改）
*   ② ensureOk 返回契约（halt-on-error 的唯一判定点，所有 Lua 驱动依赖它）
*   ③ logD 惰性求值（0.3.3 GC 崩溃复盘的修复点：非 debug 下不得构造日志字符串）
*/

package bot4cj
import std.unittest.*
import bot4cj.HV.*
import stdx.encoding.json.*
import mt2.*

/* ensureOk 的 Unit 包装：throwsWith 需要 () -> Unit 实参；顺带断言成功路径的返回类型 */
func okCall(res: JsonValue, ctx: String): Unit {
	let back = MatrixVars_Hardware.ensureOk(res, ctx: ctx)
	yes(back is JsonObject || back is JsonString)
}

func errResult(): JsonValue {
	let res = JsonObject()
	res.put("result", JsonString("ERR"))
	return res
}

@Test
class MatrixVarsTest {
	/* ---- getMt：`-` 逐行回看上一行同列的字面值 ---- */
	@TestCase
	func getMtInheritsFromPreviousRow() {
		let mt = mtOf("Servo", [["name", "angle"], ["s1", "30"], ["s2", "-"], ["s3", "-"]])
		eq("30", MatrixVars_Hardware.getMt(mt, 1, 1))
		eq("30", MatrixVars_Hardware.getMt(mt, 2, 1))
		eq("30", MatrixVars_Hardware.getMt(mt, 3, 1))
	}

	/* ---- 标题行不参与继承：仅 row > 1 才回看 ---- */
	@TestCase
	func getMtStopsAtHeaderRow() {
		let mt = mtOf("Servo", [["name", "angle"], ["s1", "-"]])
		eq("-", MatrixVars_Hardware.getMt(mt, 1, 1))
	}

	@TestCase
	func getMtLiteralWins() {
		let mt = mtOf("Servo", [["name", "angle"], ["s1", "30"], ["s2", "45"]])
		eq("45", MatrixVars_Hardware.getMt(mt, 2, 1))
	}

	/* ---- parseMt：首行 `-` 落 0（getMt 原样返回 "-"，两者语义不同）---- */
	@TestCase
	func parseMtFirstRowDashIsZero() {
		let mt = mtOf("Servo", [["name", "angle"], ["s1", "-"]])
		eq(0.0f64, MatrixVars_Hardware.parseMt<Float64>(mt, 1, 1))
		eq(0i64, MatrixVars_Hardware.parseMt<Int64>(mt, 1, 1))
	}

	@TestCase
	func parseMtInheritsAndParses() {
		let mt = mtOf("Servo", [["name", "angle"], ["s1", "30"], ["s2", "-"]])
		eq(30.0f64, MatrixVars_Hardware.parseMt<Float64>(mt, 2, 1))
		eq(30i64, MatrixVars_Hardware.parseMt<Int64>(mt, 2, 1))
	}

	/* ---- ensureOk：无 result 字段视为成功（Relay/Servo 一类转发驱动的既有契约）---- */
	@TestCase
	func ensureOkTreatsMissingResultAsSuccess() {
		let res = JsonObject()
		res.put("v0", JsonFloat(1.5))
		yes(MatrixVars_Hardware.ensureOk(res, ctx: "play x") is JsonObject)
	}

	@TestCase
	func ensureOkPassesThroughOk() {
		let res = JsonObject()
		res.put("result", JsonString("OK"))
		hasText(MatrixVars_Hardware.ensureOk(res, ctx: "build y").toString(), "OK")
	}

	/* ---- ERR 必须抛出，ctx 与 message 都进文案（排障时的唯一定位信息）---- */
	@TestCase
	func ensureOkThrowsOnErrWithContext() {
		let res = errResult()
		res.asObject().put("message", JsonString("No such device at 0x68"))
		throwsWith("Hardware call failed", { => okCall(res, "play imu0 (row 3)") })
		throwsWith("play imu0 (row 3)", { => okCall(res, "play imu0 (row 3)") })
		throwsWith("No such device at 0x68", { => okCall(res, "play imu0") })
	}

	/* ---- 缺 message 时仍给出带 ctx 的兜底文案；result 非字符串不参与判定 ---- */
	@TestCase
	func ensureOkWithoutMessageAndWithNonStringResult() {
		throwsWith("Hardware call failed", { => okCall(errResult(), "call x") })

		let weird = JsonObject()
		weird.put("result", JsonInt(3))
		yes(MatrixVars_Hardware.ensureOk(weird, ctx: "call y") is JsonObject)
	}

	/* ---- 尾随空白的 result 仍判为 ERR（trimAscii 契约）---- */
	@TestCase
	func ensureOkTrimsResult() {
		let res = JsonObject()
		res.put("result", JsonString("ERR "))
		throwsWith("Hardware call failed", { => okCall(res, "call z") })
	}

	/* ---- 非对象返回（别名驱动的 JsonString）原样透传，不得抛 ---- */
	@TestCase
	func ensureOkPassesNonObject() {
		eq("angle", MatrixVars_Hardware.ensureOk(JsonString("angle"), ctx: "read alias").asString().getValue())
	}

	/* ---- logD 惰性：未开 debug 时消息体一次都不求值
	* （0.3.3 复盘：热路径无条件求值 `${obj.toString()}` 抬高分配速率，触发仓颉
	*   并发 GC 缺陷；签名改 () -> String 正是为从类型上根除该问题）---- */
	@TestCase
	func logDoesNotEvaluateMessageWhenQuiet() {
		if (MatrixVars_Hardware.debugEnabled) {
			fail("本机环境开启了 BOT4CJ_DEBUG，本用例需在缺省（关闭）状态下运行")
		}
		let box = Box()
		MatrixVars_Hardware.logD({ => box.n += 1; "never" })
		eq(0i64, box.n)
	}
}
