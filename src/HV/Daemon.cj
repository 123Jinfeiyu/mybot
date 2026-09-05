/*
 * ---------------- 守护线程硬件变量（@Daemon，自 r.cj 迁出） ----------------
 *  用 Cangjie spawn 原语起守护线程（运行时已附着，区别于 pthread4cj 裸线程——
 *  后者在 foreign 线程上跑 Cangjie @C 代码必崩，已 PoC 证伪）。线程上创建
 *  【独立 lua_State】，loadfilex 采样 chunk 一次 + pcall 一次（chunk 内 while 循环
 *  轮询停止标志）。线程与其 lua_State 1:1 独占，主线程绝不触碰该状态机。
 *
 *  IPC：共享内存槽 daemon_slot_t（布局见 modules/lib/pthread.lua，两侧严格一致），
 *  守护侧 Lua 经 pthread.lua 的 publish 写值（mutex 保护），主线程侧 play 经
 *  foreign pthread_mutex 加锁读取，防撕裂。
 */

package bot4cj.HV
import std.time.*
import std.convert.*
import std.collection.*
import stdx.encoding.json.*
import ctype4cj.*
import luajit4cj.*
import mt2.*

/* ---- 共享槽 daemon_slot_t 布局偏移（必须与 modules/lib/pthread.lua 一致）---- */
public let SLOT_COUNT_OFF: Int64 = 64		/* int32：publish 次数 */
public let SLOT_NVALS_OFF: Int64 = 68		/* int32：本次值个数 */
public let SLOT_VALUES_OFF: Int64 = 72		/* double[16]：采样值 */
public let SLOT_STOP_OFF: Int64 = 200		/* uint8：停止标志 */
public let SLOT_SIZE: Int64 = 208
public let SLOT_MAXVALS: Int64 = 16

/* ---- libc pthread_mutex（直接 foreign 声明；libc 已链接，不需 pthread4cj）---- */
foreign func pthread_mutex_init(m: CPointer<UInt8>, attr: CPointer<Unit>): Int32
foreign func pthread_mutex_lock(m: CPointer<UInt8>): Int32
foreign func pthread_mutex_unlock(m: CPointer<UInt8>): Int32
foreign func pthread_mutex_destroy(m: CPointer<UInt8>): Int32

/* ---- stop 标志读写：统一经槽内 mutex 临界区 ----
 * mutex 的 lock/unlock 提供 acquire/release 内存语义，清除守护线程与主线程
 * 裸读写 stop 的数据竞争（P2：原 (slot+SLOT_STOP_OFF).read()/write(1u8) 为无序访问，
 * arm64 弱内存序下理论可见性延迟）。锁临界区极短、无嵌套，无死锁风险。 */
public func daemon_request_stop(slot: CPointer<UInt8>): Unit {
	unsafe {
		pthread_mutex_lock(slot)
		(slot + SLOT_STOP_OFF).write(1u8)
		pthread_mutex_unlock(slot)
	}
}

public func daemon_stop_requested(slot: CPointer<UInt8>): Bool {
	unsafe {
		pthread_mutex_lock(slot)
		let s = (slot + SLOT_STOP_OFF).read()
		pthread_mutex_unlock(slot)
		return s != 0u8
	}
}

/* ---- 可变状态持有者：rise/free 闭包捕获此 let 引用并改其字段，
 *      规避 Cangjie「lambda 捕获可变变量」限制 ---- */
public class DaemonHolder {
	public let slot: CPointer<UInt8>
	public var future: Option<Future<Int64>>
	public init(slot!: CPointer<UInt8>) {
		this.slot = slot
		this.future = None
	}
}

/* ---- 守护线程体（spawn 绿色线程上运行）：建私有 lua_State，注入槽地址+配置，
 *      先跑 chunk 一次完成 setup 并定义全局 daemon_tick，随后逐拍驱动 ---- */
public func daemon_body(slot: CPointer<UInt8>, chunk: CString, config: CString, interval_ms: Int64): Int64 {
	unsafe {
		let L = Lua_init()
		if (Lua_check(L) == LUA_ERR) { return -1i64 }
		Lua_loadlib(L)

		/* 注入共享槽地址（整数）与配置串（"k=v;k=v"）为 Lua 全局 */
		Lua_pushinteger(L, Int64(slot.toUIntNative()))
		Lua_setfield(L, LUA_GLOBALSINDEX, LibC.mallocCString("DAEMON_SLOT_ADDR"))
		Lua_pushstring(L, config)
		Lua_setfield(L, LUA_GLOBALSINDEX, LibC.mallocCString("DAEMON_CONFIG"))

		/* 跑 chunk 一次：完成 setup（开总线/配置传感器/附着槽）并定义全局 daemon_tick */
		let st = Lua_loadfilex(L, chunk, CPointer<C_Char>())
		if (st != LUA_OK) {
			let err = Lua_tostring(L, -1)
			println("[daemon] chunk load error: ${err.toString()}")
			Lua_free(L)
			return -1i64
		}
		var pcs = Lua_pcall(L, 0, LUA_MULTRET, 0)
		if (pcs != LUA_OK) {
			let err = Lua_tostring(L, -1)
			println("[daemon] chunk setup error: ${err.toString()}")
			Lua_free(L)
			return -1i64
		}
		Lua_settop(L, 0)

		/* 逐拍循环：每拍 getglobal(daemon_tick)+pcall 采样一次；拍间用 Cangjie sleep——
		 * 在 M:N 运行时下 sleep 会让出绿色线程、不独占 schd-worker（D 方案关键，
		 * 区别于 chunk 内 ffi usleep 阻塞 worker 导致 worker 池耗尽的死锁）。 */
		let tickName = LibC.mallocCString("daemon_tick")
		while (!daemon_stop_requested(slot)) {
			Lua_getglobal(L, tickName)
			pcs = Lua_pcall(L, 0, LUA_MULTRET, 0)
			if (pcs != LUA_OK) {
				let err = Lua_tostring(L, -1)
				println("[daemon] tick error: ${err.toString()}")
			}
			Lua_settop(L, 0)
			sleep(Duration.millisecond * interval_ms)
		}

		let n = CPointer<Int32>(slot + SLOT_COUNT_OFF).read()
		Lua_free(L)
		return Int64(n)
	}
}

/* ---- 守护硬件变量 ---- */
public class MtVars_Daemon <: MatrixVars_Hardware {
	public init(things!: (JsonValue) -> JsonValue,
			rise!: Option<() -> Unit> = None,
			free!: Option<() -> Unit> = None)
	{  super(things: things, rise: rise, free: free)  }

	/*
	 *  @Daemon
	 *   name    chunk;
	 *   sim0    modules/lib/daemon_probe.lua;
	 *  @
	 */
	public static func Factory(defs: MatrixObject, i: Int64,
			_: HashMap<String, MatrixVars_Hardware>,
			registry: ArrayList<DaemonHolder>): MtVars_Daemon
	{
		let idxName = defs.indexOfCol(0, "name")
		let idxChunk = defs.indexOfCol(0, "chunk")
		let name = defs[i, idxName]
		let chunkPath = MatrixVars_Hardware.getMt(defs, i, idxChunk)
		println("Create a Daemon hard-Var: ${name}, chunk: ${chunkPath}")

		/* 收集 name/chunk 之外的列为配置串 "k=v;k=v"，注入 chunk（如 i2c/addr/interval） */
		var cfgAcc = ""
		for (j in 0..defs.col) {
			let colName = defs[0, j]
			if (colName == "name" || colName == "chunk") { continue }
			let v = MatrixVars_Hardware.getMt(defs, i, j)
			cfgAcc = "${cfgAcc}${colName}=${v};"
		}
		let configStr = cfgAcc

		/* 提取采样间隔（ms）供 daemon_body 逐拍 Cangjie sleep；缺省 5ms。
		 * 用 let 定型以便 rise 闭包安全捕获（规避捕获可变变量） */
		var ivAcc: Int64 = 5
		for (j in 0..defs.col) {
			if (defs[0, j] == "interval") {
				let iv = MatrixVars_Hardware.getMt(defs, i, j)
				if (iv != "-") { ivAcc = Int64.parse(iv) }
			}
		}
		let intervalMs = ivAcc

		/* 分配并清零共享槽，初始化 mutex（offset 0） */
		let slot = unsafe { LibC.malloc<UInt8>(count: SLOT_SIZE) }
		unsafe {
			for (k in 0..SLOT_SIZE) { (slot + k).write(0u8) }
			pthread_mutex_init(slot, CPointer<Unit>())
		}
		let holder = DaemonHolder(slot: slot)
		registry.add(holder)	/* 注册：deactive 先统一置 stop 再 join，防饿死死锁 */

		let daemon = MtVars_Daemon(
			/* play：主线程加锁读采样值 → JSON。值同时以标量键 v0..vN 暴露，
			 *       供别的硬件变量经引用机制读取（call_lua 只传标量、不传 JsonArray） */
			things: { _: JsonValue =>
				let ret = JsonObject()
				let arr = JsonArray()
				unsafe {
					pthread_mutex_lock(holder.slot)
					let nvals = Int64(CPointer<Int32>(holder.slot + SLOT_NVALS_OFF).read())
					let count = Int64(CPointer<Int32>(holder.slot + SLOT_COUNT_OFF).read())
					var nv = nvals
					if (nv > SLOT_MAXVALS) { nv = SLOT_MAXVALS }
					for (k in 0..nv) {
						let v = CPointer<Float64>(holder.slot + SLOT_VALUES_OFF + 8 * k).read()
						arr.add(JsonFloat(v))
						ret.put("v${k}", JsonFloat(v))
					}
					pthread_mutex_unlock(holder.slot)
					ret.put("status", JsonString("OK"))
					ret.put("count", JsonInt(count))
					ret.put("nvals", JsonInt(nvals))
					ret.put("values", arr)
					println("Daemon '${name}' play: count=${count} nvals=${nvals}")
				}
				return ret
			},
			/* rise：spawn 守护线程（Cangjie 原生并发原语，运行时已附着） */
			rise: { =>
				unsafe {
					let cstr = LibC.mallocCString(chunkPath)
					let cfg_cstr = LibC.mallocCString(configStr)
					let slot_p = holder.slot
					let f = spawn { daemon_body(slot_p, cstr, cfg_cstr, intervalMs) }
					holder.future = Option<Future<Int64>>.Some(f)
					println("Daemon '${name}' spawned, config: ${configStr}")
				}
			},
			/* free：置停止标志 + future.get() 等待退出 + 销毁 mutex（显式停止，见 issue #3） */
			free: { =>
				unsafe {
					daemon_request_stop(holder.slot)
					if (let Some(f) <- holder.future) {
						let n = f.get()
						println("Daemon '${name}' joined, total publishes=${n}")
					}
					pthread_mutex_destroy(holder.slot)
					LibC.free(holder.slot)
				}
			}
		)
		return daemon
	}
}
