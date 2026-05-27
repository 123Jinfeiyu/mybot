macro package bot4cj.hd.extern

import std.fs.*
import std.io.*
import std.ast.*

public macro ExternHardware(attrTokens: Tokens, inputTokens: Tokens): Tokens {
	let expr = LitConstExpr(attrTokens)
	let file = File(expr.literal.value, OpenMode.Read)
	let bytes = readToEnd(file)
	file.close()
	let sb = StringBuilder()
	sb.appendFromUtf8(bytes)
	let code = sb.toString()
	return cangjieLex(code)
}
