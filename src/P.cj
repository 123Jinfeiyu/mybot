package bot4cj
import std.io.*
import std.math.*
import std.convert.*
import std.collection.*

/* 单条动作的类型枚举 */
enum ActType {

	SERVO | SPEAK | NOPE | UNKNOW

	// 以固定方式解析命名含义
	static func parse(nm: String): ActType {
		if (nm.startsWith("s")) {
			ActType.SERVO
		} else if (nm == "spk") {
			ActType.SPEAK
		} else if (nm == "-") {
			ActType.NOPE
		} else {
			ActType.UNKNOW
		}
	}
}


/* 单条动作类 */
class Act {
	let typ: ActType // 该动作的类型
	let val: Option<Float32> // 动作值
	let duration: Float32 // 该动作持续的时间
	let skip: Bool  // 是否跳过
	init(Type: ActType, value!: Option<Float32>, duration!: Float32, skip!: Bool = false) {
		this.typ = Type
		this.val = value
		this.duration = duration
		this.skip = skip
	}
	func getVal() { this.val }
	func getType() { this.typ }
	func getDelay() {this.duration}
	func getSkip() {this.skip}
}


/* 动作解析器类 - 只解析
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
 */
class ActsParser {

	var input: StringReader<InputStream>
	init(input: StringReader<InputStream>) {
		this.input = input
	}

	// 切换目标
	func target(input: StringReader<InputStream>) {
		this.input = input
	}

	/* 解析器 - 循环不变式
	 * 初始化： ss是一个动作脚本的输入流
	 * 保持： ss是动作脚本剩余部分的输入流
	 * 终止： 当输入流为空时，循环终止  */
	func parse(): Option<HashMap<String, Act>> {
		var count = 1i64 	// 从第一行开始
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
					return this[l]
				}
				so  = input.read()
			}
			catch (exp: BufOverTheLine) { throw exp }
			catch (exp: IllegalArgumentException) { throw IllegalArgumentException("Parse failed at line ${count}") } 
			catch (exp: Exception) { throw UnknowExcept(count) } // FIXME: 还是暴露出来更好
		}

		return Option<HashMap<String, Act>>.None
	}
	var nl: Bool = true	// new line
	var sl: Bool = false	// special
	var sl_n: Bool = false 	// special (name)
	var cm: Bool = false	// comment
	var tmp = ArrayList<Rune>()	// rune buf
	var grp = ArrayList<Float32>()	// one line
	var nam = ArrayList<String>()	// 列命名
	var bot = ArrayList<ArrayList<Float32>>()	// bot settings
	var act = ArrayList<ArrayList<Float32>>()	// acts list
	static const TMP_LEN = 16	// size of tmp
	func next(c: Rune, count: Int64): Int64 {
		var cc: Int64  = count
		//println("nl: ${nl}\tsl: ${sl}\tsl_n: ${sl_n}\tcm: ${cm}\tval: \'${c}\'")
		//------------ COMMENTS ------------
		if (!cm && c == r'#') { cm = true }	// 注释开始时
		else if (cm && c == r'\n') {		// 注释结束时
			nl = true
			sl = false
			cm = false
			cc += 1
		}
		else if (cm && c != r'\n') { ; }	// 注释进行时 FIXME: EOF情况
		
		//------------ new line ------------
		else if (nl && (c == r' ' || c == r'\t' || c == r'\r')) { ; }	// 新行空字符，维持现状
		else if (nl && c == r'\n') {	// 新行空行
			cc += 1
		}
		else if (nl && c == r'!') {		// 新行特殊行
			nl = false
			sl = true
		}
		else if (nl && c == r'~') {		// 新行特殊行2留空
			nl = false
			sl_n =true
		}
		else if (nl && c != r'~' && c != r'!') {	// 新行且不为特殊行、制表符和空格
			nl = false
			sl = false
			tmp.add(c)			// to buf
			if (tmp.size > TMP_LEN) {	// to long
				throw BufOverTheLine(count)
			}
		}
		
		//------------- not nl -------------
		else if (!nl && sl_n && (c == r' ' || c == r'\t' || c == r"\n")) {	// 添加命名项
			if (!tmp.isEmpty()) {
				let n = String(tmp)
				nam.add(n)
				tmp.clear()
			}
		}
		else if (!nl && !sl_n && (c == r' ' || c == r'\t' || c == r'\n')) {	// 空格与制表符为两列之间分隔字符
			if (!tmp.isEmpty()) {
				let f = fromTmp()	// 结算当前列
				grp.add(f)
			}
		}
		else if (!nl && sl_n && c == r';') {	// 命名行结束
			if (!tmp.isEmpty()) {
				let n =String(tmp)
				nam.add(n)
				tmp.clear()
			}
			nl = true
			sl_n = false
		}
		else if (!nl && !sl_n && c == r';') {	// 分号是一行的结束标记
			if (!tmp.isEmpty()) {
				let f = fromTmp()	// 结算当前列
				grp.add(f)
			}
			if (!grp.isEmpty() && sl) {	// 特殊行结算
				bot.add(grp.clone())
				grp.clear()
			}
			if (!grp.isEmpty() && !sl) {	// 普通行结算
				act.add(grp.clone())
				grp.clear()
			}
			nl = true
			sl = false
		}

		else if (!nl) {	// 非空格、非制表符、非分号 FIXME: 且为数字
			tmp.add(c)			// to buf
			if (tmp.size > TMP_LEN) {	// to long
				throw BufOverTheLine(count)
			}
		}

		//返回当前行号
		return cc
	}

	// 从临时字符缓冲区解析出数值并清理
	func fromTmp(): Float32 {
		let str = String(tmp)
		let f = Float32.parse(str)
		tmp.clear()
		return f
	}

	// 序列化
	func serialize(): String {

		// 字符串构造器
		let sb = StringBuilder()

		// 序列化命名行
		if (!nam.isEmpty()) {
			sb.append("~")
			for (i in 0..nam.size){
				sb.append(nam[i])
				let c = if (i == nam.size-1) {
					 "\n"
				} else { "\t\t" }
				sb.append(c)
			}
		}

		// 序列化特殊行
		for (line in bot) {
			sb.append("!")
			for (i in 0..line.size) {
				sb.append(line[i])
				let c = if (i == line.size-1) {
					 "\n"
				} else { "\t" }
				sb.append(c)
			}
		}

		// 序列化普通行
		for (line in act) {
			for (i in 0..line.size){
				sb.append(line[i])
				let c = if (i == line.size-1) {
					"\n"
				} else { "\t" }	
				sb.append(c)
			}
		}

		// 取字符串
		return sb.toString()
	}

	//var rotates = ArrayList<Bool>()
	//func sync() {	// 对齐
	//	let sl = bot[0]
	//	rotates.clear()
	//	for (col in sl) {
	//		let flag = col < 0.0f32
	//		rotates.add(flag)
	//	}
	//}

	// 取动作的长度
	public mut prop size: Int64 {
		get() { act.size }
		set(v) {}
	}

	// 获取一行原生的动作脚本
	public func which(idx: Int64) {
		return act[idx]
	}

	// 取一行动作脚本
	// qing(20260430) TODO: 返回Map<String, Act> 对应名字
	operator func [](idx: Int64) {
		let a = act[idx]
		let map = HashMap<String, Act>()
		for (j in 0..a.size-1) {
			let nm = nam[j] // name
			let typ = ActType.parse(nm)
			let act = Act(typ,	// 生成动作
				value: _sync(j, a[j]), duration: 0.0f32,
				skip: false)
			map.add(nm, act)
		}
		let act_n = Act(ActType.NOPE, value: None,
			duration: a.last.getOrThrow(),
			skip: false)
		map.add("delay", act_n)
		return map
	}

	// 根据配置对齐原始数据
	private func _sync(j: Int64, target: Float32): Float32 {

		// 抽象成方法
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
		t = t + dx  // plus offset

		return t
	}
}


/* 解析异常 */
open private class ParseExcept <: Exception {
	public ParseExcept(msg: String, length: Int64) {
		super('${msg}. we got length: ${length}')
	}
}

/* 缓冲区过长 */
private class BufOverTheLine <: ParseExcept {
	public BufOverTheLine(count: Int64) {
		super('Buffer length over the line', count)
	}
}

/* 未知异常 */
private class UnknowExcept <: ParseExcept {
	public UnknowExcept(count: Int64) {
		super("I don't understand what happened", count)
	}
}
