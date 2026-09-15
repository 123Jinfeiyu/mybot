#pragma once
/* luaend.hpp —— Lua 状态机辅助（移植 bot4cj/src/HV/Lua.cj 的公共部分）
 * 错误串、全局类型探测、沙箱、Json↔Lua 递归互转、call_lua 单入口。
 */

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include <string>

#include "json.hpp"

namespace bot {

/* 读取 Lua 栈顶错误对象为字符串（判型防空指针） */
std::string luaErrorString(lua_State* L);

/* 探测全局变量类型（getglobal + type + pop，栈深不变） */
int luaGlobalType(lua_State* L, const char* name);

/* 收紧 Lua 沙箱：中和 os.execute/exit/remove/rename、io.popen/open/lines/input/output */
void sandbox_lua(lua_State* L);

/* 将 Json 递归压入 Lua 栈（数组/对象转 Lua 表）；返回后栈顶即该值 */
void push_lua_value(lua_State* L, const Json& val);

/* 将 Lua 栈上 idx 处的值递归转为 Json（int→字面整 判 Int，否则 Float） */
Json lua_value_to_json(lua_State* L, int idx);

/* 调用 Lua 脚本的 mode 全局函数，传入表中字段；返回 转换后的结果表。
 * pcall 失败带 ERR 契约返回，并打印 Lua 错误详情。调用前后栈深不变。 */
Json call_lua(lua_State* L, const Json& obj);

} // namespace bot