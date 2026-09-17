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

- 可内联 `@Alias` 别名矩阵（如把 `s1.A` 写成 `s1.angle`）；`@Vars` 命名文本变量同构（列 `name`/`value`），也可内联
- 结构与动作两侧都可定义别名/文本变量，同名时**动作脚本的定义覆盖**（打印覆盖日志）
- 同一动作脚本内重复定义同名别名（或文本变量）报错（负值契约）

## controller 引用（转交任务）

某个硬件收到指令后先干一份活，再把结果转交给另一个硬件：

1. 先运行自己的 Lua 驱动 `call`
2. 框架把「该行元数据（跳过 `name`/`controller` 列）∪ 自己 Lua 返回表」发给 `controller` 指定的硬件变量
3. `controller` 支持 `-` 继承上一行；被引用方必须在结构脚本中先定义

转发 payload 里混着上游返回表的键，故派发目标 `mode` 由**框架独占**（在合并参数之后才写入）——上游驱动返回 `{mode="print"}` 之类的键不会改写下游的派发目标。驱动的 `build`/`call`/`destroy` 必须返回带 `result` 键的表（`"OK"` 或 `"ERR"`）：返回非表或漏写 `return` 会被判契约违反并中断，而不会被静默当成成功。`result`/`message`/`mode` 三个键名另由框架保留，`.mt` 动作脚本不得用它们作参数列（`MtVars_Lua::rejectReservedKeys`，即 `--help` 里「安全约束」一条的代码正身）。

## `$` 引用列

元数据列可以按值引用另一个硬件变量：值以 `$` 开头（如 `$imu0`），被引用方的 `read()` 快照字段实时合并进本变量元数据；**不带 `$` 的裸名一律按字面值**。`$` 指向未定义变量在 **build 前**（全部表装配完成后）显式报错；指向**标量**（如 Alias 的 JsonString）也会清晰报错——标量引用请走 `controller` 列（唯一例外是 `@Vars` 文本变量，见下节）：

```sql
CREATE TABLE Monitor (name String, src String);
INSERT INTO Monitor (name, src) VALUES ('mon0', '$imu0');   -- 引用守护 imu0
```

顺序说明：`$` **允许前向引用**（可以指向声明在后的变量）——存在性校验在全部表注册之后、任何驱动 `build` 之前统一执行；与 `controller` 列不同（后者要求被指向的变量必须先声明）。另有两点与顺序相关的行为：`$` 的取值是**每次播放时**的实时快照（不是声明时的值）；销毁阶段（`destroy`）取「字面列 + 引用列的尽力而为快照」——引用仍存活则并入其快照，已被销毁则**只跳过该引用**（本变量的 `destroy` 照样会被调用）。

### 接口器件（总线）必须声明在器件之前

总线句柄 `fd` 与从机 `address` 不是字面列，而是器件行经 `$i2c-N` / `$uart-N` 从 `I2C` / `UART` 表的 `call` 快照拿到的。因此 **`I2C` / `SPI` / `UART` 表必须声明在挂载其上的器件之前**：器件 `build` 要用 `fd`，而 `build` 按声明序执行——总线写在后面前向引用时，器件 `build` 会在 `Invalid Argument: fd or address` 上失败（`robots/test/ref_bus_forward.sql` 为正身负值用例）。

这条约束同时是**销毁期 `fd` 有效性的来源**：`fd` 只由接口器件自己的 `destroy` 关闭，而倒序销毁 + 上面的声明顺序 ⇒ 接口器件最后关闭 `fd`，所有挂在它上面的器件在 `destroy` 期间（关断/断电/写寄存器这类去初始化）`fd` 始终有效。`$` 引用列因此必须在 `destroy` 阶段解析——若你把器件去初始化写成依赖 `fd` 的形式，请务必保证总线表在器件表之前。

## `@Vars` 命名文本变量

`@Vars` 与 `@Alias` 同构（列 `name` / `value`），存放**命名文本值**。SQL 解析器只支持 String，故值一律按文本保存。它有两种消费方式：

1. **结构脚本 `$` 文本常量**：元数据列写 `$<name>`，该列取变量的文本值——这是唯一允许 `$` 指向标量的变量类型（`Alias` 等标量仍按上节报错）：

   ```sql
   CREATE TABLE Vars (name String, value String);
   INSERT INTO Vars (name, value) VALUES ('i2cpath', '/dev/i2c-1');
   CREATE TABLE I2C (name String, path String);
   INSERT INTO I2C (name, path) VALUES ('i2c-1', '$i2cpath');   -- path 即 /dev/i2c-1
   ```

2. **动作脚本参数位替换**：与 `@Alias` 完全一致，`dev.<name>` 的参数位用值替换成真实参数名。`@Vars` 可在结构脚本定义，也可在 `.mt` 内联；同名时动作脚本覆盖结构定义，同一动作脚本内重名报错。

与 `$` 对象快照不同，`@Vars` 不做字段合并，只把文本写到承载 `$` 的那一列；它无资源，`build`/`destroy` 为空操作。回归见 `vars_text`（正值）与 `vars_bad`/`vars_dup`（负值）。

## mtmerge 合成器（.mt 两两归并）

`mtmerge` 是随仓库构建的**离线编译目标**（`./build/mtmerge`），把设备集不相交的多个 @Action 脚本静态合并为一个，产物是规范 .mt 文本：

```bash
./build/mtmerge [--phase=N] [--out merged.mt] a.mt b.mt [c.mt ...]
cat merged.mt | ./build/mybot --robot=robots/spider1.sql   # 产物直接回喂
```

- **冲突裁决（静态）**：列头 `<device>.<param>` 切出的 device 名出现在两个输入中即报错退出 2——同设备不同参数列（`s1.A` vs `s1.speed`）也算冲突，不做参数并集。
- **行相位 `--phase=N`**：第 2 个（及后续每个）输入相对已合并结果整体滞后 N 行（负值提前，缺省 0）。N 个输入按序两两归并：先合 a+b，结果再合 c。仅作用于 `@Action`。
- **耗尽策略**：较短一方行数耗尽后补字面 `-`——继承链自然延伸，保持最后姿势直到另一方播完。
- **列序**：= 前者的全部列（原序） ∪ 后者的全部列（原序）。
- **`@Alias` / `@Vars` 归并**：不做相位，按行**叠加**（各输入的 data 行依次拼接）；两侧列头必须一致，同名行（`name` 列）报错退出 2。产物顺序为 `@Alias`、`@Vars`（若存在）、`@Action`。

语义边界（务必知悉）：

- 合并产物仍是**单时间轴**：同一合并行内前者设备先 play、后者后 play，前者行内 `Timer.delay` 会推迟后者同行设备。相位的正确用法是把长 delay 的行与对方的保持行错开。
- 注释与空行不保真（`MatrixParser` 不保留），产物为规范化文本；`-` 保持行在源脚本占一拍，合并后仍占一拍，拍数语义不变。
- 每列只属于一个源脚本，列内 `-` 继承链在合并产物中逐一保持——任何一方单独跑的结果与合并跑的结果一致（设备不相交保证）。
