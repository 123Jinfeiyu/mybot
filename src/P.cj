package bot4cj
import std.io.*
import std.math.*
import std.convert.*
import std.collection.*

/*  -------- 单条动作的类型枚举 --------
 *
 *  INFO(20260515-qing):
 *  - 这是老版本解析器的固定命名含义枚举
 *  - 新版本中是相对解耦合的
 *  - 新版本成熟后，这个版本可能被删除
 */
enum ActType {

	SERVO | SPEAK | NOPE | UNKNOW
	/* -------- 以固定方式解析命名含义 -------- */
	public static func parse(nm: String): ActType	{
		if (nm.startsWith("s")) {  ActType.SERVO   }
		else if (nm == "spk") {  ActType.SPEAK  }
		else if (nm == "-") {  ActType.NOPE  }
		else {  ActType.UNKNOW  }
	}
}


/* -------- 单条动作类 --------
 * INFO(20260515-qing):
 * - 这是老版本的动作解析器的动作类
 * - 能够记录动作的类型、值、持续时间、是否被跳过
 * - 新版本使用矩阵式对象，能够正常使用后，可能 删除该旧版本
 */
class Act {
	private let typ: ActType		/* 该动作的类型 */
	private let val: Option<Float32>	/* 动作值 */
	private let duration: Float32		/* 该动作持续的时间 */
	private let skip: Bool			/* 是否跳过 */
	public init(Type: ActType, value!: Option<Float32>, duration!: Float32, skip!: Bool = false)  {
		this.typ = Type
		this.val = value
		this.duration = duration
		this.skip = skip
	}
	public func getVal() { this.val }
	public func getType() { this.typ }
	public func getDelay() { this.duration }
	public func getSkip() { this.skip}
}


/* -------- 动作解析器类 - 只解析 --------
 * INFO(20260517-qing): 这是旧版本的解析器类。
 *
 * 兼容上一代Actor类的脚本格式  
 * TODO: 增加缓慢到达（该方式在drl微调时可能会很有用）
 * TODO: 脚本引入（使用@符号）
 * TODO: 状态变量
 * TODO: 在check()中检查脚本列数是否对应
 * TODO: 解析Pi为单位的角度
 * TODO: 空的特殊行
 *
 * qing(20260429):
 * TODO: 硬件模式标签
 * 按照构想， 它（这种脚本）应该能够控制多种硬件，包含“乐谱”
 * 乐谱脚本将会包含多种和弦设置，我还想加入更多功能  
 *
 * qing(20260430):
 * TODO: 实时交互式解析(done)，在启动时选择模型
 * TODO: 列名声明(done)
 * TODO: 增加跳过项（需要将）
 * TODO: 增加复合项（或者通过动作累加完成）
 * TODO: 增加设置项
 * TODO: 增加变量设置
 *
 * qing(20260502)
 * TODO: 增加变量表，创建硬件变量以及模型
 * TODO*: 增加从字符串到数值的延迟解析
 * TODO*: 扩展为矩阵编程语言
 *
 * INFO(20260515-qing):
 * - 带变量表的矩阵式脚本解析器正在开发，届时我们将这里的一些想法
 * - 移动到新的解析器特性，届时该旧版本可以被删除
 */
class ActsParser {

	private var input: StringReader<InputStream>
	public init(input: StringReader<InputStream>) {
		this.input = input  }

	/* -------- 切换目标 -------- */
	public func target(input: StringReader<InputStream>) {
		this.input = input  }

	/* -------- 解析器 - 循环不变式 --------
	 * 初始化： ss是一个动作脚本的输入流
	 * 保持： ss是动作脚本剩余部分的输入流
	 * 终止： 当输入流为空时，循环终止  */
	public func parse(): Option<HashMap<String, Act>> {
		var count = 1i64 	/*  从第一行开始  */
		var so = input.read()
		while (match(so) {
			case Some(s) => true
			case None => false
		}) {
			try {
				let s = so.getOrThrow()
				print(s)
				let l = act.size
				count = next(s, count)
				let n = act.size
				if (n == l + 1) {
					return this[l] }
				so  = input.read()
			}
			catch (exp: BufOverTheLine) { throw exp }
			//catch (exp: IllegalArgumentException) { throw IllegalArgumentException("Parse failed at line ${count}") } 
			//catch (exp: Exception) { throw UnknowExcept(count) } /* FIXME: 还是暴露出原来的错误位置来更好？ */
		}

		return Option<HashMap<String, Act>>.None
	}
	private var nl: Bool = true	/* new line */
	private var sl: Bool = false	/* special */
	private var sl_n: Bool = false 	/* special (name) */
	private var cm: Bool = false	/* comment */
	private var tmp = ArrayList<Rune>()	/* rune buf */
	private var grp = ArrayList<Float32>()	/* one line */
	private var nam = ArrayList<String>()	/* 列命名 */
	private var bot = ArrayList<ArrayList<Float32>>()	/* bot settings */
	private var act = ArrayList<ArrayList<Float32>>()	/* acts list */
	private static const TMP_LEN = 16	/* size of tmp */
	private func next(c: Rune, count: Int64): Int64 {
		var cc: Int64  = count
		/* ------------ COMMENTS ------------ */
		if (!cm && c == r'#') { cm = true }	/* 注释开始时 */
		else if (cm && c == r'\n') {		/* 注释结束时 */
			nl = true
			sl = false
			cm = false
			cc += 1
		}
		else if (cm && c != r'\n') { ; }	/* 注释进行时
							 * FIXME: EOF情况  */
		//------------ new line ------------
		else if (nl && (c == r' ' || c == r'\t' || c == r'\r')) { ; }	/*  新行空字符，维持现状 */
		else if (nl && c == r'\n') {	/* 新行空行 */
			cc += 1
		}
		else if (nl && c == r'!') {		/* 新行特殊行 */
			nl = false
			sl = true
		}
		else if (nl && c == r'~') {		/* 新行特殊行2留空  */
			nl = false
			sl_n =true
		}
		else if (nl && c != r'~' && c != r'!') {	/* 新行且不为特殊行、制表符和空格 */
			nl = false
			sl = false
			tmp.add(c)			/* to buf */
			if (tmp.size > TMP_LEN) {	/* to long */
				throw BufOverTheLine(count)  }
		}
		
		/* ------------- not nl ------------- */
		else if (!nl && sl_n && !sl && (c == r' ' || c == r'\t' || c == r"\n")) {	/* 添加命名项 */
			if (!tmp.isEmpty()) {
				let n = String(tmp)
				nam.add(n)
				tmp.clear()  }
		}
		else if (!nl && !sl_n && sl && (c == r' ' || c == r'\t' || c == r'\n')) {	/* 添加特殊项 */
			if (!tmp.isEmpty() && String(tmp) == "-") {
				grp.add(0.0f32)		/* 当前行略过（0） */
				tmp.clear()  }

			else if (!tmp.isEmpty()) {
				let f = fromTmp()	/* 结算当前列 */
				grp.add(f)  }
		}
		else if (!nl && !sl_n && !sl && (c == r" " || c == r'\t' || c == r'\n')) {	/* 添加动作项 */

			if (!tmp.isEmpty() && String(tmp) == "-" && act.size == 0) {	/* 首行动作被跳过 */
				grp.add(0.0f32)
				tmp.clear()  }

			else if (!tmp.isEmpty() && String(tmp) == "-" && act.size > 0) {	/* 非首行 */
				let f = act[act.size-1][grp.size]
				grp.add(f)
				tmp.clear()  }

			else if (!tmp.isEmpty()) {
				let f = fromTmp()	/* 结算当前列 */
				grp.add(f)  }

		}
		else if (!nl && sl_n && !sl && c == r';') {	/* 命名行结束 */
			if (!tmp.isEmpty()) {
				let n =String(tmp)
				nam.add(n)
				tmp.clear()  }
			nl = true
			sl_n = false
		}
		else if (!nl && !sl_n && sl && c == r';') { /* 特殊行结束 */
			if (!tmp.isEmpty() && String(tmp) == "-") {
				grp.add(0.0f32)		/* 当前行略过（0） */
				tmp.clear()  }

			else if (!tmp.isEmpty()) {
				let f = fromTmp()	/* 结算当前列 */
				grp.add(f)  }

			if (!grp.isEmpty()) {
				bot.add(grp.clone())
				grp.clear()  }

			nl = true
			sl = false
		}
		else if (!nl && !sl_n && !sl && c == r';') {	/* 普通行结束 */

			if (!tmp.isEmpty() && String(tmp) == "-" && act.size == 0) {  /* 首行动作被跳过 */
				grp.add(0.0f32)
				tmp.clear()  } 

			else if (!tmp.isEmpty() && String(tmp) == "-" && act.size > 0) {	/* 非首行 */
				let f = act[act.size-1][grp.size]
				grp.add(f)
				tmp.clear() }

			else if (!tmp.isEmpty()) {
				let f = fromTmp()	/* 结算当前列 */
				grp.add(f)  }

			if (!grp.isEmpty()) {		/* 普通行结算 */
				act.add(grp.clone())
				grp.clear()  }

			nl = true

		}

		else if (!nl) {	/* 非空格、非制表符、非分号
					FIXME: 且为数字  */
			tmp.add(c)			/*  to buf  */
			if (tmp.size > TMP_LEN) {	/*  to long */
				throw BufOverTheLine(count)  }
		}

		/*  返回当前行号  */
		return cc
	}

	/* -------- 从临时字符缓冲区解析出数值并清理 -------- */
	private func fromTmp(): Float32 {
		let str = String(tmp)
		let f = Float32.parse(str)
		tmp.clear()
		return f  }

	/* ---------------- 序列化 ---------------- 
	 * TODO(qing-20260502): 我需要先解析对其动作，然后再序列化吗？  */
	public func serialize(): String {

		/* -------- 字符串构造器 -------- */
		let sb = StringBuilder()

		/* -------- 序列化命名行 -------- */
		if (!nam.isEmpty()) {
			sb.append("~")
			for (i in 0..nam.size){
				sb.append(nam[i])
				let c = if (i == nam.size-1) {
					 "\n"  }
				else { "\t\t" }
				sb.append(c)  }
		}

		/* -------- 序列化特殊行 -------- */
		for (line in bot) {
			sb.append("!")
			for (i in 0..line.size) {
				sb.append(line[i])
				let c = if (i == line.size-1) {
					 "\n"  }
				else { "\t" }
				sb.append(c)  }
		}

		/* -------- 序列化普通行 -------- */
		for (line in act) {
			for (i in 0..line.size){
				sb.append(line[i])
				let c = if (i == line.size-1) {
					"\n"
				} else { "\t" }	
				sb.append(c)
			}
		}

		/* 取字符串 */
		return sb.toString()
	}

	/* -------- 取动作的长度 -------- */
	public mut prop size: Int64 { /* 类属性访问器 */
		get() { act.size }
		set(v) {}  }

	/* ---- 获取一行原生的动作脚本 ---- */
	public func which(idx: Int64) {
		return act[idx] }

	/*  -------- 取一行动作脚本 --------
	 *  qing(20260430) TODO: 返回Map<String, Act> 对应名字  */
	public operator func [](idx: Int64) {
		let a = act[idx]
		let map = HashMap<String, Act>()
		for (j in 0..a.size) {
			let nm = nam[j] // name
			let typ = ActType.parse(nm)
			let act = Act(typ,	/* 生成动作 */
				value: _sync(j, a[j]), duration: 0.0f32,
				skip: false)
			map.add(nm, act) }

		return map
	}

	/* -------- 根据配置对齐原始数据 -------- */
	private func _sync(j: Int64, target: Float32): Float32 {

		/* 抽象成方法 */
		let ma = abs(bot[0][j])
		let mi = abs(bot[1][j])
		let rf = bot[0][j] < 0.0f32
		let dx = if (bot.size >= 3) {
			bot[2][j]
		} else { 0.0f32 }

		var t = target
		t = if (ma < t) {ma} else {t}
		t = if (mi > t) {mi} else {t}
		t = if (!rf) {t} else {180.0f32 - t}
		t = t + dx  /* plus offset */

		return t
	}
}

/* --------------------------------------------------------- */
/* --------------------------------------------------------- */


/* -------- 矩阵编程语言 - 基本对象 --------
 * TODO：储存功能
 * TODO：神经网络层转换为矩阵对象 */
class MatrixObject {
	private var name: String = ""		/* 名字 */
	private var tmp = ArrayList<Rune>()	/* rune buf 字符缓冲区 */
	private var grp = ArrayList<String>()	/* one line 整行缓冲区 */
	private var values = ArrayList<ArrayList<String>>()	/* 原始矩阵 */

	/* -------- 已命名 -------- */
	public func hasName(): Bool { !(name == "") }

	/* --------- 获得名字 --------- */
	public func getName(): String { return name }

	/* ------- 取动作的长度 ------- */
	public mut prop size: Int64 {
		get() { values.size }
		set(_) {}
	}

	/* ------- 获取矩阵元素，只能取，不能修改 ------- */
	public operator func [](row: Int64, col: Int64): String {
		return values[row][col]
	}

	/** -- 在某一行中寻找某一个字符串的列号 -- */
	public func indexOfCol(row: Int64, key: String): Int64 {
		for (col in 0..values[row].size) {
			if (values[row][col] == key) {
				return col  }
		}

		/* ---- 没找到 ---- */
		throw IllegalArgumentException("The key does not found.")
	}

	/*  -----  储存对象 ----- */
	public func save(output: StringWriter<OutputStream>) {
		output.writeln("@${getName()}")
		for (vec in values) {
			for (item in vec) {
				output.write(item)
				output.write(" ")
			}
			output.writeln(";")
		}
		output.writeln("@")
	}

	/* -------- 添加字符 -------- */
	public func addRune(c: Rune) {  tmp.add(c)  }

	/* -------- 提交名字 -------- */
	public func submitName() {
		if (tmp.size == 0) { return }
		let s = String(tmp)
		tmp.clear()
		name = s
	}

	/* -------- 提交元素 -------- */
	public func submitItem() {
		if (tmp.size == 0) { return }
		let s = String(tmp)
		tmp = ArrayList<Rune>()
		grp.add(s)
	}

	/* -------- 提交行 -------- */
	public func submitLine(count: Int64) {
		if (grp.size == 0) { return }
		if (!values.size == 0 && !grp.size == values[values.size - 1].size)
			{ throw DoesNotSync(count) }
		values.add(grp)
		grp = ArrayList<String>()
	}
}

/*  -------- 矩阵式解析器 --------
 *  TODO(20260514-qing): 这个类输入流数据，输出MatrixObject对象
 *  它是与其深度绑定的，我倾向于将其与MatrixObject融合
 *  即持有HashMap<String, ArrayList<ArrayList<String>>>  */
class MatrixParser {

	/* ---- 静态判断Option<Rune>是否有值 ----
	   - 该方法用于文本文件遍历时判空  */
	private static func hasValue(co: Option<Rune>): Bool {
		match(co) {
			case Some(_) => true
			case None => false
		}
	}

	/* -------- 主初始化器（构造函数） --------
	   - 设置目标输入流  */
	private var input: StringReader<InputStream>
	public init(input: StringReader<InputStream>) {
		this.input = input  }

	/* -------- 切换目标输入流 -------- */
	public func reset(input: StringReader<InputStream>) {
		this.input = input  }

	/* -------- 解析器 - 循环不变式 ----------------------
	 * -------- 初始化： ss是一个动作脚本的输入流 --------
	 * -------- 保持： ss是动作脚本剩余部分的输入流 ------
	 * -------- 终止： 当输入流为空时，循环终止 ----------  */
	private var obj = MatrixObject() 
	public func parse(): Option<MatrixObject> {
		var count = 1i64		/* 行计数器始于1 */
		flag = false			/* 重置结束标志 */
		this.obj = MatrixObject()	/* 创建一个新的对象，一次只解析一个对象 */
		var so = input.read()		/* 获取初始状态 */
		while (!flag && hasValue(so)) {
			try {
				let s = so.getOrThrow()
				print(s)		/* debug */
				count = _next(s, count)	/* 进行解析解析 */
				so  = input.read()	/* 为下一次循环提供条件 */
			}
			catch (exp: BufOverTheLine) { throw exp }
			catch (exp: IllegalArgumentException) { throw IllegalArgumentException("Parse failed at line ${count}") } 
			catch (exp: Exception) { throw UnknowExcept(count) }	/* FIXME: 还是暴露出原来的错误位置来更好？ */
		}

		/* 如果没有新的矩阵式对象（意外终止）则返回空 */
		if (!obj.hasName()) {  return None  }
		return  obj	/* 否则为解析成功正常退出，返回解析到的矩阵式对象 */
	}

	private var flag = false	/* 结束标志 */
	private var nl: Bool = true	/* new line */
	private var cm: Bool = false	/* comment */
	private var nm: Bool = false	/* name-line */
	private func _next(c: Rune, ct: Int64): Int64 {
		var count = ct
		/* ------------ COMMENTS ------------ */
		if (!cm && c == r'#') { cm = true }	/* 注释开始时 */
		else if (cm && c == r'\n') {		/* 注释结束时 */
			nl = true
			cm = false
			count += 1
		}
		else if (cm && c != r'\n') { ; }	/* 注释进行时 */ 
							/* FIXME: EOF情况 */
		/* ------------ new line ------------ */
		else if (nl && (c == r' ' || c == r'\t' || c == r'\r')) { ; }	/* 新行空字符，维持现状 */
		else if (nl && c == r'\n') {	/* 新行空行 */
			count += 1
		}
		else if (nl && c == r'@' && !obj.hasName()) {
			nl = false
			nm = true
		}
		else if (nl && c == r'@' && obj.hasName()) {
			flag =true
			nl = false
			nm = true
		}
		else if (nl && c != r'@') {	/* 新行内容 */
			nl = false
			obj.addRune(c)
		}
		/* ------------- not nl ------------- */
		else if (!nl &&  nm && (c == r' ' || c == r'\t' || c == r'\n')) {	/* 添加名字 */
			obj.submitName()
			nl = true
			nm = false
		}
		else if (!nl && !nm && (c == r' ' || c == r'\t' || c == r"\n")) {	/* 添加命名项 */
			obj.submitItem()
			if (c ==  r"\n") { count += 1 }
		}
		else if (!nl && c == r';') {	/* 普通行结束 */
			obj.submitItem()
			obj.submitLine(count)
			nl = true
		} 
		else if (!nl) {	// 非空格、非制表符、非分号
			obj.addRune(c)
		}

		/* -------- 返回行号 -------- */
		return count
	}

}

/* -------- 矩阵式储存器 -------- 
 * - 接收矩阵式的哈希表（HashMap<String, MatrixObject>）
 * - 以及一个输出流（在构造函数）。 */
class MatrixSaver {
	private let stringWriter: StringWriter<OutputStream>
	public init(output: OutputStream) {
		stringWriter = StringWriter(output)  }
	public func save(C: LinkedList<MatrixObject>) {
		for (item in C) {
			item.save(stringWriter)
			stringWriter.writeln()	/* 对象之间空一行 */
			stringWriter.flush()  }
	}
}


/* --------------------------------------------------------- */
/* --------------------------------------------------------- */
/* TODO(20260516-qing): 继续完善异常种类 */

/* -------- 解析异常 -------- */
open class ParseExcept <: Exception {
	public ParseExcept(msg: String, line: Int64) {
		super('${msg}. we got line: ${line}')  }
}

/* -------- 缓冲区过长 -------- */
class BufOverTheLine <: ParseExcept {
	BufOverTheLine(count: Int64) {
		super('Buffer length over the line', count)  }
}

/* -------- 行没对齐 -------- */
class DoesNotSync <: ParseExcept {
	DoesNotSync(count: Int64) {
		super("Matrix Line Does Not Sync", count)  }
}

/* -------- 未知异常 -------- */
class UnknowExcept <: ParseExcept {
	UnknowExcept(count: Int64) {
		super("I don't understand what happened", count)  }
}



