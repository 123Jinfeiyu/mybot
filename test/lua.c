#include <stdio.h>
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

int main() {
    printf("========== Lua Type Constants ==========\n");
    printf("LUA_TNONE           = %d\n", LUA_TNONE);
    printf("LUA_TNIL            = %d\n", LUA_TNIL);
    printf("LUA_TBOOLEAN        = %d\n", LUA_TBOOLEAN);
    printf("LUA_TLIGHTUSERDATA  = %d\n", LUA_TLIGHTUSERDATA);
    printf("LUA_TNUMBER         = %d\n", LUA_TNUMBER);
    printf("LUA_TSTRING         = %d\n", LUA_TSTRING);
    printf("LUA_TTABLE          = %d\n", LUA_TTABLE);
    printf("LUA_TFUNCTION       = %d\n", LUA_TFUNCTION);
    printf("LUA_TUSERDATA       = %d\n", LUA_TUSERDATA);
    printf("LUA_TTHREAD         = %d\n", LUA_TTHREAD);
    
    printf("\n========== Testing lua_type() ==========\n");
    
    // 创建 Lua 状态机
    lua_State *L = luaL_newstate();
    
    if (L == NULL) {
        printf("Failed to create Lua state\n");
        return 1;
    }
    
    // 测试各种类型压栈后的 lua_type 返回值
    printf("\n1. 压入 nil:\n");
    lua_pushnil(L);
    printf("   lua_type(L, -1) = %d (应为 LUA_TNIL = %d)\n", lua_type(L, -1), LUA_TNIL);
    lua_pop(L, 1);
    
    printf("\n2. 压入 boolean (true):\n");
    lua_pushboolean(L, 1);
    printf("   lua_type(L, -1) = %d (应为 LUA_TBOOLEAN = %d)\n", lua_type(L, -1), LUA_TBOOLEAN);
    lua_pop(L, 1);
    
    printf("\n3. 压入 number:\n");
    lua_pushnumber(L, 3.14159);
    printf("   lua_type(L, -1) = %d (应为 LUA_TNUMBER = %d)\n", lua_type(L, -1), LUA_TNUMBER);
    lua_pop(L, 1);
    
    printf("\n4. 压入 string:\n");
    lua_pushstring(L, "hello");
    printf("   lua_type(L, -1) = %d (应为 LUA_TSTRING = %d)\n", lua_type(L, -1), LUA_TSTRING);
    lua_pop(L, 1);
    
    printf("\n5. 创建并压入 table:\n");
    lua_newtable(L);
    printf("   lua_type(L, -1) = %d (应为 LUA_TTABLE = %d)\n", lua_type(L, -1), LUA_TTABLE);
    lua_pop(L, 1);
    
    printf("\n6. 压入函数 (C 函数):\n");
    lua_pushcfunction(L, lua_gettop);
    printf("   lua_type(L, -1) = %d (应为 LUA_TFUNCTION = %d)\n", lua_type(L, -1), LUA_TFUNCTION);
    lua_pop(L, 1);
    
    // 测试无效索引
    printf("\n7. 测试无效索引:\n");
    printf("   lua_type(L, 100) = %d (应为 LUA_TNONE = %d)\n", lua_type(L, 100), LUA_TNONE);
    
    // 测试 lua_typename 函数
    printf("\n========== Testing lua_typename() ==========\n");
    printf("LUA_TTABLE 的类型名称: \"%s\"\n", lua_typename(L, LUA_TTABLE));
    printf("所有类型常量对应的名称:\n");
    printf("  LUA_TNONE           -> \"%s\"\n", lua_typename(L, LUA_TNONE));
    printf("  LUA_TNIL            -> \"%s\"\n", lua_typename(L, LUA_TNIL));
    printf("  LUA_TBOOLEAN        -> \"%s\"\n", lua_typename(L, LUA_TBOOLEAN));
    printf("  LUA_TNUMBER         -> \"%s\"\n", lua_typename(L, LUA_TNUMBER));
    printf("  LUA_TSTRING         -> \"%s\"\n", lua_typename(L, LUA_TSTRING));
    printf("  LUA_TTABLE          -> \"%s\"\n", lua_typename(L, LUA_TTABLE));
    printf("  LUA_TFUNCTION       -> \"%s\"\n", lua_typename(L, LUA_TFUNCTION));
    printf("  LUA_TUSERDATA       -> \"%s\"\n", lua_typename(L, LUA_TUSERDATA));
    printf("  LUA_TTHREAD         -> \"%s\"\n", lua_typename(L, LUA_TTHREAD));
    
    // 关闭 Lua 状态机
    lua_close(L);
    
    return 0;
}
