-- Speaker 声卡播放 驱动模块（预设波形 + 音调 + 时长）
-- bot4cj 仓颉语言机器人框架
--
-- 经 libasound（ALSA）ffi 直驱宿主机声卡：给定波形名（preset）、音调
-- （freq，Hz 数字）与时长（delay，毫秒），在 call 期间合成并阻塞播放。
-- "default" 设备在 PipeWire/PulseAudio 桌面自动经兼容层混音路由，在树莓派
-- 走 ALSA dmix/裸 hw——长驻进程持有设备不妨碍其他应用出声（直配 "hw:..."
-- 除外，那是独占）。
--
-- 用法（结构脚本 .sql）：
--   CREATE TABLE Speaker (
--       name    String,   -- 实例名
--       device  String,   -- ALSA 设备名（缺省 "default"；直配 "hw:0,0" 为独占）
--       rate    String,   -- 采样率 Hz（缺省 16000，允许 8000~48000）
--       preset  String,   -- 缺省波形名（缺省 sine），见下表
--       volume  String,   -- 音量 0~100（缺省 80；平方曲线近似听感，非标定）
--       max_ms  String,   -- 单次播放时长上限 ms（缺省 10000，硬上限 60000）
--       simulate String   -- '1' = 仿真模式（不开声卡不出声，全量校验照跑）
--   );
--   INSERT INTO Speaker (name) VALUES ('spk0');
--
-- preset 波形集（sine / square / triangle / sawtooth / noise / silence）：
--   sine      纯音（最常用提示音）
--   square    方波（蜂鸣感，谐波经 16kHz 采样率自然截断）
--   triangle  三角波（比 sine 柔、比 sine 亮）
--   sawtooth  锯齿波（警报感）
--   noise     白噪声（LCG 伪随机、每次 call 重置种子可复现；freq 对白噪声
--             无频谱意义，仅保持契约统一仍必填）
--   silence   静音（写零样本占时——旋律行间的间隙）
--
-- call 参数（.mt 动作脚本列）：
--   wave     波形名（可选，逐拍指定；.mt 数据格只认数值，文本要经 $(别名)。
--            与结构列 preset 的分工：preset 是这台实例的常驻缺省（元数据列，
--            框架不允许 .mt 同名覆盖——param_override 防线），wave 是这一拍
--            想要什么波形，给了就优先）
--   freq     音调 Hz（silence 可省；其余波形必填，20~20000）
--   delay    时长 ms（必填，>0 且 ≤ max_ms）
--   volume   音量 0~100（结构列同名，.mt 不能用——param_override 防线会拒；
--            保留给 Lua 直调/controller 转发路径逐次覆盖，缺省用结构列值）
--
--   @Action
--   spk0.freq  spk0.delay;
--        880        150;      -- 用结构脚本缺省波形播 150ms
--        440        400;
--   @
--
-- 时序语义与 Timer 同构（阻塞式 call）：该行播多久声音就多久，同行后续列
-- 顺延——列序即播放序，与舵机列同帧混排即「先出腿、后发声」。旋律不需要
-- 新机制：.mt 的时间轴就是音序器，一行一音。
--
-- 运行期依赖：libasound.so.2（缺失时 build 报错并提示安装，不静默降级）。
-- simulate='1' 时不加载 libasound、不真睡（E2E 快跑），每音打印锚点行
-- `Speaker: wave=... freq=...Hz ...ms vol=...`，返回表带 simulated=true。
--
-- 不声明 SINGLE_CONSUMER：无总线事务，PCM 句柄由本驱动独占自持（第二个
-- Speaker 实例直配同一 "hw:x,y" 时由内核 -EBUSY 在 build 期拒绝）。

-- 标准前导：HAL/print_table/drv_common 经 mpath 加载（modules/lib/）
local mpath = (__mpath or dofile((os.getenv("BOT4CJ_MODULE_PATH") or "./modules"):match("[^:]+") .. "/lib/mpath.lua"))
dofile(mpath.resolve("lib/HAL.lua"))
dofile(mpath.resolve("lib/print_table.lua"))
dofile(mpath.resolve("lib/drv_common.lua"))
local asound = dofile(mpath.resolve("lib/asound.lua"))

-- 波形生成器：phase（单位周期）→ [-1, 1]。silence 由幅值零表达，不进表。
local WAVES = {
	sine     = function(p) return math.sin(2 * math.pi * (p % 1)) end,
	square   = function(p) return (p % 1) < 0.5 and 1 or -1 end,
	triangle = function(p) return 4 * math.abs((p % 1) - 0.5) - 1 end,
	sawtooth = function(p) return 2 * (p % 1) - 1 end,
	noise    = nil,			-- LCG 分支特判（见 gen_chunk），不占相位
	silence  = function(p) return 0 end,
}

-- 校验后的播放参数 → 模块级状态（build 写入，call 读缺省；destroy 后废弃）
local pcm = nil			-- ALSA 句柄（真实模式）；nil = 未开/仿真
local sim = false		-- 仿真模式
local cfg = { device = "default", rate = 16000, preset = "sine",
	volume = 80, max_ms = 10000 }

-- 分块合成的块大小（样本数）：4096 帧 ≈ 256ms@16kHz，块内 Lua 循环生成、
-- 块边界 writei 阻塞节流——内存有界且相位跨块连续（无缝）
local CHUNK = 4096

-- Attack/Release 包络长度（秒）：消爆音；不足 40ms 的短音按时长 1/4 缩
local ENV_S = 0.005

-- 白噪声 LCG（确定性：每次 call 重置，测试可复现；x mod 2^31，输出 [-1,1)）
local function lcg_next(x)
	x = (x * 1103515245 + 12345) % 2147483648
	return x
end

local function wave_of(name)
	if type(name) ~= "string" then return nil end
	return WAVES[string.lower(name)] ~= nil or string.lower(name) == "noise",
		string.lower(name)
end

-- 【配置脚本】校验列配置；真实模式开 PCM 设备（缺库/无声卡即报错，fail-fast）
function build(data)

	local ok, ek = arg_table(data)
	if not ok then
		return ek
	end

	dbg_table("-------- Speaker builder func get a table --------", data)

	-----------------------------------------------------
	-- device：缺省 "default"；给了就必须是非空字符串
	local dev = data.device
	if dev == nil or dev == "-" then
		dev = "default"
	end
	if type(dev) ~= "string" or #dev == 0 then
		return err("Invalid Argument: device (expect ALSA name like 'default')")
	end

	-- rate：缺省 16000；越界报错不钳制（采样率错是配置错误，就近吸附
	-- 会让音调整体偏移却听不出原因）
	local rate = finite_of(data.rate)
	if rate == nil then rate = asound.RATE_DEFAULT end
	if rate < asound.RATE_MIN or rate > asound.RATE_MAX
		or rate ~= math.floor(rate) then
		return err(string.format("Invalid Argument: rate (expect %d~%d integer Hz)",
			asound.RATE_MIN, asound.RATE_MAX))
	end

	-- preset：缺省 sine；名字必须命中波形集
	local okw, wname = wave_of(data.preset or "sine")
	if not okw then
		return err("Invalid Argument: preset '" .. tostring(data.preset)
			.. "' (expect sine/square/triangle/sawtooth/noise/silence)")
	end

	-- volume：缺省 80，0~100
	local vol = finite_of(data.volume)
	if vol == nil then vol = 80 end
	if vol < 0 or vol > 100 then
		return err("Invalid Argument: volume (expect 0~100)")
	end

	-- max_ms：缺省 10000，1~60000（超长提示音是设计错误，响亮拒绝）
	local mx = finite_of(data.max_ms)
	if mx == nil then mx = 10000 end
	if mx < 1 or mx > 60000 then
		return err("Invalid Argument: max_ms (expect 1~60000)")
	end

	sim = (tostring(data.simulate) == "1")
	cfg.device, cfg.rate, cfg.preset = dev, rate, wname
	cfg.volume, cfg.max_ms = vol, mx

	-----------------------------------------------------
	-- 重复 build 先释放旧句柄（与 PWM 的重复 build 守卫同款）
	if pcm ~= nil then
		asound.close(pcm)
		pcm = nil
	end

	if sim then
		print(string.format("Speaker init OK (simulate): %gHz, preset=%s, vol=%g",
			rate, wname, vol))
		return { result = "OK", device = dev, rate = rate,
			preset = wname, volume = vol, max_ms = mx, simulated = true }
	end

	local h, e = asound.open_stream(dev, rate)
	if h == nil then
		return err("Speaker: " .. tostring(e))
	end
	pcm = h

	print(string.format("Speaker init OK: %s, %gHz, preset=%s, vol=%g",
		dev, rate, wname, vol))
	return { result = "OK", device = dev, rate = rate,
		preset = wname, volume = vol, max_ms = mx }
end

-- 【销毁Speaker】drain 收尾 + 关设备；关失败如实上报（PWM 例外口径：
-- destroy 做的是器件状态收尾，失败不是「资源不可用」）
function destroy(data)

	local ok, ek = arg_table(data)
	if not ok then
		return ek
	end

	dbg_table("-------- Speaker ender func get a table --------", data)

	if pcm == nil then
		return { result = "OK", released = false }
	end

	asound.drain(pcm)		-- 尽力播完残 buffer；失败仍要 close
	local rc = asound.close(pcm)
	pcm = nil
	if rc ~= nil and rc < 0 then
		return err("Speaker: snd_pcm_close failed: " .. asound.errmsg(rc))
	end

	return { result = "OK", released = true }
end

-- 合成一个块：填 buf[0..n-1]（含 attack/release 包络），返回块帧数
local function fill_chunk(buf, total, done, wave, phase, amp16, env_len)
	local n = math.min(CHUNK, total - done)
	for i = 0, n - 1 do
		local env = 1
		local pos = done + i
		if pos < env_len then
			env = pos / env_len				-- attack
		elseif pos >= total - env_len then
			env = (total - pos) / env_len			-- release
		end
		buf[i] = math.floor(amp16 * env * wave(phase()) + 0.5)
	end
	return n
end

-- 【调用Speaker】合成并阻塞播放一个音
function call(data)

	local ok, ek = arg_table(data)
	if not ok then
		return ek
	end

	dbg_table("-------- Speaker caller func get a table --------", data)

	-----------------------------------------------------
	-- wave（.mt 逐拍）> preset（元数据/转发）> 结构缺省；
	-- noise 特判（WAVES 占位 nil）
	local okw, wname = wave_of(data.wave or data.preset or cfg.preset)
	if not okw then
		return err("Invalid Argument: wave '" .. tostring(data.wave or data.preset)
			.. "' (expect sine/square/triangle/sawtooth/noise/silence)")
	end
	local is_noise, is_silence = (wname == "noise"), (wname == "silence")

	-- freq：silence 可省；其余必填，20~20000（越界报错：错的音调不如不响）。
	-- 0 仅 silence 合法——square 相位不推进会恒 +1（直流全幅爆音）
	local freq = finite_of(data.freq)
	if freq == nil and is_silence then freq = 0 end
	if freq == nil then
		return err("Invalid Argument: freq (required, Hz)")
	end
	if freq == 0 and not is_silence then
		return err("Invalid Argument: freq 0 is only valid with preset=silence")
	end
	if freq ~= 0 and (freq < 20 or freq > 20000) then
		return err("Invalid Argument: freq '" .. tostring(data.freq)
			.. "' (expect 20~20000 Hz)")
	end

	-- delay：必填，>0 且 ≤ max_ms
	local ms = finite_of(data.delay)
	if ms == nil then
		return err("Invalid Argument: delay (required, ms)")
	end
	if ms <= 0 then
		return err("Invalid Argument: delay (must be > 0)")
	end
	if ms > cfg.max_ms then
		return err(string.format("Invalid Argument: delay %g exceeds max_ms %g",
			ms, cfg.max_ms))
	end

	-- volume：可选覆盖，0~100
	local vol = finite_of(data.volume)
	if vol == nil then vol = cfg.volume end
	if vol < 0 or vol > 100 then
		return err("Invalid Argument: volume (expect 0~100)")
	end

	-----------------------------------------------------
	-- 仿真模式：全量校验已过，打锚点即返回（不睡，E2E 快跑）
	if sim then
		print(string.format("Speaker: wave=%s freq=%gHz %gms vol=%g (simulate)",
			wname, freq, ms, vol))
		return { result = "OK", wave = wname, freq = freq,
			played_ms = ms, volume = vol, simulated = true }
	end

	if pcm == nil then
		return err("Speaker not initialized (build first)")
	end

	-- drain 后设备处于 SETUP 态，writei 前必须 prepare 拉回 PREPARED（首次
	-- prepare 幂等空操作）——真机连续两音实测：漏掉这条第二个音 -EBADFD
	local prc = asound.prepare(pcm)
	if prc ~= nil and prc < 0 then
		return err("Speaker: snd_pcm_prepare failed: " .. asound.errmsg(prc))
	end

	-----------------------------------------------------
	local rate = cfg.rate
	local total = math.floor(ms * rate / 1000 + 0.5)
	if total < 1 then total = 1 end
	local env_len = math.min(math.floor(ENV_S * rate + 0.5), math.floor(total / 4))
	local amp16 = 32767 * (vol / 100) * (vol / 100)	-- 平方近似听感

	-- 相位闭包与噪声状态（noise 不走相位，占位保持签名一致）
	local ph = 0
	local phase = function()
		ph = ph + freq / rate
		return ph
	end
	local lcg = 20260924			-- 每次 call 重置：噪声可复现
	local wave
	if is_noise then
		wave = function()
			lcg = lcg_next(lcg)
			return lcg / 1073741824 - 1	-- [0,2^31) → [-1,1)
		end
	else
		wave = WAVES[wname]
	end

	-- 块循环：填一块写一块；-EPIPE（underrun）prepare 后重写本块一次，
	-- 仍失败如实报错（静默丢样本 = 播放时长悄悄变短）
	local buf = asound.alloc(CHUNK)
	local done = 0
	while done < total do
		local n = fill_chunk(buf, total, done, wave, phase, amp16, env_len)
		local w = asound.write(pcm, buf, n)
		if w == nil or w < 0 then
			local rc = (w == nil) and 0 or w
			if rc == -asound.EPIPE then
				local pok = asound.prepare(pcm)
				local w2 = (pok ~= nil and pok >= 0) and asound.write(pcm, buf, n) or nil
				if w2 == nil or w2 < 0 then
					return err("Speaker: underrun recover failed: "
						.. asound.errmsg(rc))
				end
			else
				return err("Speaker: snd_pcm_writei failed: " .. asound.errmsg(rc))
			end
		end
		done = done + n
	end

	-- 播完缓冲内剩余样本再返回：call 返回 = 声音播完（.mt 时序语义）
	local rc = asound.drain(pcm)
	if rc ~= nil and rc < 0 then
		return err("Speaker: snd_pcm_drain failed: " .. asound.errmsg(rc))
	end

	if os.getenv("BOT4CJ_DEBUG") == "1" then
		print(string.format("Speaker: wave=%s freq=%gHz %gms vol=%g",
			wname, freq, ms, vol))
	end

	return { result = "OK", wave = wname, freq = freq,
		played_ms = ms, volume = vol }
end
