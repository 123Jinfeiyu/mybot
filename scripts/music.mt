# 小星星——Speaker 声乐音调示例
#
# 音调按 MIDI 号直写（.mt 数据格原生支持）：60=中央C(do)，67=G4(sol)，69=A4(la,440Hz)
# 休止符：wave 是文本参数，数据格只认数值，文本要经 $(别名) 间接——
#   @Alias 定义文本值，数据格写 $(rest) 即得 "silence"（静音占时，幅值零）
# wave 列的 '-'：首行=不投递（落到结构脚本缺省 sine），其后=继承上一行

@Alias
name   alias;
sin    sine;
rest   silence;
@

@Action
spk0.wave  spk0.note  spk0.delay;
 -               60         350;	# do
 -               60         350;	# do
 -               67         350;	# sol
 -               67         350;	# sol
 -               69         350;	# la
 -               69         350;	# la
 -               67         700;	# sol——长音
 $(rest)         60         200;	# 休止（note 照给过校验，静音幅值零）
 $(sin)          65         350;	# fa
 -               65         350;	# fa
 -               64         350;	# mi
 -               64         350;	# mi
 -               62         350;	# re
 -               62         350;	# re
 -               60         700;	# do——长音
 $(rest)         60         300;	# 尾声休止
@
