-- AgentFakeDriver.lua —— test/cpp/test_agentdriver.cpp 用的假 LLM 协议驱动
--
-- 不联网：把收到的规范化请求特征回显成 content，用来断言 C++ 侧的
-- ChatTransport 适配器确实把请求递到了驱动、并把驱动的响应解析回来。
-- model 传 "fail" 时走 ERR 契约，用于覆盖错误回传路径。

function build(data)
	return { result = "OK" }
end

function destroy(data)
	return { result = "OK" }
end

function call(data)
	if data.model == "fail" then
		return { result = "ERR", message = "假驱动故意失败" }
	end

	local nmsgs = 0
	for _ in ipairs(data.messages or {}) do nmsgs = nmsgs + 1 end
	local ntools = 0
	for _ in ipairs(data.tools or {}) do ntools = ntools + 1 end

	local first = (data.messages or {})[1] or {}
	return {
		result = "OK",
		content = string.format("msgs=%d tools=%d model=%s endpoint=%s first=%s",
			nmsgs, ntools, tostring(data.model), tostring(data.endpoint),
			tostring(first.content)),
		tool_calls = { { id = "c1", name = "list_hardware", arguments = "{}" } },
		extras = { reasoning_content = "fake-think" },
	}
end
