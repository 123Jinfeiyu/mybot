/*  动作输入源工厂的单元测试（ActionSource.cj）
*
*  TCP 长驻分支的两条契约在此钉死：
*   ① 参数校验（端口、连接数上限）必须在 bind 之前显式报错，不静默降级
*   ② 回环对端白名单是 --tcp 缺省唯一的访问控制面（SDK 无法按地址绑定），
*      判定函数的正确性等价于「远程能不能打进这台机器人」
*  合法端口的 bind/accept 属真 socket 行为，由 scripts/tcp_e2e.sh 端到端覆盖。
*/

package bot4cj
import std.unittest.*
import std.collection.*
import bot4cj.HV.*

func opt0(): HashMap<String, String> {
	return HashMap<String, String>()
}

@Test
class ActionSourceTest {
	/* ---- 无 --tcp 时返回 None（让 main 走文件/stdin 分支）---- */
	@TestCase
	func absentTcpYieldsNoListener() {
		no(openTcpListener(opt0()).isSome())
	}

	/* ---- 端口非法一律抛出且带原值，不再「打一行提示后返回 None」被误当成缺省模式 ---- */
	@TestCase
	func invalidPortThrows() {
		let m = opt0()
		m["tcp"] = "abc"
		throwsWith("端口号无效", { => openTcpListener(m) })

		let z = opt0()
		z["tcp"] = "0"
		throwsWith("端口号无效", { => openTcpListener(z) })

		let big = opt0()
		big["tcp"] = "70000"
		throwsWith("端口号无效", { => openTcpListener(big) })
	}

	/* ---- --tcp-max：缺省 0 = 不限 ---- */
	@TestCase
	func tcpMaxDefaultsToUnlimited() {
		eq(0i64, tcpMaxConnections(opt0()))
		let m = opt0()
		m["tcp-max"] = "3"
		eq(3i64, tcpMaxConnections(m))
		/* 0 显式合法（等价不限） */
		let z = opt0()
		z["tcp-max"] = "0"
		eq(0i64, tcpMaxConnections(z))
	}

	@TestCase
	func tcpMaxRejectsNegativeAndGarbage() {
		let neg = opt0()
		neg["tcp-max"] = "-1"
		throwsWith("--tcp-max", { => tcpMaxConnections(neg) })

		let bad = opt0()
		bad["tcp-max"] = "many"
		throwsWith("--tcp-max", { => tcpMaxConnections(bad) })
	}

	/* ---- 回环判定：IPv4 整段 127.0.0.0/8、IPv6 带方括号形式 ---- */
	@TestCase
	func loopbackPeerDetection() {
		yes(isLoopbackPeer("127.0.0.1:49186"))
		yes(isLoopbackPeer("127.0.0.2:1"))
		yes(isLoopbackPeer("127.255.255.255:65535"))
		yes(isLoopbackPeer("[::1]:49186"))
	}

	@TestCase
	func nonLoopbackPeerRejected() {
		no(isLoopbackPeer("192.168.3.158:51207"))
		no(isLoopbackPeer("10.0.0.5:22"))
		no(isLoopbackPeer("0.0.0.0:1"))
		no(isLoopbackPeer("::ffff:192.168.1.1:1"))
		/* 前缀判定不得把 127x.xxx 误纳为回环 */
		no(isLoopbackPeer("1270.0.0.1:1"))
		no(isLoopbackPeer("1abc:1"))
		no(isLoopbackPeer(""))
	}
}
