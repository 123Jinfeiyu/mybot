/* test_luavar.cpp —— @Action 参数与结构元数据的互斥契约（审计 #1 / C-1 防线）
 *
 * 动作参数与结构元数据最终合并成同一张 Lua 表交给驱动 call()。若 .mt 能写
 * `dev.note` / `dev.fd` / `dev.address` / `dev.max`，就等于动作脚本可以覆盖结构
 * 脚本定义的总线句柄、从机地址与舵机限幅——在 --tcp 模式下即远程改硬件拓扑。
 * 收口语义：结构元数据由 .sql 独占，.mt 只能引入新的参数键。
 *
 * 校验发生在 call_lua 之前，因此这些用例不需要任何 .lua 文件：
 * 放行路径会一路走到「未 build 的状态机」并由返回契约报错，据此区分两类错误。
 */

#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <string>

#include "harness.hpp"
#include "json.hpp"
#include "luavar.hpp"
#include "util.hpp"

using bot::Json;
using bot::JsonFloat;
using bot::JsonInt;
using bot::JsonObject;
using bot::JsonString;
using bot::MtVars_Lua;

namespace {

Json relayMetadata() {
	Json m = JsonObject();
	m.put("name", JsonString("relay0"));
	m.put("note", JsonString("hello-relay"));
	m.put("address", JsonInt(0x18));
	return m;
}

Json params(std::initializer_list<const char*> keys) {
	Json p = JsonObject();
	for (const char* k : keys) { p.put(k, JsonFloat(200.0)); }
	return p;
}

std::unique_ptr<MtVars_Lua> makeVar() {
	/* 第二参 staticOnly：destroy 路径只取字面列（F-04）——本夹具不需要区分。
	 * 无 controller：ctrlName/registry 走缺省值。 */
	return MtVars_Lua::Factory("./definitely-not-here-9f3a1c.lua", "relay0",
		[](bool) { return relayMetadata(); });
}

} // namespace

TH_TEST(action_param_may_not_shadow_structure_metadata) {
	auto v = makeVar();
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "is rejected because",
		v->play(params({"note"})));
	/* 报错要点名「哪个变量的哪一列」并说明原因，便于用户直接改 .mt */
	try {
		v->play(params({"note"}));
	} catch (const std::exception& e) {
		const std::string msg = e.what();
		TH_CHECK(msg.find("Hardware variable 'relay0'") != std::string::npos);
		TH_CHECK(msg.find("@Action column 'relay0.note'") != std::string::npos);
		TH_CHECK(msg.find("structure-script metadata column") != std::string::npos);
	}
	/* 结构元数据里的任意列都受保护，不只是 note */
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "'relay0.name'", v->play(params({"name"})));
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "'relay0.address'",
		v->play(params({"address"})));
}

TH_TEST(action_param_may_not_use_framework_reserved_keys) {
	auto v = makeVar();
	/* mode/result/message 由框架自己写入，动作脚本给同名键会污染契约判定 */
	for (const char* key : {"mode", "result", "message"}) {
		TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "framework-reserved key",
			v->play(params({key})));
	}
}

TH_TEST(action_param_with_a_new_key_is_allowed_through) {
	auto v = makeVar();
	/* 放行后一路走到未 build 的状态机，由返回契约报错——
	 * 关键是报错不得再是「保留键」那一类。 */
	std::string msg;
	try {
		v->play(params({"deg", "test"}));
	} catch (const std::exception& e) {
		msg = e.what();
	}
	TH_CHECK(msg.find("is rejected because") == std::string::npos);
	TH_CHECK(msg.find("Hardware call failed") != std::string::npos);
}

TH_TEST(empty_action_params_never_trip_the_guard) {
	auto v = makeVar();
	std::string msg;
	try {
		v->play(JsonObject());
	} catch (const std::exception& e) {
		msg = e.what();
	}
	TH_CHECK(msg.find("is rejected because") == std::string::npos);
}

TH_TEST(read_after_destroy_reports_instead_of_crashing) {
	/* 防御回归：状态机已释放时 read() 必须清晰报错，
	 * 而不是把空指针送进 LuaJIT（C1 环引用负路径实测暴露过的形态）。 */
	auto v = makeVar();
	/* 未 build 的变量全局 destroy 为 nil：跳过调用、释放状态机、不抛 */
	v->destroy();
	TH_CHECK_THROWS_MSG(bot::IllegalArgumentException, "Lua state not built", v->read());
	/* 二次销毁同样安全（幂等，不重复 lua_close） */
	v->destroy();
}
