# DSL 参考：结构脚本与动作脚本

> 本文是 bot4cj 两类 DSL 的完整参考：结构脚本 `.sql` 声明硬件结构与调用链，动作脚本 `.mt` 以行列式 DSL 编排动作。[README](../README.md) 中只保留速览。

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

- 可内联 `@Alias` 别名矩阵（如把 `s1.A` 写成 `s1.angle`）
- 结构与动作两侧都可定义别名，同名时**动作脚本的定义覆盖**（打印覆盖日志）
- 同一动作脚本内重复定义同名别名报错（负值契约）

## controller 引用（转交任务）

某个硬件收到指令后先干一份活，再把结果转交给另一个硬件：

1. 先运行自己的 Lua 驱动 `call`
2. 框架把「该行元数据（跳过 `name`/`controller` 列）∪ 自己 Lua 返回表」发给 `controller` 指定的硬件变量
3. `controller` 支持 `-` 继承上一行；被引用方必须在结构脚本中先定义

## `$` 引用列

元数据列可以按值引用另一个硬件变量：值以 `$` 开头（如 `$imu0`），被引用方的 `read()` 快照字段实时合并进本变量元数据；**不带 `$` 的裸名一律按字面值**。`$` 指向未定义变量在 build 期显式报错；指向**标量**（如 Alias 的 JsonString）也会清晰报错——标量引用请走 `controller` 列：

```sql
CREATE TABLE Monitor (name String, src String);
INSERT INTO Monitor (name, src) VALUES ('mon0', '$imu0');   -- 引用守护 imu0
```
