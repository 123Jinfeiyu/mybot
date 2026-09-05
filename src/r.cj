/*
 *  机器人构成模块
 *  INFO(20260514-qing): 该模块确定机器人结构，为解耦考虑应该从解析出的矩阵对象中构建
 *  TODO(20260527-qing): 使脚本中的数值亦可从运行时确定
 */

package bot4cj
import std.io.*
import std.collection.*
import std.process.*
import stdx.encoding.json.*
import mt2.*
import SQL4cj.*
import bot4cj.HV.*


/* 机器人结构类
 * 
 * 机器人结构是动态的，从脚本中解析的 
 * TODO(20260514-qing): 调整为输入MatrixObject
 * TODO(20260515-qing): 需要重写一遍硬件类
 * TODO(20260518-qing): 使用神经网络RNN直接根据动作+传感器脚本生成下一步动作
 * TODO(20260527-qing): 增加硬件模块行为脚本
 * TODO*(20260527-qing): 为@Action表演矩阵对象增加位置指示器，以增量行动
 *
 * INFO(20260826-qing): 机器人结构类对象，接受输入流，随后逐个解析存入链表
 */
class RobotStruct {

	/* -------- 存放解析结果和创建的变量 -------- */
	private let C = LinkedList<MatrixObject>()
	/* -------- 守护 holder 注册表：deactive 先统一置 stop 再 join（防 worker 饿死死锁）-------- */
	private let daemonHolders = ArrayList<DaemonHolder>()
	private let M = HashMap<String, MatrixVars_Hardware>()
	/* 硬件变量声明序序列：active/deactive 按声明序 build/destroy
	 * （HashMap 遍历序未定义，而 I2C→PCA9685 等 fd 依赖要求声明序） */
	private let hwSeq = ArrayList<MatrixVars_Hardware>() 

	/* -------- 安全添加硬件变量（检查重复键） -------- */
	private func addHardwareVar(name: String, hw: MatrixVars_Hardware) {
		if (let Some(_) <- M.get(name)) {
			throw IllegalArgumentException("Duplicate hardware variable name: '${name}'")
		}
		M[name] = hw
		hwSeq.add(hw)
	}

	/*
	 *  注册一张 @Alias 矩阵中的所有别名变量
	 *
	 *  session: 动作脚本侧注册时传入本次 run 的会话集合——
	 *  同一动作脚本内重复定义抛错；与结构脚本同名则覆盖并打日志。
	 *  None 表示结构脚本侧注册，重名直接抛错（addHardwareVar 原有行为）。
	 */
	private func registerAliasMatrix(matrix: MatrixObject, session: Option<HashSet<String>>) {
		let idxName = matrix.indexOfCol(0, "name")
		for (i in 1..matrix.size) {
			let name = matrix[i, idxName]
			let aliasVar = MtVars_Alias.Factory(matrix, i, M)
			if (let Some(set) <- session) {
				if (set.contains(name)) {
					throw IllegalArgumentException("Duplicate alias name '${name}' within the action script")
				}
				set.add(name)
				if (let Some(_) <- M.get(name)) {
					println("Alias '${name}' defined in action script overrides the structure definition")
				}
				M[name] = aliasVar
			}
			else {
				addHardwareVar(name, aliasVar)
			}
		}
	}

	/*
	 *  使用矩阵脚本驱动机器人
	 * 
	 *  不行就将矩阵脚本转化为bot脚本吧
	 */
	public init(input: InputStream) {
		readSQL(input, C)
		createHardVars()
	}

	/* -------- 激活所有硬件变量 -------- */
	public func  active(): Unit {
		for (item in hwSeq) {
			item.build()
		}
	}

	/*
	 *  消除所有硬件变量
	 * 
	 *  INFO(20260517-qing): 它需要以倒序方式进行清除
	 */
	public func deactive(): Unit {
		/* P1 防死锁：先给【所有】守护置 stop，再统一 join。
		 * 否则逐个 stop+get 时，若先 join 一个被 worker 池饿死的守护，其余守护
		 * （stop 未置位）仍独占 worker → 饿死者永不运行 → get() 永久阻塞。
		 * 全部置 stop 后：运行中的守护退出释放 worker，饿死者随后拿到 worker 即见 stop 退出。 */
		for (h in daemonHolders) {
			unsafe { (h.slot + SLOT_STOP_OFF).write(1u8) }
		}
		let items = hwSeq.toArray()
		for (i in items.size-1..=0:-1) {
			/* 单个 destroy 失败不阻塞后续清理（记录并继续） */
			try { items[i].destroy() }
			catch (e: Exception) { println("Deactive warning: ${e}") }
		}
	}

	/* -------- 执行@Action矩阵中的内容 -------- */
	public func run(input: InputStream): Unit {
		let AC = LinkedList<MatrixObject>()
		readMatrix(input, AC)

		/* ---- 注册动作脚本中内联的 @Alias（同名时覆盖结构脚本的定义） ---- */
		let session = HashSet<String>()
		for (matrix in AC) {
			if (matrix.getName().trimAscii() == "Alias") {
				registerAliasMatrix(matrix, Some(session))
			}
		}

		run_drl_action(AC)
	}

	/*
	 *  储存整个矩阵变量表
	 * 
	 *  NOTE(20260517-qing): 使用完毕要关闭输出流
	 */
	public func save(out: OutputStream): Unit {
		let ms = MatrixSaver(out)
		ms.save(C)
	}

	/*
	 *  阅读矩阵脚本
	 * 
	 *  TODO(20260504-qing): 使用矩阵脚本存储神经网络模型
	 */
	static private func readMatrix(file: InputStream, C: LinkedList<MatrixObject>){
		let sr = StringReader<InputStream>(file)
		let parser =  MatrixParser(sr)
		while (true) {
			if (let Some(O) <- parser.parse()) {
				let n = O.getName();
				println(n)
				C.addLast(O)  }
			else { break }
		}
	}

	static private func readSQL(input: InputStream, C: LinkedList<MatrixObject>) {
		let sr = StringReader<InputStream>(input)
		let tkns = Lexer.parseAll(sr)
		let eves = Parser.parseAll(tkns)
		for (eve in eves) {
			match (eve) {
				case CREATE_TABLE(ev) =>
					var mt = MatrixObject()
					for (ch in ev.tableName) {
						mt.addRune(Rune(ch))
					}
					mt.submitName()
					for (col in ev.tableColumns) {

						match (col._type) {
							case _String => 
								for (ch in col.name) {
									mt.addRune(Rune(ch))
								}
								mt.submitItem()
							case _ => throw Exception("Only String is Valid")
						}

					}
					mt.submitLine(0)
					C.addLast(mt)

				case INSERT_TABLE(ev) =>

					C.forEach({item =>

						if (ev.targetTabName == item.getName()) {

							println("{${ev.targetTabName}}")

							for (row in ev.tableRows) {
								for (i in 0..item.col) {
									for (col in row) {
										if (col.columnName == item[0, i]) {
											for (ch in col.value) {
												item.addRune(Rune(ch))
											}
											item.submitItem()
										}
									}
								}
								item.submitLine(0)
							}
						}
					})

				case _ => throw Exception("Unsupported sql command")
			}
		}
	}

	/*
	 *  创建硬件变量字典
	 * 
	 *  TODO(20260515-qing): 解耦合  
	 *  FIXME(20260518-qing): 在插入哈希表之前如果有已经存在的键值，要释放它的资源
	 */
	private func createHardVars()
	{

		for (matrix in C) {
		let MtName = matrix.getName()

		/* ---- 【特殊的】Alias别名硬件
			 * ---- */
			if (MtName == "Alias") {
				registerAliasMatrix(matrix, None)
			}


			/* ---- [特殊的] Action矩阵
			 * ---- */
			else if (MtName == "Action") {
				;
			}

			/* ---- 【特殊的】Daemon 守护线程硬件 ---- */
			else if (MtName == "Daemon") {
				let idxName = matrix.indexOfCol(0, "name")
				for (i in 1..matrix.size) {
					let name = matrix[i, idxName]
					addHardwareVar(name, MtVars_Daemon.Factory(matrix, i, M, daemonHolders))
				}
			}

			/* ---- 【通用的】Lua脚本硬件 ---- */
			else if (matrix.size > 0) {
				let idxName = matrix.indexOfCol(0, "name")

				/* ---- 手动扫描标题行，寻找可选的 controller 列 ---- */
				var idxCtrl: Int64 = -1
				for (j in 0..matrix.col) {
					if (matrix[0, j] == "controller") { idxCtrl = j; break }
				}

				/* ---- 从环境变量 BOT4CJ_MODULE_PATH 获取模块路径，默认为 ./modules ---- */
				let modulePath = Process.current.getEnv("BOT4CJ_MODULE_PATH") ?? "./modules"
				let scriptPath = "${modulePath}/${MtName}.lua"

				for (i in 1..matrix.size ) {
					let name = matrix[i, idxName]

					/* ---- 解析该行的 controller 引用（支持 - 占位符继承） ---- */
					var ctrlOpt: Option<MatrixVars_Hardware> = None
					if (idxCtrl >= 0) {
						let ctrlName = MatrixVars_Hardware.getMt(matrix, i, idxCtrl)
						if (ctrlName != "-") {
							let ctrlOptHV = M.get(ctrlName)
							if (ctrlOptHV.isNone()) {
								throw IllegalArgumentException(
									"Hardware variable '${name}' declares controller '${ctrlName}', but it is not defined yet. The controller must be defined before the matrix that references it.")
							}
							ctrlOpt = ctrlOptHV.getOrThrow()
						}
					}

					/* ---- 元数据闭包：跳过 controller 列，避免误当作引用 play ---- */
					let skipCol = idxCtrl	/* lambda 不得捕获可变变量 */
					addHardwareVar(name, MtVars_Lua.Factory(scriptPath, {=>
							this._getMetadata(matrix, i, skipCol: skipCol)
					}, controller: ctrlOpt))
				}
		}
		}
	}


	/* -------- 在矩阵对象的容器中寻找目标 -------- */
	private static func findMt(ll: LinkedList<MatrixObject>, name: String): Option<MatrixObject>
	{
		for (item in ll) {
			var target  = item.getName()
			target = target.trimAscii() // ???
			if (name == target) {
				return item  }
		}

		return None
	}

	/* -------- 使用drl微调动作，使用RNN生成动作流 -------- */
	private func run_drl_action(AC: LinkedList<MatrixObject>) {
		let ActionDefsOpt = findMt(AC, "Action")
		if (ActionDefsOpt.isNone()) {
			throw IllegalArgumentException("No @Action matrix found in script. Please define action matrix in your .mt file.")
		}
		let ActionDefs = ActionDefsOpt.getOrThrow()	/* MatrixObject */

		/* 按标题行的列顺序收集设备名，保证执行顺序与脚本列序一致
		 * （例如把 Timer(Speaker) 列写在最后，它就会最后执行，见 issue #12） */
		let order = ArrayList<String>()
		for (j in 0..ActionDefs.col) {
			let name_HV = ActionDefs[0, j]
			let arr = name_HV.split(".")
			if (arr.size != 2) {
				throw IllegalArgumentException("Action column is not valid")  }
			let name_dev = arr[0]
			if (!order.contains(name_dev)) {
				order.add(name_dev)  }
		}

		for (i in 1..ActionDefs.size) {
			let set =JsonObject()		/* 每个硬件变量的参数集合 */
			for (j in 0..ActionDefs.col) {
				parse_action_matrixObject(set, ActionDefs, i, j)
			}

			/* -------- 执行一行 -------- */
			for (name_dev in order) {
				let hvOpt = M.get(name_dev)
				if (hvOpt.isNone()) {
					throw IllegalArgumentException("Hardware variable '${name_dev}' is used in @Action but not defined in hardware declaration section.")
				}
				let hv = hvOpt.getOrThrow()
				let paramsOpt = set.get(name_dev)
				if (paramsOpt.isNone()) {
					throw IllegalArgumentException("Parameter for hardware variable '${name_dev}' is missing in action row ${i}.")
				}
				let params: JsonValue = paramsOpt.getOrThrow()
				let res: JsonValue = hv.play(params)

				/* ---- halt-on-error：硬件 call 返回显式 ERR 即中断剩余动作 ---- */
				MatrixVars_Hardware.ensureOk(res, ctx: "play ${name_dev} (row ${i})")
			}


		}
	}

	/* -------- 解析@Action矩阵对象 -------- */
	private func parse_action_matrixObject(set: JsonObject, ActionDefs: MatrixObject,
			i: Int64, j: Int64) {
		let name_HV = ActionDefs[0, j]	/* 获取硬件变量名 FIXME:  该名称不得使用占位符 且不重复 */
		let arr = name_HV.split(".")
		if (arr.size != 2) {
			throw IllegalArgumentException("Action column is not valid")  }
		let name_dev = arr[0]
		var name_param = arr[1]

		var val_fp64 = MatrixVars_Hardware.parseMt<Float64>(ActionDefs, i, j)

		let AliasHV = M.get(name_param)		/* 可能的参数别名转换 */
		if (let Some(aliasHV) <- AliasHV) {
			let ret = aliasHV.play(JsonObject())
			let alias = ret.asString().getValue()
			name_param = alias
		}

		var set_params: JsonObject	/* 设置或者取得属于该硬件变量的参数 */
		if (let Some(obj_) <- set.get(name_dev)) {
			set_params = obj_.asObject()  }
		else {
			set_params = JsonObject()
			set.put(name_dev, set_params)  }

		set_params.put(name_param, JsonFloat(val_fp64))	/* 设置一个该硬件变量的参数 */
	}

	/*
	 *  skipCol: 收集元数据时跳过的列下标（-1 表示不跳过）
	 *  controller 列的值是控制器硬件变量的名字，既不是字面元数据，
	 *  也不应在收集阶段被 play（转发发生在 things 回调里）
	 */
	private func _getMetadata(matrix: MatrixObject, i: Int64, skipCol!: Int64 = -1): JsonObject {
		let obj = JsonObject()
		let idxName = matrix.indexOfCol(0, "name")
		let name = matrix[i, idxName]
		for (j in 0..matrix.col) {
			let colName = matrix[0, j]
			/* ---- 无法继承的 "-" 占位符（整列皆 "-" 时递归止于首行）归一为 "0"，
			 * ---- 与 parseMt 的既有约定对齐，避免 "-" 泄漏进元数据与转发载荷 ---- */
			var value = MatrixVars_Hardware.getMt(matrix, i, j)
			if (value == "-") { value = "0" }
			/* ---- ----
			 * ---- FIXME: 加入点号别名的检索逻辑 ----
			 * ---- ---- */
			if (j == idxName || j == skipCol) { continue  } /* 自身调用警告 */
			if (let Some(mt) <- M.get(value)) {
				/* ---- ----
				 * ---- 存在该名称的变量，传入空对象参数，并求取返回值
				 * ---- ---- */
				let json = JsonObject()
				let res = mt.play(json).asObject()
				let fields: HashMap<String, JsonValue> = res.getFields()
				for (k in fields.keys()) {
					let val = fields.get(k).getOrThrow()
					obj.put(k, val)
				}
					
			}
			else {
				obj.put(colName, JsonString(value))
			}
		}

		return obj
		
	}


}


