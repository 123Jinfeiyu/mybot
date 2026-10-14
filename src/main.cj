package bot4cj
import xxisp.*
import std.collection.*

main(): Int64 {
	test_lisp()
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
