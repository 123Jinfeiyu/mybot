/*
 *  ActionSource：可替换的动作输入源工厂
 *
 *  设计动机：bot4cj 原有入口仅支持 stdin 管道读 .mt。
 *  本模块将"动作输入"抽象为 InputStream，支持三种源：
 *    - stdin（缺省，向后兼容）
 *    - 文件（--input <path>，单次执行后退出）
 *    - TCP socket（--tcp <port>，长驻：硬件 active() 一次后反复 accept，
 *      每连接下发并执行一份 .mt，单脚本报错只打印、不带走服务；
 *      缺省仅接受回环对端，--tcp-max 限定连接数、--allow-remote 放开）
 *
 *  与 RobotStruct.cj 的集成：RobotStruct.run(input: InputStream) 已接受 InputStream，
 *  本工厂返回的 InputStream 直接传入即可，核心侧零改动（长驻模式逐连接重复调 run）。
 *
 *  注意（Cangjie std.io 事实，1.0.x/1.1.x 一致）：InputStream 是【字节流】
 *  接口，唯一抽象成员为 read(buffer: Array<Byte>): Int64（读到 EOF 返回 0），
 *  并没有 read(): ?Rune 字符方法；字符读取由 StringReader/ConsoleReader 等
 *  包装提供。File 与 TcpSocket 本身即 InputStream，无需 getReader() 转换。
 */

package bot4cj

import std.io.*
import std.net.*
import std.env.*
import std.fs.*
import std.collection.*
import std.convert.*

/* ---- ActionSource 工厂：input > stdin（TCP 走 openTcpListener 长驻分支）---- */
/* 参数取 ReadOnlyMap：std.argopt 解析出的 options 即该类型（只需读操作） */
public func openActionSource(options: ReadOnlyMap<String, String>): Option<InputStream> {
	/* ---- 文件模式（File 本身即字节 InputStream）---- */
	if (options.contains("input")) {
		let path = options["input"]
		try {
			let f = File(path, OpenMode.Read)
			println("从文件读取动作脚本：${path}")
			return Some(f)
		} catch (e: Exception) {
			println("无法打开动作脚本文件 '${path}': ${e.message}")
			return None
		}
	}

	/* ---- 缺省：stdin ---- */
	return Some(getStdIn())
}

/* ---- 打开 TCP 监听器：--tcp 缺席返回 None，端口非法抛出（由 main 统一报错）---- */
public func openTcpListener(options: ReadOnlyMap<String, String>): Option<TcpListener> {
	if (!options.contains("tcp")) {
		return None
	}
	let portStr = options["tcp"]
	/* 解析失败按 -1 处理，与越界值共用同一校验/报错路径 */
	let port = try { Int64.parse(portStr) } catch (e: Exception) { -1 }
	if (port < 1 || port > 65535) {
		throw IllegalArgumentException("--tcp 端口号无效：${portStr}（合法范围 1-65535）")
	}
	return Some(TcpListener(port, allowRemote: options.contains("allow-remote")))
}

/* ---- --tcp-max 连接数上限（缺省 0 = 不限，长驻到进程被终止）---- */
public func tcpMaxConnections(options: ReadOnlyMap<String, String>): Int64 {
	if (!options.contains("tcp-max")) {
		return 0
	}
	let raw = options["tcp-max"]
	let parsed = try { Int64.parse(raw) } catch (e: Exception) { -1 }
	if (parsed < 0) {
		throw IllegalArgumentException("--tcp-max 需为不小于 0 的整数（0 = 不限），实际为 '${raw}'")
	}
	return parsed
}

/*
 * 长驻动作服务：逐连接执行动作脚本，直到服务满 maxConn 个（0 = 不限）。
 *
 * 单个脚本解析/执行出错只打印并继续服务下一个连接——一次脚本报错不得带走
 * 整个机器人控制器。硬件仅在进入本循环前 active() 一次，跨连接保持已建状态。
 *
 * 注意：maxConn == 0 时本循环永不退出，main 的 finally deactivate() 只在达到
 * 上限正常收尾时执行；仓颉 stdlib 无信号接口（实测 1.1.3 无 std.os 包，
 * std.posix 不含 signal），Ctrl-C 不走本清理路径。
 */
/* 仅包内调用（main.cj）：RobotStruct 为 internal 类，不得出现在 public 签名中 */
func serveTcpActions(robot: RobotStruct, listener: TcpListener, maxConn: Int64): Int64 {
	var served = 0
	while (maxConn == 0 || served < maxConn) {
		if (let Some(input) <- listener.acceptNext()) {
			try {
				robot.run(input)
			} catch (e: Exception) {
				println("Aborted: ${e}（继续服务下一个连接）")
			} finally {
				input.close()
			}
			served += 1
			println("TCP 动作源：已服务 ${served} 个连接")
		}
	}
	return served
}

/*
 * TCP 长驻监听器：一次 bind，反复 accept。
 *
 * 访问控制：SDK 1.1.3 的 TcpServerSocket 只接受 bindAt: 端口（SocketAddress
 * 为抽象类不可构造，实测无按地址绑定的入口），监听面必为 0.0.0.0——绑定层
 * 收不窄，故改在对端判定上：缺省只接受回环对端，其余在读取任何字节前关闭。
 * --allow-remote 显式放开；但 Lua 沙箱不是安全边界（驱动可经 ffi 直调 libc），
 * 放开等价于开放远程执行，只应在自行建立信任边界（隧道/防火墙）后使用。
 */
public class TcpListener {
	private let server: TcpServerSocket
	private let allowRemote: Bool

	init(port: Int64, allowRemote!: Bool = false) {
		let port16 = UInt16(port)
		this.server = TcpServerSocket(bindAt: port16)
		this.server.bind()
		this.allowRemote = allowRemote
		println("TCP 动作源：监听 :${port16}，等待客户端连接（缺省仅接受回环对端）...")
	}

	/* 接受下一个连接；被拒对端就地关闭并返回 None，调用方继续等下一个 */
	public func acceptNext(): Option<TcpStreamReader> {
		let sock = server.accept()
		let peer = "${sock.remoteAddress}"
		if (!allowRemote && !isLoopbackPeer(peer)) {
			println("TCP 动作源：拒绝非回环对端 ${peer}（远程下发需显式加 --allow-remote）")
			sock.close()
			return None
		}
		println("TCP 客户端已连接：${peer}")
		return Some(TcpStreamReader(sock))
	}
}

/* SocketAddress 的字符串形如 "127.0.0.1:49186" / "[::1]:49186"，前缀判定即可 */
func isLoopbackPeer(peer: String): Bool {
	return peer.startsWith("127.") || peer.startsWith("[::1]:")
}

/*
 * TcpStreamReader：TcpSocket 字节流的断连安全包装。
 *
 * InputStream 是字节流接口（read(buffer): Int64，EOF 返 0）；.mt 脚本为
 * 纯 ASCII，字节即字符，消费端 StringReader 按字节做 UTF-8 解码即可。
 * 对端关闭连接时 socket 可能抛 SocketException，此处吞掉并返回 0（EOF），
 * 让上层 run() 像读到管道 EOF 一样正常收尾，而不是 abort 退出。
 */
public class TcpStreamReader <: InputStream {

	private let sock: TcpSocket

	public init(sock: TcpSocket) {
		this.sock = sock
	}

	public func read(buffer: Array<Byte>): Int64 {
		try {
			return sock.read(buffer)
		} catch (e: SocketException) {
			if (!sock.isClosed()) {
				sock.close()
			}
			return 0
		}
	}

	/* 长驻模式逐连接收尾：服务端主动关闭，避免客户端异常断连时句柄滞留 */
	public func close(): Unit {
		if (!sock.isClosed()) {
			sock.close()
		}
	}
}
