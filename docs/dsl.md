# DSL 参考：结构脚本与动作脚本

> 本文是 bot4cj 两类 DSL 的完整参考：结构脚本 `.sql` 声明硬件结构与调用链，动作脚本 `.mt` 以行列式 DSL 编排动作。[README](../README.md) 只留入口链接，语法细节以本文为准。

## 结构脚本（.sql）

由内置 SQL4cj 解析器（`src/sqlparse.cpp`）解析，支持 `--` 行注释与 `/* */` 块注释。一组 `CREATE TABLE / INSERT INTO`：每张表对应一类硬件（表名即矩阵名，决定加载哪个驱动，`modules/<矩阵名>.lua`），每行 INSERT 就是一个实例：

```sql
CREATE TABLE I2C (name String, path String);
INSERT INTO I2C (name, path) VALUES ('i2c-1', '/dev/i2c-1');

CREATE TABLE PCA9685 (name String, I2C String, address String);
INSERT INTO PCA9685 (name, I2C, address) VALUES ('pca9685-0', '$i2c-1', '0x40');

CREATE TABLE Servo (name String, max String, min String, offset String,
                    controller String, channel String);
INSERT INTO Servo (name, max, min, offset, controller, channel)
  VALUES ('s1', '90', '-', '-', 'pca9685-0', '0');   -- 左下水平关节
```

要点：

- `-` 是占位符，继承上一行同列的值
- `I2C` 列以 `$i2c-1` 引用上方定义的总线（引用列必须 `$` 前缀，裸名一律按字面值）
- 注释整行 INSERT 即可下线一个硬件实例；报错会指明是哪张表、哪一行、哪一列
- 列类型支持 `String` / `Number`：值一律以文本驻留（`MatrixObject` 只存 String），`Number` 目前仅在**解析期**对字面量做一次数值校验（整数/小数/带符号/科学计数法合法，空值与 `nan`/`inf` 拒绝），不做运行期类型转换；校验按 INSERT 的列名匹配声明列，报错形如 `Column 'delay' of table 'Timer' is declared Number, but value 'abc' is not numeric.`

**引号字面量**：`'` / `"` 包裹的值整体按字面解析——引号内的 `--`、`/* */`、`;`、`,`、`(`、`)` 与空白都是**数据**而非注释/分隔符（注释只在引号外生效）。这是 1.x 起修复的词法器行为，有负值回归守门（`robots/test/sqlquote.sql`）。

## 动作脚本（.mt）

用一张表格描述一帧一帧的动作，表头是「硬件变量.参数」，每行一帧，逐行播放：

```mt
# 行动矩阵为硬件变量输入参数
@Action
s1.A  s2.A  spk.F  spk.delay;   # column name
  45    45     10      200;      # stand
  90     -      -       20;      # left-behind
@
```

- 可内联 `@Alias` 别名矩阵（如把 `s1.A` 写成 `s1.angle`）；`@Vars` 命名数值变量同构（列 `name`/`value`），也可内联
- 单元格数值可写 `$(<表达式>)`（见 [表达式](#表达式-)），如 `$(v1+v2+10)`；`-` 继承会沿用上一行的解析结果
- 结构与动作两侧都可定义别名/数值变量：`@Alias` 同名时**动作脚本的定义覆盖**（打印覆盖日志）；**`@Vars` 不允许覆盖**——它的数值经 `$`/`$(...)` 引用列进入硬件参数，动作脚本同名覆盖等价于改写结构定义（`.mt` 内联 `@Vars` 只能使用新名字，引用结构 Vars 读取不受影响）
- 同一动作脚本内重复定义同名别名（或数值变量）报错（负值契约）

## controller 引用（转交任务）

某个硬件收到指令后先干一份活，再把结果转交给另一个硬件：

1. 先运行自己的 Lua 驱动 `call`
2. 框架把「该行元数据（跳过 `name`/`controller` 列）∪ 自己 Lua 返回表」发给 `controller` 指定的硬件变量
3. `controller` 支持 `-` 继承上一行；被引用方必须在结构脚本中先定义

两点框架级行为需要知道：

- **派发目标 `mode` 由框架独占**（在合并参数之后才写入）：转发 payload 里混着上游返回表的键，
  上游驱动返回 `{mode="print"}` 之类的键不会改写下游的派发目标
- **驱动的 `build`/`call`/`destroy` 必须返回带 `result` 键的表**（`"OK"` 或 `"ERR"`）：
  返回非表或漏写 `return` 会被判契约违反并中断，不会被静默当成成功。
  `result`/`message`/`mode` 三个键名另由框架保留，`.mt` 动作脚本不得用它们作参数列
  （`MtVars_Lua::rejectReservedKeys`，即 `--help` 里「安全约束」一条的代码正身）。

反方向的调用编排（先把别人的结果收集进来，再调用当前零件）见下一节 [trigger 引用](#trigger-引用前置调用)。

**多列（扇出）**：controller 之外放行编号列 `controller2`/`controller3`…（基名或基名+纯数字
后缀；`controller2x` 仍是普通元数据列），一行可声明多个转发目标：

- 按**列声明序**逐个转发，每个 controller 收到**同一份** payload「元数据 ∪ 自己返回表」——
  同一行的多个 controller **不接力**（controller1 的结果不进 controller2 的输入）；
  跨变量链式转发仍然可用：目标自己的 controller 列会在它被调到时照常生效
- **出现一个 ERR 即停止并返回**：不再转发给其余 controller，异常上抛整帧中断（halt-on-error）
- 全部成功时当前零件的返回值 = **最后一个** controller 的结果（单列与既有行为一致；
  顶层回显打印的也是它）
- 同一行的 controller 列指向同一目标在装配期拒绝（同 payload 双转发是配置错误）；
  controller 目标也不能是 `@Alias`/`@Vars` 标量（装配期拒绝）

## trigger 引用（前置调用）

trigger 列是 controller 列的镜像：controller 是**出边**（当前零件调完后把结果传走），
trigger 是**入边**（当前零件被调用之前，先运行 trigger 列指向的硬件变量，把结果收集进当前零件的输入）：

1. 当前零件每次被派发（play）时，框架先以**空参数**调用 `trigger` 列指向的硬件变量——
   这是一次语义完整的 play：触发器自身的行元数据照常并入其调用参数，它若也带 controller 列则转发链照常走
2. 触发器返回表经 `ensureOk` 契约检查（`result="ERR"`/非表/空表即整帧响亮中断）
3. 返回表以**触发器名字为键的子表**并入当前零件的调用参数：`data.<触发器名> = {…}`
   （多触发器互不踩键；如触发 MPU6050 后驱动按 `data.imu0.ACCEL_X` 取六轴读数）
4. 再运行当前零件自己的 Lua `call`

```sql
CREATE TABLE I2C (name String, path String);
INSERT INTO I2C (name, path) VALUES ('i2c-1', '/dev/i2c-1');

CREATE TABLE MPU6050 (name String, I2C String, address String);
INSERT INTO MPU6050 (name, I2C, address) VALUES ('imu0', '$i2c-1', '0x68');

CREATE TABLE Servo (name String, max String, min String, offset String,
                    controller String, trigger String, channel String);
INSERT INTO Servo (name, max, min, offset, controller, trigger, channel)
  VALUES ('s1', '90', '-', '-', 'pca9685-0', 'imu0', '0');   -- 每次出腿前先触发一次 imu0
```

要点与边界：

- `trigger` 支持 `-` 继承上一行；**被引用方必须先声明**（与 `controller` 同规则）。
  这条约束顺带保证整个 trigger/controller 调用图按构造无环——派发期无需环检测；
  `$` 式前向引用是数据引用列的专属能力，写在 trigger 列会被明确拒绝
- 触发器目标可以是 Lua 驱动变量或守护变量（守护的 `play` 返回最新采样快照，首帧未出时
  `count=0` 不是错误）；`@Alias`/`@Vars` 标量不能当触发目标（装配期拒绝——触发器经
  play 调用，必须返回结果表）
- 触发器只挂在 play 路径（含 controller 转发的嵌套 play 与 agent 的 `playQuiet`）；
  `read()`/`build`/`destroy` 一律不触发
- 触发器名不得与本表元数据列同名、不得用框架保留键 `result`/`message`/`mode`
  （装配期拒绝）；`.mt` 参数列不得占用触发器名（派发期 `rejectReservedKeys` 拒绝）
  ——触发器结果子表按触发器名并入调用参数，这些名字因此成为调用参数里的保留键
- 触发器数据在元数据与动作参数之后并入；controller 转发的 payload = 元数据 ∪ 自己返回表，
  **不含触发器子表**——输入侧数据要继续转发，须由驱动在返回表里显式回显
- 触发是同步调用：N 个设备都挂同一触发器 = 每帧 N 次触发调用，无帧内去重；
  高频采样场景请改用守护变量 + `$` 引用读快照，触发器留给「每次执行前必须新鲜测量/先干一件事」的编排

**多列**：trigger 之外放行编号列 `trigger2`/`trigger3`…（命名规则与 controller 相同），
一行可声明多个触发器，按**列声明序**逐个前置调用——某个触发器 ERR 即停止，其后的触发器
与当前零件都不再执行；返回表各自以触发器名字为键并入，互不踩键。同一行的触发列指向
同一目标在装配期拒绝。

回归：`trig_basic`（正值 + 顺序/到位观察面专段）与 `trig_forward`/`trig_scalar`/
`trig_reserved`/`trig_collide`/`trig_dollar`/`trig_param`/`trig_n6`/`trig_err`（负值）；
多列 `ctrl2_basic`/`trig2_basic`（正值 + 观察面专段）与 `flow_dup`/`flow_dupc`/
`flow_fwd2`/`flow_scalar2`/`trig_scalar2`/`ctrl2_err`（负值）。

## `$` 引用列

元数据列可以按值引用另一个硬件变量：值以 `$` 开头（如 `$imu0`），被引用方的 `read()` 快照字段实时合并进本变量元数据；**不带 `$` 的裸名一律按字面值**。`$` 指向未定义变量在 **build 前**（全部表装配完成后）显式报错；指向**标量**（如 Alias 的 JsonString）也会清晰报错——标量引用请走 `controller` 列（唯一例外是 `@Vars` 数值变量，见下节）：

```sql
CREATE TABLE Monitor (name String, src String);
INSERT INTO Monitor (name, src) VALUES ('mon0', '$imu0');   -- 引用守护 imu0
```

值也可写成 `$(<表达式>)`：对 `@Vars` 做算术并把结果填入该列（完整语义见 [表达式](#表达式-)）。结构脚本里 `(` 是分隔符，必须整体加引号写成 `'$(...)'`：

```sql
INSERT INTO Servo (name, max, min, offset, controller, channel)
  VALUES ('s1', '$(smax+5)', '-', '-', 'pca9685-0', '0');
```

与声明顺序相关的三点行为：

- `$` **允许前向引用**（可以指向声明在后的变量）：存在性校验在全部表注册之后、任何驱动 `build`
  之前统一执行——与 `controller` 列不同，后者要求被指向的变量必须先声明
- `$` 的取值是**每次播放时**的实时快照，不是声明时的值
- 销毁阶段（`destroy`）取「字面列 + 引用列的尽力而为快照」：引用仍存活则并入其快照，
  已被销毁则**只跳过该引用**，本变量的 `destroy` 照样会被调用

### 接口器件（总线）必须声明在器件之前

总线句柄 `fd` 与从机 `address` 不是字面列，而是器件行经 `$i2c-N` / `$uart-N` 从 `I2C` / `UART` 表的 `call` 快照拿到的。因此 **`I2C` / `SPI` / `UART` 表必须声明在挂载其上的器件之前**：器件 `build` 要用 `fd`，而 `build` 按声明序执行——总线写在后面前向引用时，器件 `build` 会在 `Invalid Argument: fd or address` 上失败（`robots/test/ref_bus_forward.sql` 为正身负值用例）。

这条约束同时是**销毁期 `fd` 有效性的来源**：`fd` 只由接口器件自己的 `destroy` 关闭，而倒序销毁 + 上面的声明顺序 ⇒ 接口器件最后关闭 `fd`，所有挂在它上面的器件在 `destroy` 期间（关断/断电/写寄存器这类去初始化）`fd` 始终有效。`$` 引用列因此必须在 `destroy` 阶段解析——若你把器件去初始化写成依赖 `fd` 的形式，请务必保证总线表在器件表之前。

## `@Vars` 命名数值变量

`@Vars` 与 `@Alias` 同构（列 `name` / `value`），但**仅支持 Number**：`value` 必须是数值字面量（整数/小数/带符号/科学计数法）或 `$(<表达式>)`（见 [表达式](#表达式-)），否则在注册期即报错。字面量按原文驻留、不做转换（与结构脚本 `Number` 列共用同一判定）；表达式在读取时求值。它有两种消费方式：

1. **结构脚本 `$` 数值常量**：元数据列写 `$<name>`，该列取变量的数值文本——这是唯一允许 `$` 指向标量的变量类型（`Alias` 等标量仍按上节报错）：

   ```sql
   CREATE TABLE Vars (name String, value Number);
   INSERT INTO Vars (name, value) VALUES ('v1', 10);
   INSERT INTO Vars (name, value) VALUES ('v2', 5);
   INSERT INTO Vars (name, value) VALUES ('smax', '$(v1+v2+75)');   -- 90
   CREATE TABLE Servo (name String, max String, min String, offset String,
                       controller String, channel String);
   INSERT INTO Servo (name, max, min, offset, controller, channel)
     VALUES ('s1', '$smax', '-', '-', 'pca9685-0', '0');   -- max 即 90
   ```

2. **动作脚本参数位替换**：沿用 `@Alias` 的规则，`dev.<name>` 的参数位用 `value` 替换成参数名（值被限定为 Number，故实际以第 1 种 `$` 常量用法为主）。`@Vars` 可在结构脚本定义，也可在 `.mt` 内联；**`.mt` 内联只允许新名字，与结构定义同名即报错**（防动作脚本改写结构硬件参数），同一动作脚本内重名同样报错。

与 `$` 对象快照不同，`@Vars` 不做字段合并，只把数值文本写到承载 `$` 的那一列；它无资源，`build`/`destroy` 为空操作。回归见 `vars_num`（正值）与 `vars_bad`/`vars_nonnum`/`vars_dup`/`vars_override`（负值）。

## 表达式 `$(...)`

`$(<expr>)` 在三处可用，语法一致：`+ - * / %`、括号、一元正负、十进制字面量、标识符（**只能是 `@Vars` 名**）。

1. `@Vars.value`：`INSERT INTO Vars ... VALUES ('v3', '$(v1+v2+10)')`（`.mt` 内联可裸写 `v3 $(v1+v2+10)`）
2. 结构脚本元数据列：`'$(v1*v2)'`（SQL 里必须整体加引号，否则 `(` 触发切词）
3. `@Action` 单元格：`$(v3*2)`

规则与边界：

- **仅算术**：无函数调用/赋值；标识符只能指向 `@Vars`（数值），引用 `Alias`/驱动变量会清晰报错。
- **惰性求值**：读取时才解析标识符，故支持**前向引用**；成环（`a=$(b+1)`、`b=$(a+1)`）由线程局部深度计数拦下并报 `circular @Vars`。`.mt` 内联 `@Vars` 不得覆盖结构定义（见上节），因此动作脚本无法借此改写结构侧表达式的取值。
- **存在性校验**：元数据列表达式里的标识符与 `$name` 一样，在全部表注册后、任何驱动 `build` 之前统一校验（允许前向引用）；`.mt`/`@Vars` 侧的引用在首次读取（build/首帧）时报错。
- **除零/取模为零** 明确报错；表达式结果以 `%.15g` 格式化回写文本（整数不带小数点）。
- **空格**：矩阵按空白切格，`$(...)` 内不得含空格；SQL 引号形式内空白是数据，可以写 `$(v1 + v2)`。
- **深度上限**：括号嵌套（`$(((...)))`）与平铺运算符链（`1+1+1+…`，左深树的求值递归深度等于链长）分别设上限，超限都在解析期拒绝，不会打爆宿主栈。
- 回归：`expr_num`（正值，三处都覆盖）与 `expr_undefined`（负值）。

## mtmerge 合成器（.mt 两两归并）

`mtmerge` 是随仓库构建的**离线编译目标**（`./build/mtmerge`），把设备集不相交的多个 @Action 脚本静态合并为一个，产物是规范 .mt 文本：

```bash
./build/mtmerge [--phase=N] [--out merged.mt] a.mt b.mt [c.mt ...]
cat merged.mt | ./build/mybot --robot=robots/spider1.sql   # 产物直接回喂
```

- **冲突裁决（静态）**：列头 `<device>.<param>` 切出的 device 名出现在两个输入中即报错退出 2——同设备不同参数列（`s1.A` vs `s1.speed`）也算冲突，不做参数并集。
- **行相位 `--phase=N`**：第 2 个（及后续每个）输入相对已合并结果整体滞后 N 行（负值提前，缺省 0）。N 个输入按序两两归并：先合 a+b，结果再合 c。仅作用于 `@Action`。
- **耗尽策略**：较短一方行数耗尽后补字面 `-`——继承链自然延伸，保持最后姿势直到另一方播完。
- **未起拍**：N>0 时后者开头 N 行落在前者时间轴之前，这些行填 `-`；派发时该设备整行未起拍会被**跳过**，不会以 `0.0` 误发一次指令（首数据行即 `-` 的 0.0 数值语义只对显式书写生效）。
- **负相位丢内容告警**：N<0 把后者整体上移，其开头 |N| 行落到输出窗口之外而被丢弃；mtmerge 在 stderr 告警，|N| ≥ 行数时提示「全部内容」——静默丢动作不可接受。
- **列序**：= 前者的全部列（原序） ∪ 后者的全部列（原序）。
- **`@Alias` / `@Vars` 归并**：不做相位，按行**叠加**（各输入的 data 行依次拼接）；两侧列头必须一致，同名行（`name` 列）报错退出 2。产物顺序为 `@Alias`、`@Vars`（若存在）、`@Action`。

语义边界（务必知悉）：

- 合并产物仍是**单时间轴**：同一合并行内前者设备先 play、后者后 play，前者行内 `Timer.delay` 会推迟后者同行设备。相位的正确用法是把长 delay 的行与对方的保持行错开。
- 注释与空行不保真（`MatrixParser` 不保留），产物为规范化文本；`-` 保持行在源脚本占一拍，合并后仍占一拍，拍数语义不变。
- 每列只属于一个源脚本，列内 `-` 继承链在合并产物中逐一保持——任何一方单独跑的结果与合并跑的结果一致（设备不相交保证）。

## `@Include` 内联归并（运行端动作脚本合并）

mtmerge 的离线归并在运行端的等价物：主脚本用一张 `@Include` 矩阵声明要并入的 `.mt`，mybot 解析主脚本后**就地展开**，后续注册与派发与手写大脚本完全一致。归并内核与 mtmerge 共用同一份代码（`mtmerge_core`），冲突裁决与报错文案不漂移：

```
@Include
path phase;
  scripts/test/include_a.mt  1;
  scripts/test/include_b.mt  2;
@

@Action
timer-0.delay;
  200;
  200;
@
```

- **归并语义 = mtmerge**：`@Action` 两两归并（设备集相交报错、`phase` 滞后/提前、耗尽补 `-` 保持姿势、前导 `-` 未起拍跳过、负相位部分丢弃告警/全部丢弃报错）；`@Alias` / `@Vars` 按行叠加（列头一致 + 同名行报错）。主脚本没有 `@Action` 时首个 include 顶替累加器（其相位无对齐目标不生效，与 mtmerge「phase 作用于第 2 个及后续输入」同口径）。
- **`path` 列**：`.mt` 相对路径。词法规则：拒绝绝对路径、`..` 分量、空段与 NUL，只放行 `.mt`；`O_NOFOLLOW` 拒绝符号链接。
- **基准目录（按输入通道）**：`--input` 文件通道 = 脚本所在目录；agent 工具 `run_action_script` = `--scripts-dir`；stdin / TCP 无脚本位置 = 进程当前工作目录。上例的 `scripts/test/...` 前缀即 stdin 场景（从仓库根运行）。
- **`phase` 列**（可选，缺省 0）：整数，范围 ±100000（与 mtmerge `--phase` 同界）；写 `-` 继承上一行的相位（首行 `-` = 0）。
- **硬限制**：`@Include` 数据行 ≤ 32、被包含文件字节总量 ≤ 8 MiB、单文件 ≤ 8 MiB。主脚本不含 `@Include` 时行为逐字不变（fast path）。
- **深度限 1**：被包含文件里再出现 `@Include` 即报错——递归包含需要环检测与深度记账，链式主脚本可表达同样的组合。
- **信任边界（务必知悉）**：`--tcp` 下发的动作脚本此前无法触达本地文件，`@Include` 是第一个例外——远端脚本可让宿主读取**基准目录树内**的 `.mt` 文件并作为动作执行。词法规则与字节预算把读取面限制在该树内、总量有界，且文件内容不进报错文本（无从借报错探测）；但对公网暴露 `--tcp` 时仍应意识到这一新增的本地读取面。
