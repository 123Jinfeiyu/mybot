-- print_table.lua - 调试打印辅助（modules/lib/）
-- bot4cj 仓颉语言机器人框架
--
-- 递归打印一张 Lua 表（原各驱动内联复制的同名函数收敛于此）。
-- 驱动顶部 dofile 加载；print 在调用时才查全局，故 sensor_daemon 沉默
-- print 后本函数的输出随之静默，行为与原内联版一致。

-- 重复加载保护：同一状态机内只定义一次
if print_table ~= nil then return end

function print_table(t, indent)
    indent = indent or 0
    for k, v in pairs(t) do
        if type(v) == 'table' then
            print(string.rep(' ', indent) .. k .. ":")
            print_table(v, indent + 2)
        else
            print(string.rep(' ', indent) .. k .. ": " .. tostring(v))
        end
    end
end
