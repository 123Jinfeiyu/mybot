/*
 *  ActionSource：可替换的动作输入源工厂
 *
 *  设计动机：bot4cj 原有入口仅支持 stdin 管道读 .mt。
 *  本模块将"动作输入"抽象为 InputStream，支持三种源：
 *    - stdin（缺省，向后兼容）
 *    - TCP socket（--tcp <port>，长驻模式，客户端可下发 .mt）
 *    - 文件（--input <path>，单次执行后退出）
 *
 *  与 r.cj 的集成：RobotStruct.run(input: InputStream) 已接受 InputStream，
 *  本工厂返回的 InputStream 直接传入即可，核心侧零改动。
 */

package bot4cj

import std.io.*
import std.net.*
import std.env.*
import std.fs.*

/* ---- ActionSource 工厂：tcp > input > stdin ---- */
public func openActionSource(options: Map<String, String>): Option<InputStream> {
	/* ---- TCP 模式（最高优先级）---- */
	if (options.contains("tcp")) {
		let portStr = options["tcp"]
		let port = portStr.toLong()
		if (port < 1 || port > 65535) {
			println("--tcp 端口号无效：${portStr}（合法范围 1-65535）")
			return None
		}
		return Some(openTcpActionSource(port))
	}

	/* ---- 文件模式 ---- */
	if (options.contains("input")) {
		let path = options["input"]
		try {
			let f = File(path, OpenMode.Read)
			println("从文件读取动作脚本：${path}")
			return Some(f.getReader())
		} catch (e: Exception) {
			println("无法打开动作脚本文件 '${path}': ${e.message}")
			return None
		}
	}

	/* ---- 缺省：stdin ---- */
	return Some(getStdIn())
}

/*
 * 打开 TCP 动作源：监听 0.0.0.0:<port>，接受第一个客户端连接，
 * 返回适配为 InputStream 的字节流包装。
 *
 * 仓颉 stdlib API：
 *   TcpServerSocket(bindAt: UInt16) → .bind() → .accept() → TcpSocket
 *   TcpSocket 为字节流（read(Array<Byte>)），需适配为 InputStream（字符流）。
 */
public func openTcpActionSource(port: Int64): InputStream {
	let port16 = port.toUInt16()
	println("TCP 动作源：监听 0.0.0.0:${port16}，等待客户端连接...")
	let server = TcpServerSocket(bindAt: port16)
	server.bind()
	let sock = server.accept()
	let peer = sock.getRemoteAddress()
	println("TCP 客户端已连接：${peer}")
	return TcpStreamReader(sock)
}

/*
 * TcpStreamReader：将 TcpSocket 字节流适配为 InputStream（字符流）。
 * .mt 脚本为纯 ASCII，单字节 = 单 Rune 直接映射，无需 UTF-8 多字节解码。
 * socket EOF / SocketException → 返回 None（视为连接结束）。
 */
public class TcpStreamReader <: InputStream {

	private let sock: TcpSocket

	public init(sock: TcpSocket) {
		this.sock = sock
	}

	public override func read(): Option<Rune> {
		let buf = Array<Byte>(1, repeat: 0)
		let n: Int32
		try {
			n = sock.read(buf)
		} catch (e: SocketException) {
			if (!sock.isClosed()) {
				sock.close()
			}
			return None
		}
		if (n <= 0) {
			return None
		}
		return Some(Rune(buf[0].toUInt8()))
	}
}