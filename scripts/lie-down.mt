# 控制蜘蛛从标准站立动作逐渐躺下
#
@Alias
name   alias;
 FL0      s1;
 FL1      s2;
 FR0      s3;
 FR1      s4;
 BL0      s5;
 BL1      s6;
 BR0      s7;
 BR1      s8;
@
@Vars
name   value;
POS_S0     45;	# THE POSITION OF START
POS_S1      0;
INT_S1     10;
BEAT_T0    80;
@
@Include
phase path;
    0 scripts/move-fl.mt;
    0 scripts/move-fr.mt;
    0 scripts/move-bl.mt;
    0 scripts/move-br.mt;
@
@Action
spk.F   spk.delay;
   10  $(BEAT_T0);	# stand
    -           -;
    -           -;
    -           -;
    -           -;
    -           -;
@
