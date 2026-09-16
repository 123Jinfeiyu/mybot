/*  机器人结构解析与元数据收集的单元测试（RobotStruct.cj / MetaProvider）
*
*  这一层此前完全依赖端到端冒烟：解析期校验（表未定义/重复列/缺列/未知列）与
*  引用列语义（P4/M1 实时快照、P8/M2 静态列缓存）都没有独立用例。
*  本文件用内存输入流直接构造结构脚本，不触碰任何硬件与 Lua 状态机。
*/

package bot4cj
import std.unittest.*
import std.collection.*
import stdx.encoding.json.*
import mt2.*
import bot4cj.HV.*

/* ---- 引用测试用的假硬件：read() 返回随 box.n 变化的快照，用于验「实时」语义 ---- */
func fakeSnap(n: Int64): JsonValue {
	let o = JsonObject()
	o.put("v0", JsonFloat(0.5 + Float64(n)))
	o.put("count", JsonInt(n))
	return o
}

class FakeHw <: MatrixVars_Hardware {
	public init(box: Box) {
		super(things: { _: JsonValue => fakeSnap(box.n) }, rise: None, free: None)
	}
}

/* 一张最小的 Kalman 表：name / Q / src(引用列) / skip(占位符) / controller */
func kalmanMt(refValue: String): MatrixObject {
	return mtOf("Kalman", [
		["name", "Q", "src", "skip", "controller"],
		["kf0", "20", refValue, "-", "-"],
	])
}

@Test
class RobotStructTest {
	/* SQL 解析期校验（表未定义/重复列/缺列/未知列）需要真实输入流，
	* 本 SDK 无内存 InputStream 实现，改由 test/run_all.sh 的
	* 「结构脚本负向端到端」夹具覆盖（退出码 + 文案双重锚定）。 */

	/* ---- MetaProvider：name 列、controller 列、`-` 占位列都不进元数据 ---- */
	@TestCase
	func collectSkipsNameControllerAndDash() {
		let vars = HashMap<String, MatrixVars_Hardware>()
		let mp = MetaProvider(kalmanMt("-"), 1, 4, vars)
		let obj = mp.collect().asObject()
		hasText(obj.toString(), "Q")
		eq(false, obj.getFields().contains("name"))
		eq(false, obj.getFields().contains("controller"))
		eq(false, obj.getFields().contains("skip"))
	}

	/* ---- 裸名按字面值处理（0.3.1 及之前的「值恰为变量名即引用」已废弃）---- */
	@TestCase
	func bareNameIsLiteralNotReference() {
		let vars = HashMap<String, MatrixVars_Hardware>()
		let box = Box()
		vars["imu0"] = FakeHw(box)
		let obj = MetaProvider(kalmanMt("imu0"), 1, 4, vars).collect().asObject()
		eq("imu0", obj.getFields().get("src").getOrThrow().asString().getValue())
		no(obj.getFields().contains("v0"))
	}

	/* ---- 引用列每次实时 read()：count/nvals 等动态值必须反映当前状态（P4/M1）---- */
	@TestCase
	func referenceColumnReadsLiveSnapshot() {
		let vars = HashMap<String, MatrixVars_Hardware>()
		let box = Box()
		vars["imu0"] = FakeHw(box)
		let mp = MetaProvider(kalmanMt("$imu0"), 1, 4, vars)

		let first = mp.collect().asObject()
		hasText(first.toString(), "v0")
		eq(0i64, first.getFields().get("count").getOrThrow().asInt().getValue())

		box.n = 7
		let second = mp.collect().asObject()
		eq(7i64, second.getFields().get("count").getOrThrow().asInt().getValue())
	}

	/* ---- 静态列缓存每次返回全新对象：调用方在副本上写参数不得污染下一次（守护多线程 play 安全）---- */
	@TestCase
	func collectReturnsFreshObjectEachTime() {
		let vars = HashMap<String, MatrixVars_Hardware>()
		let mp = MetaProvider(kalmanMt("-"), 1, 4, vars)
		let first = mp.collect()
		first.put("mode", JsonString("call"))
		/* 不比较对象身份（本 SDK 无引用相等运算符），而是断言其效果：
		* 上一次返回对象被写入字段后，下一次采集的结果不得带上该字段 */
		eq(false, mp.collect().asObject().getFields().contains("mode"))
	}

	/* ---- $ 指向未定义变量属拼写错误：显式报错并给出表/列/行/值 ---- */
	@TestCase
	func danglingReferenceThrowsWithContext() {
		let vars = HashMap<String, MatrixVars_Hardware>()
		throwsWith("is not defined", { => MetaProvider(kalmanMt("$nope"), 1, 4, vars).collect() })
		throwsWith("column 'src'", { => MetaProvider(kalmanMt("$nope"), 1, 4, vars).collect() })
	}

	/* ---- @Alias 变量不可被 $ 引用（回归：曾抛裸 JsonException 'Fail to convert to JsonObject'）
	* 别名只展开 @Action 列头的参数名，read() 返回单个字符串、无字段可合并 ---- */
	@TestCase
	func referenceToAliasThrowsFriendlyError() {
		let vars = HashMap<String, MatrixVars_Hardware>()
		let aliasMt = mtOf("Alias", [["name", "alias"], ["A", "angle"]])
		vars["A"] = MtVars_Alias.Factory(aliasMt, 1, vars)

		throwsWith("references alias variable", { =>
			MetaProvider(kalmanMt("$A"), 1, 4, vars).collect()
		})
		throwsWith("An @Alias variable expands a parameter name", { =>
			MetaProvider(kalmanMt("$A"), 1, 4, vars).collect()
		})
	}
}
