# 仿真器（sim/）

> 3D 蜘蛛机器人仿真器：GLFW/OpenGL 可视化从端，经三条 AF_UNIX 行文本 socket
> 接入 mybot 链路。本文是三条桥的协议、成败口径与架构约定的单一事实源；
> 逐字实现见 `sim/src/`（C++ 侧）与 `modules/Sim.lua` / `modules/Camera.lua`（驱动侧）。

## 是什么、不是什么

- **是**：可视化从端 + 感知源。上游只下发**语义动作**（前进/转向/停/拾取/放置），
  蜘蛛步态由仿真器按身体速度自己生成，关节角不过桥——这让 `.mt` 动作脚本与
  agent 工具层无需了解形态学。
- **不是**：物理引擎。平面运动学（一阶滞后逼近目标速度）+ 球-AABB 碰撞推出，
  无足地接触、无 IK；搬运减速只是「质量越大越慢」的折算公式。
- **刻意缺席即成功**：命令桥的客户端（动作脚本）不该被一个调试窗口绑架，
  仿真器没开 = 命令静默丢弃（`BOT4CJ_DEBUG=1` 可见）；感知桥（拍照/测距）相反，
  抓不到就响亮 ERR——静默返回假数据比报错危险。

## 构建与运行

```bash
bash scripts/build_sim.sh        # 构建 + 自动跑 simcore 单测（ctest）
./sim/build/mybot-sim            # 监听三条缺省路径的 socket
```

独立 CMake 工程，刻意不被根 CMakeLists 引用（CI 五条 job 全 target 构建，
镜像无 GL/X11、musl 编不过 GLFW）。依赖经 FetchContent 拉 glfw 3.4 + glm 1.0.1，
仓库自带 glad 与 stb。

目标分两层：**simcore**（GL 无关：运动学/场景交互/三桥，可单测）与
**mybot-sim**（GLFW/OpenGL 壳）。`sim/tests/` 是 simcore 的单元测试（无 GL），
`test/test_sim.lua` 从 mybot 侧守门 Sim 驱动的 ERR 契约与线上协议。

## 三条 socket

路径解析三桥同规矩：**argv > 环境变量 > 缺省**；环境变量名是 mybot 侧与仿真器
侧的单一事实源，两侧同读。缺省路径的 C++ 侧单一事实源是 `sim/src/sim_config.h`
的 `kDefault*SocketPath`（main.cpp 不再各写字面量）；Lua 侧 `Sim.lua` 也硬编码了
命令/测距两条缺省路径——两套构建系统间不做代码生成，改由 `sim_tests` 的
`testSharedConstants` 运行期读 `Sim.lua` 交叉校验字面量一致，任一侧漏改即红
（拍照路径 Camera.lua 只取自 env、无硬编码缺省，不参与校验）。

| 桥 | 环境变量 | 缺省路径 | 语义 |
| --- | --- | --- | --- |
| 命令 | `MYBOT_SIM_SOCK` | `/tmp/mybot-sim.sock` | 持续型即发即忘 + 一次性事件 |
| 拍照 | `MYBOT_SIM_CAPTURE_SOCK` | `/tmp/mybot-sim-capture.sock` | 一问一答，回 JPEG 二进制体 |
| 测距 | `MYBOT_SIM_SENSOR_SOCK` | `/tmp/mybot-sim-sensor.sock` | 一问一答，回毫米整数 |

分三条的原因：Sim.lua 与 Camera.lua 是各自独立的驱动实例（各自的 lua_State /
各自的 fd），物理上无法共用一条连接；语义上持续型命令与请求-应答也不同构。

### 命令桥（`sim/src/bridge.cpp`）

一行一条指令，`\n` 结尾，速度缺省 1.0：

```
forward | backward | left | right | stop   [速度 0.0~1.0]
fdist <meters>      # 相对前进 N 米后自动停（负=后退）
turn <degrees>      # 相对转向 N 度后自动停（正=左，负=右）
pick                # 拾取前方 0.6m 内最近障碍
place               # 把搬运物放到前方 0.5m 处
save                # 把当前场景写回配置的场景文件（见「场景文件」）
```

指令分两类，**别混**：

- **持续型**（forward/backward/left/right/stop）：像油门位，保持到下一条持续型
  指令为止。新持续型指令打断进行中的相对运动；`stop` 只是松油门，**不打断**
  相对运动（要取消请发一条持续型指令或新的 fdist 0）。
- **一次性**（fdist/turn/pick/place/save）：入队逐条触发，每条必执行一次。
  fdist 与 turn **顺序执行**（先走完再转，同帧连发也不互相吞），同通道新目标
  覆盖旧目标；到量前提前松油门刹车，实际位姿贴着目标（不再有减速尾程的
  滑出）。队列上限 32（防灌事件），控制器断开时未执行事件作废、持续型指令
  回落 stop（没人管的蜘蛛不该继续走）。

`.mt` 动作脚本里用**数字动作码**（矩阵单元格只解析 Float64）：

```
0=stop  1=forward  2=backward  3=left  4=right
5=fdist(配 param=米)  6=turn(配 param=度)  7=pick  8=place  9=save
```

动作分类学（码↔线上名↔持续/一次性↔是否带参）的**单一事实源是
`sim/src/action_table.h`**：命令桥查表分派（不再手写 if-else 字符串链），
`sim/tests` 交叉校验 `modules/Sim.lua` 的 `CODES`/`PARAM_CODES`/`CODE_MAX`
与表一致——加动作只需在表里追加一行，漏改 Lua 会被测试挡下（数字码是对外契约，
`.mt`/`.sql`/本文都用它，**只增不改**）。

手测：`socat - UNIX-CONNECT:/tmp/mybot-sim.sock` 然后敲 `forward 0.5` / `stop`。

### 拍照桥（`sim/src/capture.cpp`）

请求 `capture\n`；响应先文本头一行、紧跟二进制体：

```
OK jpeg <len> <w> <h>\n   + <len> 字节 JPEG
ERR <reason>\n
```

抓的是本帧渲染结果（SwapBuffers 前的 GL_BACK，垂直翻正后 stb 编码，质量 90）。
mybot 侧归 `modules/Camera.lua` 的 sim-live 图源（`capture_image` 工具）。
手测：`printf 'capture\n' | socat - UNIX-CONNECT:/tmp/mybot-sim-capture.sock | head -c 64`。

### 测距桥（`sim/src/sensor.cpp`）

请求 `range\n`；响应 `OK <mm>\n` 或 `ERR <reason>\n`。距离是身体前端中心沿
heading 的射线与障碍盒的最近交点，无命中返回 10.0m 上限；被搬运的物体不计入。
mybot 侧两个消费者：`Sim.lua` 的 `range()`（动作脚本可用）与 C++ 侧
`querySimRange()`（`capture_image` 时把距离附进照片描述）。
手测：`printf 'range\n' | socat - UNIX-CONNECT:/tmp/mybot-sim-sensor.sock`。

## 三桥共同的行为约定（`sim/src/unix_line_server.cpp`）

- **单线程**：无独立线程，主渲染循环每帧 `poll()` 一次；recv 非阻塞绝不卡画面，
  发送侧带 3s 绝对 deadline（对端连上不读就断开，绝不无限阻塞）。
- **单客户端、新顶旧**：新连接直接顶掉旧连接（对端每次请求都可能重连）。
- **EOF 延迟断开**：对端关写端（socat 手测「发完即关」模式）时先把已到的行
  全部派发、答复发出后再断开——请求不允许被 EOF 连带清掉。
- **垃圾有界**：攒够 1024/4096 字节还没换行就断开，不无界吃内存；认不出的
  指令整行忽略并告警，**不改当前状态**（持续型指令被垃圾打成 stop 是事故）。
- SIGPIPE 一律 `MSG_NOSIGNAL` 折成 EPIPE 处理；上次异常退出留下的 socket inode
  先 unlink 再 bind。

## 场景文件（环境与代码解耦）

障碍布局是**数据不是代码**：`sim/scenes/default.scene`（行文本，与三桥协议同
风格，可手编可 diff）。C++ 里的 `World::defaultScene()` 只是无场景文件时的内置
回落，两者保持同值。

```
mybot-sim scene v1            ← 首行魔数，缺了整文件拒绝
# box: cx cy cz hx hy hz r g b [mass]，mass 缺省=体积×800kg/m³
box 2.0 0.40 1.2 0.40 0.40 0.40 0.72 0.30 0.24 61.44
# robot: x z theta_deg [carried_idx]，carried 恢复搬运状态
robot 0 0 0
```

注释（`#`）与空行忽略，未知/坏行告警跳过。`sim/tests` 有 round-trip 与容错
解析的守门。

**路径解析**：`argv[4] > env MYBOT_SIM_SCENE > 内置默认`（空=内置）。文件读失败
回落内置并告警，不开黑屏世界。

**保存**：`S` 键或命令桥 `save` 指令（动作码 9）把当前世界状态（障碍位置 +
机器人位姿 + 搬运状态）**原子写回**（先落 `.tmp` 再 rename）启动时配置的场景
文件。线上不接受路径参数——只写本进程启动时定下的那一个文件，与 Camera.lua
「杜绝连任意 socket」同源的安全口径。`sim_agent_demo.sh` 缺省把
`sim/scenes/default.scene` 传给 sim，agent 搬动过的环境可以经 save 存下来、
下次续用。

**复位**：`R` 键从场景源头整体重载（文件版重读文件、内置版回默认），机器人
位姿与步态一并复位——被搬走的箱子能回来了。

## 架构分层

```
simcore（GL 无关，可单测/可上 CI）        mybot-sim 壳
├─ sim_engine.cpp  引擎：拥有 Robot+World+场景会话，  ├─ main.cpp  薄壳：主循环/输入/HUD
│                  每帧推进 + 指令派发（可 headless 测）├─ spider.cpp 四足外观步态
├─ robot.cpp    平面运动学+相对运动         ├─ mesh/shader/camera
├─ world.cpp    障碍场景/碰撞/测距/搬运     └─ framegrab.cpp glReadPixels→JPEG
├─ action_table.cpp  动作分类学单一事实源
├─ sim_config.h   全局可调常量 + 缺省 socket 路径（C++ 侧 SSOT）
├─ unix_line_server.cpp  三桥公共骨架
├─ request_bridge.cpp  请求-应答桥公共基类（一问一答）
├─ bridge.cpp   命令桥（状态+事件队列，查 action_table 分派）
├─ capture.cpp  拍照桥（RequestResponseBridge 子类，只补二进制体帧化）
└─ sensor.cpp   测距桥（RequestResponseBridge 子类，只补文本响应帧化）
```

**SimEngine**（`sim/src/sim_engine.cpp`）是仿真逻辑的落点：拥有 `Robot` + `World` +
场景路径，暴露 `reloadScene/saveScene`、`applyContinuous(cmd,speed)`、`applyEvent(ev)`、
`step(dt)`、`queryRange()`。此前这些是 `main.cpp` 里的自由函数与 lambda，困在 GLFW 壳中
无法单测；抽进 simcore 后，fdist 到量、pick/place、save/reload 都能在 `sim/tests` 里
headless 验证。壳只管窗口/输入/渲染/HUD，每帧把命令桥的持续型状态与一次性事件喂给
引擎、调 `step(dt)`，再读 `robot()/world()` 渲染——蜘蛛外观步态（`Spider`）是可视化，
仍留在壳里按 `robot().v()` 自己生成。

`Robot` 只管运动学（`update(dt, input, maxSpeed)`，maxSpeed 由 World 按搬运负载
折算后传入）；「世界里有什么、撞了怎么解、搬得动什么」全在 World。协调坐标系：
机体局部 +X 前、+Y 上、+Z 右，theta 增大 = 左转。

## 端到端链路

`scripts/sim_agent_demo.sh` 起三层：sim + `mybot --mcp-backend` + `mybot-agent`，
任务提示词从 `prompts/sim_obstacle_nav/`（避障探索）或 `PROMPTS_DIR` 指定目录
按序号拼接；搬运任务提示词见 `prompts/sim_carry_task.md`（配
`scripts/sim_pick.mt` / `sim_place.mt`）。退出时自动拆链路清 socket，日志保留。
