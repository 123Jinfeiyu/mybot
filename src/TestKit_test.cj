/*  仓颉单元测试共享断言层（`*_test.cj` 只被 cjpm test 编译，不进 cjpm build 产物）
*
*  为什么自带一层：SDK 1.1.3 的 std.unittest 只导出 assertEqual，且必须写全六个
*  实参（isDelta: / optParentCtx: 为命名参数）、泛型不做实参推断，用例标注是
*  @TestCase（没有 @Case）。裸调会让每条断言淹没在样板噪声里，故收敛成
*  eq/yes/no/hasText/throwsWith 短名。失败仍由 std.unittest 统一报告并决定退出码。
*
*  注意：test 文件的声明不会进入包的模块接口，跨包不可见——所以全部单测统一放在
*  根包 bot4cj（经 import bot4cj.HV.* 使用被测的 public API），本层只此一份。
*/

package bot4cj
import std.unittest.*
import mt2.*

/* ---- 按类型重载的等值断言（assertEqual 逐类型显式展开，泛型辅助无法推断实参）---- */
public func eq(expected: Int64, actual: Int64): Unit {
	assertEqual("", "", expected, actual, isDelta: false, optParentCtx: None)
}
public func eq(expected: String, actual: String): Unit {
	assertEqual("", "", expected, actual, isDelta: false, optParentCtx: None)
}
public func eq(expected: Bool, actual: Bool): Unit {
	assertEqual("", "", expected, actual, isDelta: false, optParentCtx: None)
}
public func eq(expected: Float64, actual: Float64): Unit {
	assertEqual("", "", expected, actual, isDelta: false, optParentCtx: None)
}

public func yes(cond: Bool): Unit { eq(true, cond) }
public func no(cond: Bool): Unit { eq(false, cond) }

/* ---- 子串断言：错误文案是对外契约的一部分（表/列/行/值上下文不得丢）---- */
public func hasText(hay: String, needle: String): Unit {
	if (!hay.contains(needle)) {
		fail("期望文案包含 '${needle}'，实际：${hay}")
	}
}

/* ---- 断言抛出异常且文案含关键字 ---- */
public func throwsWith(needle: String, body: () -> Unit): Unit {
	var msg = ""
	var threw = false
	try {
		body()
	} catch (e: Exception) {
		threw = true
		msg = "${e}"
	}
	if (!threw) {
		fail("期望抛出含 '${needle}' 的异常，实际未抛出")
	}
	hasText(msg, needle)
}

/* ---- 可变状态持有者：Cangjie 禁止 lambda 捕获可变变量，
* 惰性/副作用类断言经 let 引用的对象改字段来计数（同 DaemonHolder 的做法）---- */
public class Box {
	public var n = 0
}

/* ---- 由字符串阵列构造 MatrixObject（第 0 行为标题行）----
* 构造形状与 RobotStruct.readSQL 一致：submitName → 逐格 submitItem → submitLine。 */
public func mtOf(name: String, rows: Array<Array<String>>): MatrixObject {
	let mt = MatrixObject()
	addWord(mt, name)
	mt.submitName()
	for (row in rows) {
		for (cell in row) {
			addWord(mt, cell)
			mt.submitItem()
		}
		mt.submitLine(0)
	}
	return mt
}

func addWord(mt: MatrixObject, s: String): Unit {
	for (ch in s) {
		mt.addRune(Rune(ch))
	}
}
