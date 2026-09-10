dofile((os.getenv("BOT4CJ_MODULE_PATH") or "./modules") .. "/lib/drv_common.lua")
-- Monitor.lua - 守护数据消费 / 闭环控制示例驱动
-- bot4cj 仓颉语言机器人框架
--
-- 用法：在结构脚本中定义 @Monitor 矩阵，某列的值填一个 @Daemon 变量名，
-- 经 _getMetadata 的引用机制，该守护 play() 返回的 v0..vN / count / status
-- 会被合并进本变量的元数据，call() 里即可用 data.v0 等读到实时采样值。
--
--   CREATE TABLE Daemon (name String, chunk String);
--   INSERT INTO Daemon (name, chunk) VALUES ('sim0', 'lib/daemon_probe.lua');
--   CREATE TABLE Monitor (name String, src String);
--   INSERT INTO Monitor (name, src) VALUES ('mon0', 'sim0');   -- src 引用守护 sim0
--
-- 本示例：读 v0 按阈值判定 level，并回传 out=v0；若本变量再声明 controller 列，
-- 框架会把返回值转发给执行器，形成「守护采样 → 引用消费 → 决策 → 转发执行」闭环。

local last = nil		-- 最近一次 call 输出快照（read 用，P4/M1）

function build(data)
	local ok, ek = arg_table(data)
	if not ok then
		return ek
	end
	return { result = "OK" }
end

function destroy(data)
	local ok, ek = arg_table(data)
	if not ok then
		return ek
	end
	return { result = "OK" }
end

function call(data)
	local ok, ek = arg_table(data)
	if not ok then
		return ek
	end

	-- data.v0.. / count 来自被引用的守护（经引用机制合并进元数据）
	local v0 = tonumber(data.v0)
	local count = tonumber(data.count)
	if v0 == nil then
		-- 守护尚未发布任何采样（启动瞬间）或被引用列缺失
		return { result = "ERR", message = "no daemon value (v0 missing)" }
	end

	local level = (v0 > 1.0) and "HIGH" or "LOW"
	if os.getenv("BOT4CJ_DEBUG") == "1" then
		print(string.format("Monitor: v0=%s count=%s -> level=%s",
			tostring(v0), tostring(count), level))
	end

	-- 回传决策值（若声明 controller 列，框架会把它转发给执行器）
	last = { result = "OK", level = level, out = v0 }
	return last
end

-- P4/M1：引用语义 = 读最近一次决策快照（无副作用）
function read(data)
	if last == nil then
		return { result = "ERR", message = "no sample yet" }
	end
	return last
end
