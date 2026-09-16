/*  守护硬件变量的纯逻辑单元测试（Daemon.cj）
*
*  覆盖三块此前只被 37 组守护 E2E 间接扫过的逻辑：
*   ① chunk 路径寻址（issue #19 多路径 + 可任意目录运行）——写错即「换目录跑不起」
*   ② 共享槽布局常量的自洽性（跨语言一致由 test/test_slot_layout.lua 钉死，
*      这里钉住仓颉侧自身的算术关系）
*   ③ stop 标志经槽内 mutex 的读写往返（P2 防数据竞争的那道锁）
*/

package bot4cj
import std.unittest.*
import bot4cj.HV.*
import ctype4cj.*
import pthread4cj.*

@Test
class DaemonTest {
	/* ---- ① 模块路径寻址 ---- */
	@TestCase
	func firstExistingModuleHitsFirstExistingPath() {
		eq("./modules/lib/mpath.lua", firstExistingModule("./modules", "lib/mpath.lua"))
		/* 多路径按序探测：第一段不存在时落到第二段 */
		eq("./modules/lib/mpath.lua", firstExistingModule("/no/such/dir:./modules", "lib/mpath.lua"))
	}

	@TestCase
	func firstExistingModuleReturnsLastPathWhenAllMissing() {
		/* 全部不存在时返回最后一段拼接（保持原有「文件不存在」加载报错语义） */
		eq("nope-b/nope-b.lua", firstExistingModule("nope-a:nope-b", "nope-b.lua"))
	}

	@TestCase
	func resolveDaemonChunkKeepsAbsoluteAndAlreadyRooted() {
		eq("/srv/x.lua", resolveDaemonChunk("/srv/x.lua", "./modules"))
		eq("./modules/lib/sensor_daemon.lua", resolveDaemonChunk("./modules/lib/sensor_daemon.lua", "./modules"))
		/* 兼容不带 ./ 的旧写法，避免拼成 ./modules/modules/... */
		eq("modules/lib/sensor_daemon.lua", resolveDaemonChunk("modules/lib/sensor_daemon.lua", "./modules"))
	}

	@TestCase
	func resolveDaemonChunkRelatesRelativeToModulePath() {
		eq("./modules/lib/sensor_daemon.lua", resolveDaemonChunk("lib/sensor_daemon.lua", "./modules"))
	}

	/* ---- ② 槽布局常量自洽（与 pthread.lua 的一致性由 Lua 侧测试交叉校验）---- */
	@TestCase
	func slotLayoutIsSelfConsistent() {
		eq(64i64, SLOT_COUNT_OFF)
		eq(68i64, SLOT_NVALS_OFF)
		eq(72i64, SLOT_VALUES_OFF)
		eq(32i64, SLOT_MAXVALS)
		/* stop 紧随 values[MAXVALS] 之后（8 字节对齐下即 72 + 256） */
		eq(SLOT_VALUES_OFF + 8 * SLOT_MAXVALS, SLOT_STOP_OFF)
		yes(SLOT_SIZE > SLOT_STOP_OFF)
		/* count/nvals 两个 int32 必须落在 mutex 预留之后、values 之前 */
		yes(SLOT_COUNT_OFF >= 64 && SLOT_NVALS_OFF == SLOT_COUNT_OFF + 4)
	}

	/* ---- ③ stop 标志往返：未请求为 false，置位后为 true（读写同一条 mutex 临界区）---- */
	@TestCase
	func stopFlagRoundTripsThroughMutex() {
		let slot = unsafe { LibC.malloc<UInt8>(count: SLOT_SIZE) }
		if (slot.isNull()) {
			fail("LibC.malloc 失败，无法验证槽内 stop 标志")
		}
		unsafe {
			for (k in 0..SLOT_SIZE) { (slot + k).write(0u8) }
			_mutex_init(slot)
			no(daemon_stop_requested(slot))
			daemon_request_stop(slot)
			yes(daemon_stop_requested(slot))
			/* 重复置位幂等，且不清其它字段 */
			daemon_request_stop(slot)
			yes(daemon_stop_requested(slot))
			_mutex_destroy(slot)
			LibC.free(slot)
		}
	}
}
