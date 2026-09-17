#pragma once
/* luavar.hpp —— Lua 状态机硬件变量（移植 bot4cj/src/HV/Lua.cj）
 * Lua 状态机在工厂中提前创建；build() 加载脚本并跑一次 build，destroy() 释放。
 * controller 转发：元数据 ∪ 自己 Lua 返回表 → controller.play(payload)。
 */

#include <functional>
#include <memory>
#include <string>

#include "hardware.hpp"
#include "luaend.hpp"

namespace bot {

class HwRegistry;	/* 完整定义在 robot.hpp（luavar.cpp 内联实现查找） */

class MtVars_Lua : public MatrixVars_Hardware {
	/* 顶层调用深度计数：只在外层 .mt 直接调用的原子处打印结果，
	 * 避免被 controller 转发的嵌套 Lua 调用重复输出。
	 * thread_local：controller 转发是同线程同步调用链，需跨实例共享计数；
	 * 而守护线程等并发路径各自独立，杜绝共享静态态的跨线程竞态。 */
	static thread_local int topDepth;

	lua_State* L_;
	std::string luapath_;
	std::string name_;
	/* staticOnly=true 时只取字面列（不解析 $ 引用）——destroy 阶段用：
	 * 引用解析 read() 缺省实现是一次真实硬件事务，且对已释放变量会抛错，
	 * 让器件的去初始化被静默跳过（F-04）。 */
	std::function<Json(bool staticOnly)> getMetadata_;
	/* controller 按名在注册表里查找（R-3）：声明序保证「先于本变量注册、
	 * 后于本变量销毁」，查找式引用把这一约定变成运行期可校验的。 */
	std::string ctrlName_;
	HwRegistry* registry_;

	explicit MtVars_Lua(lua_State* L, std::string luapath, std::string name,
			std::function<Json(bool)> getMetadata, std::string ctrlName, HwRegistry* registry)
		: L_(L), luapath_(std::move(luapath)), name_(std::move(name)),
		  getMetadata_(std::move(getMetadata)), ctrlName_(std::move(ctrlName)),
		  registry_(registry) {}

	Json callImpl(const Json& json);
	Json readImpl();
	void riseImpl();
	void freeImpl();

	/* C-1 防线：@Action 参数键与结构元数据键必须互斥（见 callImpl 注释） */
	void rejectReservedKeys(const Json& metadata, const Json& params) const;

	static bool build_lua(lua_State* L, const std::string& luapath);

public:
	~MtVars_Lua() override {
		/* 防御：未成功销毁时（构造后/异常路径）不再泄漏状态机 */
		if (L_) { lua_close(L_); }
	}

	Json play(const Json& json) override { return callImpl(json); }
	Json read() override { return readImpl(); }
	void build() override { riseImpl(); }
	void destroy() override { freeImpl(); }

	static std::unique_ptr<MtVars_Lua> Factory(const std::string& luapath, const std::string& name,
			std::function<Json(bool)> getMetadata, std::string ctrlName = "", HwRegistry* registry = nullptr);
};

} // namespace bot