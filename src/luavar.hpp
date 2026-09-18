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
	/* forDestroy=true 时走销毁期收集（MetaProvider::collectForDestroy）：
	 * 字面列 + 引用列的「尽力而为」快照——总线句柄 fd / 从机 address 正藏在
	 * 引用列里，去初始化（关断/断电/写寄存器）必须用它们；单个引用已释放时
	 * 只跳过该引用，不会让本驱动的 destroy 被跳过（F-04′）。 */
	std::function<Json(bool forDestroy)> getMetadata_;
	/* controller 按名在注册表里查找（R-3）：声明序保证「先于本变量注册、
	 * 后于本变量销毁」，查找式引用把这一约定变成运行期可校验的。
	 * N-6：首次转发时把目标指针缓存进 ctrl_（注册表会拒绝会话期覆盖
	 * controller 名字，指针终身有效），后续转发不再逐次查表。 */
	std::string ctrlName_;
	MatrixVars_Hardware* ctrl_ = nullptr;
	HwRegistry* registry_;

	explicit MtVars_Lua(lua_State* L, std::string luapath, std::string name,
			std::function<Json(bool)> getMetadata, std::string ctrlName, HwRegistry* registry)
		: L_(L), luapath_(std::move(luapath)), name_(std::move(name)),
		  getMetadata_(std::move(getMetadata)), ctrlName_(std::move(ctrlName)),
		  registry_(registry) {}

	Json callImpl(const Json& json, bool echo);
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

	Json play(const Json& json) override { return callImpl(json, true); }
	/* agent 模式：同一个 call 路径但不回显结果（stdout 只留最终回答） */
	Json playQuiet(const Json& json) override { return callImpl(json, false); }
	Json read() override { return readImpl(); }
	void build() override { riseImpl(); }
	void destroy() override { freeImpl(); }

	static std::unique_ptr<MtVars_Lua> Factory(const std::string& luapath, const std::string& name,
			std::function<Json(bool)> getMetadata, std::string ctrlName = "", HwRegistry* registry = nullptr);
};

} // namespace bot