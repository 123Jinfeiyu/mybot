package bot4cj
import std.io.*
import std.math.*
import std.convert.*
import std.collection.*

/* 单条动作的类型枚举 */
enum ActType {
	SERVO | SPEAK
}

/* 单条动作类 */
class Act {
	let typ: ActType // 该动作的类型
	let val: Option<Float32> // 动作值
	let duration: Float32 // 该动作持续的时间
	let skip: Bool
	init(Type: ActType, value!: Float32, duration!: Float32, skip!: Bool = false) {
		this.typ = Type
		this.val = value
		this.duration = duration
		this.skip = skip
	}
}


/* 动作解析器类 - 只解析
 * 兼容上一代Actor类的脚本格式  
 * TODO: 增加缓慢到达（该方式在drl微调时可能会很有用）
 * TODO: 脚本引入
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
 * TODO: 实时交互式解析，在启动时选择模型
 * TODO: 列名声明(done)
 * TODO: 增加跳过项
 */
class ActsParser {

	// 直接通过输入流构造
	init(inputStream: InputStream) { _parse(inputStream) }

	/* 解析器 - 循环不变式
	 * 初始化： ss是一个动作脚本的输入流
	 * 保持： ss是动作脚本剩余部分的输入流
	 * 终止： 当输入流为空时，循环终止  */
	func _parse(inputStream: InputStream) {
		let ss = StringReader(inputStream)
		var count = 1i64 	// 从第一行开始
		while (true) {
			if(let Some(s) <- ss.read()) {
				try {
					//print(s)
					count = next(s, count)
				}
				catch (exp: BufOverTheLine) { throw exp }
				//catch (exp: IllegalArgumentException) { throw UnknowExcept(count) } // FIXME: 多半是解析问题？还是暴露出来更好
			}
			else { break} 
		}
		//check()
		sync()
	}
	var nl: Bool = true	// new line
	var sl: Bool = false	// special
	var sl_n: Bool = false 	// special (name)
	var cm: Bool = false	// comment
	var tmp = ArrayList<Rune>()	// rune buf
	var grp = ArrayList<Float32>()	// one line
	var nam = ArrayList<String>() // 列命名
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

	var rotates = ArrayList<Bool>()
	func sync() {	// 对齐
		let sl = bot[0]
		rotates.clear()
		for (col in sl) {
			let flag = col < 0.0f32
			rotates.add(flag)
		}
	}

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
	operator func [](idx: Int64) {
		let a = act[idx]
		let bs = ArrayList<Float32>()
		for (j in 0..a.size-1) {
			let ma = abs(bot[0][j])
			let mi = abs(bot[1][j])
			let rf = rotates[j]
			let dx: Option<Float32> = if (bot.size >= 3) {bot[2][j]} else {None}
			var t = a[j]
			t = if (ma < t) {ma} else {t}
			t = if (mi > t) {mi} else {t}
			t = if (!rf) {t} else {180.0f32 - t}
			t = match(dx) {
				case Some(D) => t + D  // 漂移
				case None     => t
			}
			bs.add(t)
		}
		bs.add(a.last.getOrThrow())
		bs
	}
}

/* 旧的机器人类 现在用于配合专门的解析器
 * TODO: 增加缓慢到达方法
 * TODO: 增加机器人构造偏差 
 * TODO: 机器人构造脚本
 * TODO: 增加脚本起始状态限制、最终状态记录
 * TODO: 增加动态调节变量
 * TODO：增加check()方法，检查脚本列数是否对应
 * TODO：解析pi为单位的角度
 * FIXME: print()以原貌打印
 * FIXME: parse()兼容空的特殊行
 * TODO*: 解析器与舵机设备解耦
 * TODO: ”编译“动作脚本 */
class Actor {

	public init(filepath: String) {
		i2c = I2C(filepath)
	}
    
	/*
 	 * 解析器 - 循环不变式
 	 *
 	 *
 	 * 初始化：ss是一个动作脚本的输入流
 	 *
 	 * 保持：ss是动作脚本剩余部分的输入流
 	 *
 	 * 终止：当输入流为空时，循环终止
 	 */
	public func parse(inputStream: InputStream) {
		let ss = StringReader(inputStream)
		var count: Int64 = 0;
		while (true) {	/* FIXME(花间岛): while(let Some(f) <- ss.read()) */
			try {	/*      (qing): 链式调用可读性反而降低 */
				let a = ss.read()
				let aa = match(a) {
					case Some(v) => v
					case None => break
				}
				count = this.next(aa, count)
			}
			catch (exp: BufOverTheLine) { /* 临时占位 */
				throw exp
			}
			catch (exp: IllegalArgumentException) {
				throw UnknowExcept(count)
			}
		}

		//check()
	}

	/* 打印整个动作脚本
	 * FIXME: 原样打印出来 */
	public func print() {
		for (array in bot) {
			for (item in array) {
 				print(item)
				print(" ")
			}
			print("\n")
		}
	}

	/* 开始演绎 --- 按照解析出来的顺序进行旋转
	 * FIXME: 考虑max和min符号不同的情况
	 * TODO: 加入DRL微调  */
	public func play() {
		let max = bot[0] /* 第一行是上限 */
		let min = bot[1] /* 第二行是下限 */
		let i2 = i2c.getOrThrow()
		let pca9685 = PCA9685(i2)
		//let mp = MPU6050(i2)
		//let data = mp.readSensor()
		//data.print()
		for (i in 2..bot.size) {	/* 从第2行开始遍历 */
			let a = bot[i]		/* 选中一行 */
			for (j in 0..a.size-1) {	/* 遍历关节（最后一列是时延） */
				let ma = abs(max[j])	/* 取得该关节最大值 */
				let mi = abs(min[j])	/* 取得该关节最小值 */
				let rf = max[j] < 0.0f32	/* rotate flag 逆转标志 */
				var t = a[j]			/* 选中单个 */
				t = if (ma < t) { ma } else { t }	/* 对上限进行切片 */
				t = if (mi < t) { t }  else { mi }	/* 对下限进行切片 */
				t = if (rf) { 180.0f32 - t } else { t }	/* 是否逆转 */
				let jj = UInt8(j) /* FIXME: 应该从一开始就用UInt8进行存储 */
				pca9685.setAngle(jj, Float32(t))
			}
			let aa = a.last.getOrThrow()
			sleep(Duration.millisecond * Int64(aa))
		}
	}

	/* 单行演绎 */
	public func play(t: ArrayList<Float32>) {
		let i2 = i2c.getOrThrow()
		let pca9685 = PCA9685(i2)
		for (jj in 0..t.size-1) {
			pca9685.setAngle(UInt8(jj), Float32(t[jj]))
		}
		let tt = t.last.getOrThrow()
		sleep(Duration.millisecond * Int64(tt)) /* FIXME: 小数不准*/
	}


	/*-----------------*/
	private var i2c: Option<I2C> = None
	private var nl: Bool = true   /* new line */
	private var sl: Bool = false  /* special */
	private var cm: Bool = false  /* comment */
	private var tmp = ArrayList<Rune>()    /* 文字缓冲区 */
	private var grp = ArrayList<Float32>() /* 单行 */
	private var bot = ArrayList<ArrayList<Float32>>()
	private static const TMP_LEN = 16      /* 文字缓冲区不超过 */
	private func next(c: Rune, count: Int64): Int64 {
		var cc: Int64 = count
		/* ---- comment ---- */

		/* 注释行开始标志 */
		if (!cm && c == r'#') {
			cm = true
		}

		/* 注释行结束标识 */
		else if (cm && c == r'\n') {
			nl = true
			sl = false
			cm = false
			cc += 1
		}

		/* 注释内容
		 * FIXME: EOF */
		else if (cm && c != r'\n') {
			;
		}
		/* ---------------- new line ---------------- */

		/* 新行且为空字符 */
		else if (nl && (c == r' ' || c == r'\t')) { 
			;
		}

		/* 新行且为空行 */
		else if (nl && c == r'\n') {
			cc += 1
		}

		/* 新行且为特殊行 */
		else if (nl && c == r'!') { 
			nl = false
			sl = true
		}

		/* 新行且不为特殊行、制表符和空格：录入 */
		else if (nl && c != r'!') {
			nl = false
			sl = false
			tmp.add(c) /* 到缓冲区 */
			if (tmp.size > TMP_LEN) { /* 缓冲区过长 */
				throw BufOverTheLine(count)
			}
		}

		/* ---------------- 非首字 ---------------- */
		/* 空格与制表符：两个列之间的分隔字符 */
		else if (!nl && (c == r' ' || c == r'\t')) {
			if (!tmp.isEmpty()) { /* 结算当前 */
				let f = fromTmp()
				grp.add(f)
			}
		}

		/* 分号：一行的结束标志 */
		else if (!nl && c == r';') {
			if (!tmp.isEmpty()) { /* 结算当前 */
				let f = fromTmp()
				grp.add(f)
			}
			if (!grp.isEmpty()) { /* 结算一行 */
				bot.add(grp.clone())
				grp.clear()
			}
			nl = true
			sl = false
			cm = false
		}

		/* 非空格、非制表符、非分号 （FIXME: 且是数字） */
		else if (!nl) {
			tmp.add(c) /* 到缓冲区 */
			if (tmp.size > TMP_LEN) { /* 缓冲区过长 */
				throw BufOverTheLine(count)
			}
 		}

		cc
	}

	/* 从文字缓冲区解析、清理并输出数值 */
	private func fromTmp(): Float32 {
		let str = String(tmp)
		let f = Float32.parse(str)
		tmp.clear()
		return f
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
