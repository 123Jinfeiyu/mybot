#include "luavar.hpp"

#include <iostream>
#include <stdexcept>

#include "util.hpp"

namespace bot {

thread_local int MtVars_Lua::topDepth = 0;

/* ---- 工厂：创建 Lua 状态机并以私有构造构建硬件变量 ---- */
std::unique_ptr<MtVars_Lua> MtVars_Lua::Factory(const std::string& luapath, const std::string& name,
		std::function<Json(bool)> getMetadata, std::vector<std::string> ctrlNames,
		std::vector<std::string> trigNames, HwLookup* registry) {
	if (!ctrlNames.empty() && registry == nullptr) {
		throw std::runtime_error("MtVars_Lua::Factory: controller '" + ctrlNames.front()
			+ "' requires a registry for lookup");
	}
	if (!trigNames.empty() && registry == nullptr) {
		throw std::runtime_error("MtVars_Lua::Factory: trigger '" + trigNames.front()
			+ "' requires a registry for lookup");
	}
	lua_State* L = openSandboxedState();
	if (L == nullptr) {
		throw std::runtime_error("Lua_check()");
	}
	return std::unique_ptr<MtVars_Lua>(new MtVars_Lua(L, luapath, name,
		std::move(getMetadata), std::move(ctrlNames), std::move(trigNames), registry));
}

/* ---- 加载 chunk 并执行一次（loadfile + pcall）----
 * 标准库与沙箱由 openSandboxedState 在创建状态时装好。 */
bool MtVars_Lua::build_lua(lua_State* L, const std::string& luapath) {
	int status = luaL_loadfile(L, luapath.c_str());
	if (status == LUA_OK) {
		int pcStatus = lua_pcall(L, 0, LUA_MULTRET, 0);
		logD([pcStatus] { return "lua build status: " + std::to_string(pcStatus); });
		if (pcStatus == LUA_OK) { return true; }
	}

	/* 栈顶错误信息（判型防空指针）+ 恢复栈 */
	std::cerr << luaErrorString(L) << std::endl;
	lua_settop(L, 0);
	return false;
}

/* ---- C-1 防线：@Action 参数键不得与结构元数据键同名 ----
 * 动作参数与结构元数据最终合并成同一张 Lua 表，若不加约束，.mt 里写一列
 * `dev.fd` / `dev.address` / `dev.max` 就能覆盖结构脚本的总线句柄、从机地址、
 * 舵机限幅——--tcp 模式下等价于远程改总线句柄与行程。
 * 收口语义：结构元数据由 .sql 独占，.mt 只能引入新的参数键。 */
void MtVars_Lua::rejectReservedKeys(const Json& metadata, const Json& params) const {
	static const char* const kFrameworkKeys[] = {"mode", "result", "message"};

	for (const auto& kv : params.items()) {
		const std::string& key = kv.first;
		const char* why = nullptr;
		if (metadata.find(key) != nullptr) { why = "a structure-script metadata column"; }
		for (const char* fw : kFrameworkKeys) {
			if (key == fw) { why = "a framework-reserved key"; }
		}
		for (const std::string& trig : trigNames_) {
			if (key == trig) {
				why = "a trigger reference name (the trigger result is merged under this key)";
			}
		}
		if (why != nullptr) {
			throw IllegalArgumentException("Hardware variable '" + name_ + "': @Action column '"
				+ name_ + "." + key + "' is rejected because '" + key + "' is " + why
				+ " and cannot be supplied by the action script. Action columns may only introduce"
				+ " new parameter names; bus handles, addresses and range limits come from the"
				+ " structure script.");
		}
	}
}

MatrixVars_Hardware* MtVars_Lua::flowTarget(std::vector<MatrixVars_Hardware*>& ptrs,
		const std::vector<std::string>& names, std::size_t k, const char* kind) {
	/* N-6：首次触发/转发缓存目标指针。注册表对引用名字的会话期覆盖已拒绝
	 * （HwRegistry::override），指针终身有效。 */
	if (ptrs[k] == nullptr) { ptrs[k] = registry_->find(names[k]); }
	MatrixVars_Hardware* target = ptrs[k];
	if (target == nullptr) {
		throw IllegalArgumentException(std::string(kind) + " '" + names[k]
			+ "' of hardware variable '" + name_
			+ "' is no longer registered (declaration order guarantees it exists)");
	}
	return target;
}

/* ---- trigger 前置调用（入边，controller 的镜像）：逐个以空参数运行 trigger
 * 列指向的变量，返回表以触发器名字为键并入调用参数，再运行自己的 Lua call。
 * - 触发是嵌套调用（topDepth 已 ++）：触发器自己的结果回显与顶层保留键
 *   检查被自然跳过，其 payload/转发语义与普通 play 完全一致；
 * - ERR/非表/空表经 ensureOk 响亮中断整帧（与 controller 转发同一契约）；
 * - 触发器数据在元数据与参数之后并入：顶层参数键同名已被
 *   rejectReservedKeys 拒绝，controller 转发 payload 的同名键在此让位。 */
void MtVars_Lua::mergeTriggerResults(Json& obj) {
	for (std::size_t k = 0; k < trigNames_.size(); ++k) {
		MatrixVars_Hardware* trigger = flowTarget(trigPtrs_, trigNames_, k, "trigger");
		logD([&] { return "Trigger call before play: " + trigNames_[k]; });
		Json tres = trigger->play(JsonObject());
		logD([&] { return "Trigger result (" + trigNames_[k] + "): " + tres.toString(); });
		MatrixVars_Hardware::ensureOk(tres, "trigger '" + trigNames_[k] + "' of " + luapath_);
		obj.put(trigNames_[k], tres);
	}
}

/* ---- controller 转发（出边，可多列 = 扇出）：元数据 ∪ 自己 Lua 的返回表构建
 * 一次，按列声明序逐个转发给 controller——每个 controller 收到同一份
 * payload（同列族不接力；跨变量链由目标自己的 controller 列表达）。
 * 出现一个 ERR 即停止并返回：不再转发给其余 controller，异常上抛整帧
 * 中断（halt-on-error，与触发器同一契约）。全部成功时返回最后一个
 * controller 的结果——单列与既有行为完全一致。 */
Json MtVars_Lua::forwardToControllers(const Json& metadata, const Json& res) {
	Json payload = JsonObject();
	jsonMergeInto(payload, metadata);
	jsonMergeInto(payload, res.asObject());
	logD([&payload] { return "Forward to controller: " + payload.toString(); });
	Json ctrl_res;
	for (std::size_t k = 0; k < ctrlNames_.size(); ++k) {
		MatrixVars_Hardware* controller = flowTarget(ctrlPtrs_, ctrlNames_, k, "controller");
		ctrl_res = controller->play(payload);
		logD([&] {
			return "Controller result (" + ctrlNames_[k] + "): " + ctrl_res.toString();
		});
		/* 检查本 controller 返回值（如 PCA9685 写寄存器失败）——ERR 即在此
		 * 中断，后续 controller 不再被转发 */
		MatrixVars_Hardware::ensureOk(ctrl_res, "controller '" + ctrlNames_[k] + "' of " + luapath_);
	}
	return ctrl_res;
}

/* ---- call 事件实现：trigger 前置调用 → 元数据 ∪ @Action 实时参数 → Lua call() ----
 * echo=false（playQuiet）时抑制顶层结果回显：agent 模式把驱动当模型传输用，
 * stdout 必须只留最终回答。其余语义（ERR 契约、controller 转发、元数据合并）不变。 */
Json MtVars_Lua::callImpl(const Json& json, bool echo) {
	/* 防御：状态机已释放（未 build 即派发，或异常路径先销毁后被引用）时清晰报错，
	 * 而非拿空指针进 LuaJIT 裸崩。readImpl/freeImpl 早有同款守卫，call 此前独缺
	 * ——destroy 之后的晚到派发会直接段错误（防御强度不对称）。 */
	if (L_ == nullptr) {
		throw IllegalArgumentException("call on '" + luapath_
			+ "': Lua state not built or already released (variable destroyed before being called)");
	}
	bool top = (MtVars_Lua::topDepth == 0);
	/* 计数与回退收敛到 util 的 RAII（B6）：本处只判「是否顶层」，不设上限
	 * （上限由 luabridge 的 kMaxLuaNesting 双向守），故用不带 limit 的 RecursionGuard。 */
	RecursionGuard depthGuard(MtVars_Lua::topDepth);

	if (top && echo) { std::cout << "----------" << std::endl; }

	/* 获取元数据（静态列缓存重放 + 引用列实时快照） */
	Json metadata = getMetadata_(false);

	/* 仅顶层（.mt 直接派发）校验：controller 转发的 payload 本就由框架拼装、
	 * 含上游元数据与返回键，属内部数据流，不受此约束。 */
	if (top) { rejectReservedKeys(metadata, json.asObject()); }

	/* 从元数据浅拷贝，再合并 @Action 传入的实时参数 */
	Json obj = JsonObject();
	jsonMergeInto(obj, metadata);
	jsonMergeInto(obj, json.asObject());
	mergeTriggerResults(obj);
	/* C-1 续：mode 必须在参数合并「之后」写入，由框架独占。controller payload 里
	 * 混着上游驱动返回表的键，若 mode 能被带进来，就会覆盖这里的 "call"，
	 * 使 call_lua 去 getglobal 一个外部可控的名字——实测可让下游驱动的 call()
	 * 完全不被执行、返回空表、退出码仍为 0（既跳过动作又绕过返回契约）。 */
	obj.put("mode", JsonString("call"));
	logD([&obj] { return "Lua scripts call parameters: " + obj.toString(); });

	Json res = call_lua(L_, obj);
	logD([&res] { return "Lua script call result: " + res.toString(); });

	/* 本驱动 call 返回契约（含显式 ERR 即中断） */
	MatrixVars_Hardware::ensureOk(res, "call " + luapath_);

	if (!ctrlNames_.empty()) {
		Json ctrl_res = forwardToControllers(metadata, res);
		if (top && echo) { std::cout << ctrl_res.toString() << std::endl; }
		return ctrl_res;
	}

	if (top && echo) { std::cout << res.toString() << std::endl; }
	return res;
}

/* ---- 无副作用读实现：驱动定义 read() 走快照，否则回退实时 call ----
 * 不做 ensureOk——引用读是数据流而非动作。 */
Json MtVars_Lua::readImpl() {
	/* 防御：状态机已释放（未 build 即销毁，或异常路径先销毁后被引用）时清晰报错，
	 * 而非拿空指针进 LuaJIT 裸崩（C1 环引用负路径实测暴露）。 */
	if (L_ == nullptr) {
		throw IllegalArgumentException("read on '" + luapath_
			+ "': Lua state not built or already released (variable destroyed before being read)");
	}
	int mode = luaGlobalType(L_, "read") != LUA_TNIL ? 1 : 0;
	Json metadata = getMetadata_(false);
	metadata.put("mode", JsonString(mode ? "read" : "call"));
	return call_lua(L_, metadata);
}

/* ---- 硬件构造：加载脚本 + 注入 build 指令 ---- */
void MtVars_Lua::riseImpl() {
	if (!build_lua(L_, luapath_)) {
		throw std::runtime_error("Failed to build Lua state from script: '" + luapath_
			+ "'. Please check if the file exists and is valid Lua code.");
	}

	Json metadata = getMetadata_(false);
	metadata.put("mode", JsonString("build"));
	logD([&metadata] { return "Lua scripts build parameters: " + metadata.toString(); });

	Json res = call_lua(L_, metadata);
	logD([&res] { return "Lua script build result: " + res.toString(); });

	/* build 返回契约：失败即抛（上层 finally 会清理已建硬件） */
	MatrixVars_Hardware::ensureOk(res, "build " + luapath_);
}

/* ---- 硬件析构：注入 destroy 指令；防御未构建状态机直接释放 ---- */
void MtVars_Lua::freeImpl() {
	/* 防御：build 失败的变量全局 destroy 为 nil——跳过调用、释放状态机后正常返回 */
	if (L_ == nullptr) { return; }
	if (luaGlobalType(L_, "destroy") == LUA_TNIL) {
		logD([this] {
			return "Lua '" + luapath_ + "' has no global destroy (not built); skipping destroy call, releasing state";
		});
		lua_close(L_);
		L_ = nullptr;
		return;
	}

	/* F-04′：destroy 的元数据取「字面列 + 引用列的尽力而为快照」。
	 * 必须带上引用列：总线句柄 fd / 从机 address 是器件行经 `$i2c-N` / `$uart-N`
	 * 从接口器件快照来的，而 drv_common 的 i2c_pair 要用它们——只取字面列会让
	 * PCA9685/MPU6050/BH1750 的 destroy 响亮失败，让带 `if fd ~= nil` 守卫的
	 * 十余个传感器静默丢失去初始化（MAX30102 的 BIT_SHDN、BH1750 的 POWER_DOWN
	 * 不再下发，器件滞留最后指令状态）。
	 * 单个引用已释放只在 collectForDestroy 内跳过那一条，异常不会传到这里；
	 * 外层 try 是最后一道兜底：无论如何 destroy 调用本身都必须发生。 */
	Json metadata = JsonObject();
	try {
		metadata = getMetadata_(true);
	} catch (const std::exception& e) {
		std::cerr << "警告：destroy 参数收集失败（" << e.what()
			<< "），以空表调用 destroy 以保证器件去初始化" << std::endl;
	}
	metadata.put("mode", JsonString("destroy"));
	logD([&metadata] { return "Lua scripts destroy parameters: " + metadata.toString(); });

	Json res = call_lua(L_, metadata);
	logD([&res] { return "Lua script destroy result: " + res.toString(); });

	/* 先释放状态机，再检查返回契约——避免检查抛异常时泄漏 */
	lua_close(L_);
	L_ = nullptr;
	MatrixVars_Hardware::ensureOk(res, "destroy " + luapath_);
}

} // namespace bot