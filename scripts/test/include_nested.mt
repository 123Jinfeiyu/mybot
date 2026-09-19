# 负值契约：被包含文件里再出现 @Include（include_inner.mt 含 @Include）→ 拒绝嵌套
@Action
timer-0.delay;
  100;
@

@Include
path;
  scripts/test/include_inner.mt;
@
