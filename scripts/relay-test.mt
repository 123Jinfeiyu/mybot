# controller 转发测试动作：
# relay0 先跑 Relay.lua（test → delay），框架再把「元数据(note) ∪ Lua返回(delay)」转发给 timer-0
@Action
relay0.test;
      200;
@
