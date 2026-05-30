-- 接收一个参数（表），返回另一个表
function process(data)
    local name = data.name
    local age = data.age
    local city = data.city
    
    -- 处理逻辑
    -- 修改：将 %d 改为 %s，因为 age 实际传入的是字符串
    local result_message = string.format("Hello %s, age %s from %s", name, age, city)
    
    -- 返回字典
    return {
        result = "success",
        message = result_message,
        code = 200
    }
end

-- 脚本主体：接收传入的参数，调用处理函数，返回结果
-- 注意：传入的参数在 ... 中（可变参数）
local input = ...  -- 获取第一个参数（我们的字典）
return process(input)
