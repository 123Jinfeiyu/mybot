package bot4cj
import xxisp.*
import SQL4cj.*
import mt2.*
import std.io.*
import std.fs.*
import std.env.*
import std.collection.*
import std.argopt.*

var MTOS = LinkedList<MatrixObject>()
var MTAS = LinkedList<MatrixObject>()	// Actions
var DRVDIR = "./drivers/"
var BOTPATH = ""

/* 探测输入流是否有数据 */
func stream2mtos(input: InputStream, handler: LinkedList<MatrixObject>): Unit {
	let sr = StringReader(input)
	let parser = MatrixParser(sr)
	while (true) {
		if (let Some(OBJ) <- parser.parse()) {
			handler.addLast(OBJ)
		}
		else {  break; }
	}
}

func readBotpath(input: InputStream, handler: LinkedList<MatrixObject>): Unit {
	let sr = StringReader<InputStream>(input)
	let tokens = Lexer.parseAll(sr)
	let events = Parser.parseAll(tokens)
	for (event in events) {
		match(event) {
			case CREATE_TABLE(ev) =>
				let mt = createMtViaEv(ev)
				handler.addLast(mt)
			case  INSERT_TABLE(ev) =>
				insertMtViaEv(handler, ev)
			case END(_) => continue
			case _ =>
				throw Exception("Unsupported sql command")
		}
	}
}

func createMtViaEv(ev: CreateTableEvent): MatrixObject {
	let mt = MatrixObject()
	regiestMtName(mt, ev.tableName)
	regiestMtColumns(mt, ev)
	return mt
}

func regiestMtName(mt: MatrixObject, name: String): Unit {
	for (ch in name) {
		mt.addRune(Rune(ch))
	}
	mt.submitName()
	getStdErr().writeln("Hardware module ${name} has been regiested.")
}

func regiestMtColumns(mt: MatrixObject, ev: CreateTableEvent): Unit {
	for (column in ev.tableColumns) {
		match (column._type) {

			case _String =>
				for (ch in column.name) {
					mt.addRune(Rune(ch))
				}
				mt.submitItem()
			case _ => throw Exception("Unsupported typename")
		}
	}
	mt.submitLine(0)
}


func insertMtViaEv(handler: LinkedList<MatrixObject>, ev: InsertTableEvent): Unit {
	var matchedTable = false
	for (mt in handler) {
		if (ev.targetTabName == mt.getName()) {
			matchedTable = true
			insertToMtViaEv(mt, ev)
			break
		}
	}
	if (!matchedTable) {
		throw Exception("INSERT INTO ${ev.targetTabName} FAILED: table is not defined")
	}
}

/* 按表头列名重排写入：INSERT行的列顺序可任意，缺失表头列抛异常，重复列取后值 */
func insertToMtViaEv(mt: MatrixObject, ev: InsertTableEvent): Unit {
	var rowNo = 0
	for (row in ev.tableRows) {
		rowNo += 1
		let byName = HashMap<String, String>()
		for (col in row) {
			byName.add(col.columnName, col.value)
		}
		for (i in 0..mt.col) {
			let name = mt[0, i]
			if (let Some(value) <- byName.get(name)) {
				for (ch in value) {
					mt.addRune(Rune(ch))
				}
				mt.submitItem()
			} else {
				throw IllegalArgumentException("INSERT INTO ${ev.targetTabName} row ${rowNo}: column '${name}' is missing")
			}
		}
		mt.submitLine(0)
	}
}





let help_statement = '''
 选项列表：
   --help
   --drivers    选择驱动脚本列表
   --robot      选择结构SQL

 程序需要接收一个管道输入（.mt脚本）
'''
main(args: Array<String>): Int64 {

	let specs = [
		Long("help", ArgumentMode.NoValue),
		Short(r'h', NoValue),
		Long("drivers", ArgumentMode.RequiredValue),
		Long("robot", ArgumentMode.RequiredValue)
	]

	let parsed = parseArguments(args, specs)
	let options = parsed.options

	let nono = parsed.nonOptions
	if (!nono.isEmpty()) {
		println("Unknown Options: ${nono}")
		return 1
	}

	if (options.contains("help")
		|| options.contains("h")) {
		println(help_statement)
		return 0
	}

	DRVDIR = options.get("drivers") ?? DRVDIR
	BOTPATH = options.get("robot")  ?? ""

	if (BOTPATH == "" || !exists(BOTPATH)) {
		throw Exception("Robot structure file not exists")
	}

	let botfile = File(BOTPATH, Read)
	
	readBotpath(botfile, MTOS)
	stream2mtos(getStdIn(), MTAS)
	return 0
}

/*
 * 1. lisp驱动脚本的返回值仅在lisp脚本中流转，最多转成String以便智能体阅读
 */
class HdVar {
	private let interpreter: LispInterpreterX
	public init(code: String) {
		interpreter = LispInterpreterX()
		interpreter.eval(code)
	}
	public func exec(code: String): String {
		interpreter.eval(code)
	}
}

/* 测试lisp驱动 */
func test_lisp(): Unit {
	let hv = HdVar("""
(define (initial args) "OK")
(define (destroy args) "OK")
(define (execute args) (cangjie::list-dir "/dev"))
""")
	var res: String
	res = hv.exec("(initial \"arg\")"); println(res)
	res = hv.exec("(execute \"arg\")"); println(res)
	res = hv.exec("(destroy \"arg\")"); println(res)
}


