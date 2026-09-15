#include "luavar.hpp"

#include <iostream>
#include <stdexcept>

#include "util.hpp"

namespace bot {

thread_local int MtVars_Lua::topDepth = 0;

/* ---- 工厂：创建 Lua 状态机并以私有构造构建硬件变量 ---- */
std::unique_ptr<MtVars_Lua> MtVars_Lua::Factory(const std::string& luapath,
		std::function<Json()> getMetadata, MatrixVars_Hardware* controller) {
	lua_State* L = luaL_newstate();
	if (L == nullptr) {
		throw std::runtime_error("Lua_check()");
	}
	return std::unique_ptr<MtVars_Lua>(new MtVars_Lua(L, luapath, std::move(getMetadata), controller));
}

/* ---- 构造 Lua 状态机（加载标准库 + 沙箱 + loadfile + pcall）---- */
bool MtVars_Lua::build_lua(lua_State* L, const std::string& luapath) {
	luaL_openlibs(L);
	sandbox_lua(L);

	int status = luaL_loadfile(L, luapath.c_str());
	if (status == LUA_OK) {
		int pcStatus = lua_pcall(L, 0, LUA_MULTRET, 0);
		logD([pcStatus] { return "lua build status: " + std::to_string(pcStatus); });
		if (pcStatus == LUA_OK) { return true; }
	}

	/* 栈顶错误信息（判型防空指针）+ 恢复栈 */
	std::cout << luaErrorString(L) << std::endl;
	lua_settop(L, 0);
	return false;
}

/* ---- call 事件实现：元数据 ∪ @Action 实时参数 → Lua call() ---- */
Json MtVars_Lua::callImpl(const Json& json) {
	bool top = (MtVars_Lua::topDepth == 0);
	++MtVars_Lua::topDepth;
	struct DepthGuard { ~DepthGuard() { --MtVars_Lua::topDepth; } } guard;

	if (top) { std::cout << "----------" << std::endl; }

	/* 获取元数据（静态列缓存重放 + 引用列实时快照） */
	Json metadata = getMetadata_();

	/* 从元数据浅拷贝，再合并 @Action 传入的实时参数 */
	Json obj = JsonObject();
	jsonMergeInto(obj, metadata);
	obj.put("mode", JsonString("call"));
	jsonMergeInto(obj, json.asObject());
	logD([&obj] { return "Lua scripts call parameters: " + obj.toString(); });

	Json res = call_lua(L_, obj);
	logD([&res] { return "Lua script call result: " + res.toString(); });

	/* 本驱动 call 返回契约（含显式 ERR 即中断） */
	MatrixVars_Hardware::ensureOk(res, "call " + luapath_);

	/* controller 转发：元数据 ∪ 自己 Lua 的返回表 */
	if (controller_ != nullptr) {
		Json payload = JsonObject();
		jsonMergeInto(payload, metadata);
		jsonMergeInto(payload, res.asObject());
		logD([&payload] { return "Forward to controller: " + payload.toString(); });

		Json ctrl_res = controller_->play(payload);
		logD([&ctrl_res] { return "Controller result: " + ctrl_res.toString(); });

		/* 检查 controller 返回值（如 PCA9685 写寄存器失败） */
		MatrixVars_Hardware::ensureOk(ctrl_res, "controller of " + luapath_);

		if (top) { std::cout << ctrl_res.toString() << std::endl; }
		return ctrl_res;
	}

	if (top) { std::cout << res.toString() << std::endl; }
	return res;
}

/* ---- 无副作用读实现：驱动定义 read() 走快照，否则回退实时 call ----
 * 不做 ensureOk——引用读是数据流而非动作。 */
Json MtVars_Lua::readImpl() {
	int mode = luaGlobalType(L_, "read") != LUA_TNIL ? 1 : 0;
	Json metadata = getMetadata_();
	metadata.put("mode", JsonString(mode ? "read" : "call"));
	return call_lua(L_, metadata);
}

/* ---- 硬件构造：加载脚本 + 注入 build 指令 ---- */
void MtVars_Lua::riseImpl() {
	if (!build_lua(L_, luapath_)) {
		throw std::runtime_error("Failed to build Lua state from script: '" + luapath_
			+ "'. Please check if the file exists and is valid Lua code.");
	}

	Json metadata = getMetadata_();
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

	Json metadata = getMetadata_();
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