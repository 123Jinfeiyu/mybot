# 更新日志

> 从 [README](../README.md) 迁出，按主题分组记录版本演进要点，组内大致按时间倒序。
> 每条只留「改了什么、对使用者意味着什么」——设计论证、实现落点、守门明细（用例名单、
> 反向验证过程、当时的测试计数快照）不在此复述，以 docs/ 各文档、`bash test/run_all.sh`
> 与 `./build/mybot_tests` 的实时输出（`docs/development.md` 单一事实源约定）为准。
> 本文件压缩前的完整版本见 git 历史；提案文档（原 `docs/proposals/` 与 `.tmp/*` 计划
> 文件）已移出仓库，历史版本见 git（截至 `46f90d8`）。

## v1.x（C++ 移植版）

### 2026-10-04 守门脚本共用底座：抽取原语与协议出处路径收敛一处

- **新增共用底座** `scripts/lib/protocol_sources.py`（零编译）：把四道零编译守门脚本
  （第 8 段 `scripts/check_sim_contract.py`、第 10 段 `scripts/check_doc_paths.py`、
  第 11 段 `scripts/check_llm_field_whitelist.py`、第 12 段 `scripts/check_stateful_protocol.py`）
  里各自复制的文本抽取原语（`strip_comments` 去注释、`balanced_block` 取平衡括号块、
  `rel` 转仓内相对路径、`read` 读数与「找不到」文案）与协议出处路径常量清单收敛到一处；
  各脚本改为 import 并各留两行 `read` 包装，以沿用「脚本名: 找不到 <路径>」的失败文案，
  调用点零改动。
- **判据仍各写各的**：底座只放**机械部分**（怎么读源码）——什么算不一致、两侧该同集合
  还是该有差集、报错文案怎么写，仍留在各脚本里。这**不违反** `docs/testing.md` 那条
  「主干这几道独立实现、不共享代码」，因为那条说的是 **Python 守门与 `sim/tests/` 的
  C++ 同款校验不得共享**（共享实现等于共享同一个错误）：C++ 侧仍是另一套独立实现，两侧
  照旧交叉印证。判据逻辑不下沉，故底座改动只可能影响「怎么读」，不会让两侧一起改错。
- **纯重构、行为不变**：接入前先把四个检查器的 stdout 存为基线，接入后逐文件 `diff`
  **逐字节一致**（`strip_comments` / `balanced_block` 亦与旧实现做过逐字节等价核对）；
  `bash test/run_all_selftest.sh` 12 段全绿。守门段数与 `run_all.sh` 记账口径均未变。

### 2026-10-04 跨源守门第 12 段：有状态 LLM 协议判据双端互钉

- **新增第 12 段**（`scripts/check_stateful_protocol.py`，零编译）：模型服务端对**会话
  历史**的状态性要求（工具调用 id 必须在 assistant 声明与 tool 回执之间一一配对、
  DeepSeek 思考模式下带 `tool_calls` 的历史必须带 `reasoning_content`）在仓里手写两份
  ——Rust `agent/src/session.rs::protocol_violation`（`--resume` 恢复会话前最后一道判据）
  与 Python `scripts/fake_llm_server.py::protocol_error`（让假模型服务与真网关一样严）。
  两份各有一套单测，却**没有任何守门比对两份判据集**：夹具少一条判据时，产线会拒的
  历史在离线守门里照常全绿。脚本钉产线判据点条数与判据锚点（产线加/删判据必须同步
  用例表；反向验证过：临时插一条 `return Some(format!(...))` 即失败），并用 8 条用例
  （报文为 wire 形态）逐条验夹具的拒因归属。守门自检 **11 段 → 12 段**，仍不进
  `run_all.sh` 的记账，故「守门用例」汇总数不变。
- **夹具对齐产线**（`scripts/fake_llm_server.py`）：`protocol_error` 此前接受**空字符串**
  的 `tool_call_id`（assistant 声明侧与 tool 回执侧都放行），而 Rust 产线判据拒空 id
  ——新守门第一次运行就把这条差异报了出来（夹具比真机宽松，正是它文档里点名的
  「溜到真机才暴露」）。夹具现按真机口径拒空 id，两侧拒因各自可辨。
- **C++ 侧不参与**（如实差异）：宿主不解析模型历史，这套判据由 agent 一侧承担；两条
  夹具独有规则（DeepSeek 的 `reasoning_content`、`tool_call_id` 非字符串）在脚本的
  `FIXTURE_ONLY_RULES` 里逐条登记理由——差异是记录在案的，不是沉默的。

### 2026-10-04 第三轮结构重构收官：本轮前后对照

- **范围与账**：Phase 16–22（基线核查 / 守门可信度 / 跨源一致性守门 / C++ 残留结构 /
  Rust 残留结构 / sim 内部结构 / 文档与计数校准）。基线 `b442c6c`（2026-10-03）到本轮
  末尾共 **24 个提交、77 个文件、+5713/−2639**。全程行为保持：sim 线上协议、三条 socket
  缺省路径、动作码 0–10、场景文件格式、MCP 线协议字节、SQL 语法与退出码均未动。
- **守门段数**：`test/run_all_selftest.sh` **8 段 → 11 段**（新增第 9 段状态目录取值链的
  三处拼法、第 10 段文档与仓库实况、第 11 段配置字段白名单双端）。该脚本在 `gate:release`
  里跑在 `run_all.sh` **之前**、且**不进它的记账**，所以这三段不会灌进「守门用例」那个数；
  本轮 `run_all.sh` 只多了一条 `ica-e2e`（此前无人调用的离线脚本接进主干），汇总行
  **270 → 271 通过**。
- **跨源零编译校验**（自检第 8–11 段，逐段明细见 `docs/testing.md`「跨源一致性校验」）：
  **1 段 / 1 个脚本 / 1 组契约 → 4 段 / 3 个脚本 / 10 组**（脚本侧 9 组 + 第 9 段内联 1 组）。
  `check_sim_contract.py` 4 组（动作码表 / 三条 socket 缺省路径 / 一问一答桥请求关键字 /
  默认场景的内置回落↔播种模板）、`check_doc_paths.py` 4 组（路径引用逐个实测 / `drivers.md`
  模块清单与 `ls modules/*.lua` 双向相等 / 计数口径算术自洽 / 文档全部挂上索引）、
  `check_llm_field_whitelist.py` 1 组（C++ `kKnown` ≡ Rust 解析 25 + 只登记 14）、第 9 段 1 组
  （状态目录取值链的三处实现同拼法）。为什么一律零编译、且各写一份独立实现而不共享代码，
  理由同处写着：`sim/` 不在任何 CI job 的构建范围内，而共享实现等于共享同一个错误。
- **最大文件排行**（本轮唯一有大文件的地方是 `sim/`）：`sim/src/world.cpp` **633 → 306 行**
  （2D 有向盒几何抽 `obb2d.cpp`、场景读写抽 `scene_io.cpp`）；`sim/tests/main.cpp` 拆分前
  **1621 行 / 25 个套件 → 51 行名册 + 按域 8 个 TU**（最大的 `test_world.cpp` 410 行）。
  `src/` 与 `agent/src/` 的最大文件本轮**未变**（`src/sqlparse.cpp` 593、
  `agent/src/session.rs` 1289）——上一轮已拆过，本轮在这两处做的是包含面收窄
  （`scriptlimit.hpp` / `daemonconf.hpp`）与补测，不是再拆。
- **构建墙钟**：同机 `-j20` 全新配置 + 构建（Release、无缓存，不含 Rust agent 与 `sim/`）
  基线 **17s** → 本轮 **20s**（两次全新构建分别 23s / 20s，取较快那次）。涨幅来自构建出的
  目标文件数 **140 → 143**（`test/cpp` 多了三个门面单测 TU），不是生产代码变大。
- **用例计数**（三处独立套件，口径与复算式见 `docs/testing.md`「四处套件的计数口径」）：
  C++ `mybot_tests` **619 → 649**（`test/cpp` 里 `TH_TEST(` 的静态计数 633 → 663，减去
  1 处宏定义与 `harness_selftest.cpp` 的 13 条）；Rust `cargo test` **134 → 148**（`#[test]`/
  `#[tokio::test]` 的静态计数与实跑数一致）；`sim_tests` **381 → 387**（按域拆分前后逐条相同，
  增量来自 `place` 失败原因三态）。`test/cpp/test_*.cpp` 文件数 35 → 38。
- **本轮出界与冻结**：孪生驱动收敛（`ADS1015`/`ADS1115` 这类同族器件各写一份的重复实现）
  按用户判定继续出界，`docs/drivers.md` 的「收敛豁免清单」（那是**刻意保留**、两套互为对照
  的重复，与孪生驱动不是一回事）本轮也未动；`docs/architecture_analysis.html` 与两张 SVG
  冻结为 2026-09-25 快照（只加声明、不重写），权威口径一律以 `docs/*.md` 为准。

### 2026-10-04 文档计数校准到实测值，架构网页与两张图冻结为快照

- **改了什么**：README 与 `docs/` 里的手写计数全部按实测更新（模块 58→59、`modules/lib`
  21→23、机器人 8→9、动作脚本 35→50、Lua 测试 86→87、docs 12→14、C++ 单测文件 25→38，
  「600+ 个 C++ 单测」这个模糊数改成实测 649），并在结构树后补一段**派生口径**：每个数字
  都给出可复算的 `ls | wc -l` 命令，下一轮改完文件用命令复核，而不是再猜一遍。项目结构树
  此前完全没有 `sim/`，现补上整棵子树与 `modules/Sim.lua`；文档索引补 `docs/sim.md`。
- **架构页冻结**：`docs/architecture_analysis.html` 与 `architecture_layers.svg` /
  `architecture_dataflow.svg` 是 2026-09-25 的快照，此后 sim/ 迁入、多机器人、`~/.mybot`
  持久化、领域门面拆分四件事都没反映。**只加声明、不重写**：网页顶部加冻结声明（HTML 注释
  + 可见横幅，写明快照日期与这四项）、两张 SVG 各加一行快照日期文字。
- **为什么不顺手改数字**：那页的正文叙述与两图的框、箭头都是按当时的形态画的，只改 hero 区
  的数字会让页面自相矛盾（数字说 59 个模块、图里没有 sim 那一块）。冻结 + 指向权威文档，
  比半改一遍诚实。
- **`docs/reference.md` 环境变量表补 4 行**：`MYBOT_SIM_SOCK` / `MYBOT_SIM_CAPTURE_SOCK` /
  `MYBOT_SIM_SENSOR_SOCK` / `MYBOT_SIM_SCENE`。此前只有 `docs/sim.md` 讲过，而 sim 的取值链
  （`argv[i]` > env > 缺省）与多机器人的 socket 派生规则（robot 0 字节不变、robot i>0 在
  `.sock` 前插 `-i`）在 reference 里查不到。`MYBOT_SIM_SCENE` 那行同时写明「裸名需要模板，
  取不到模板会静默退回内置场景且场景路径为空，于是 `S`/`save`/退出保存全部 no-op」——这条
  此前只存在于 sim 的日志文案里。
- **`docs/agent.md` 三处旧路径**：`KNOWN_FIELDS` → `agent/src/config/file.rs` 的
  `PARSED_FIELD_NAMES`（由 `parsed_fields!` 宏表展开）+ `KNOWN_UNPARSED_FIELDS`；
  `config.rs` → `agent/src/config/`（环境变量链在 `mod.rs`，文件内联与 `api_key*` 在
  `file.rs`）；`repl.rs` → `repl/mod.rs`，并给出 `find agent/src -name '*.rs' | wc -l` 口径。
- **`docs/drivers.md` 计数口径修正 + 补一个漏登记的驱动**：58 未含 `modules/Sim.lua`，改为
  59 = 48 器件 + 3 总线 + 8 框架/工具；`modules/lib` 21 → 23（含 `virtbus/regmap.lua`）。
  `Sim` 驱动的表行此前**从未存在**——它是由同日新增守门的「双向相等」检查抓出来的（见下条）。

### 2026-10-04 跨源一致性：文档与仓库实况、配置字段白名单各加一组零编译守门

- **`scripts/check_doc_paths.py`（新）**：计数与路径改完只是「今天对」，没有任何东西阻止它们
  明天再过期。新脚本零编译核对四组：① 全部 docs 里的路径引用逐个实测存在（342 条）；
  ② `docs/drivers.md` 各节点名的模块集合与 `ls modules/*.lua` **双向**相等；③ 计数口径那串
  算式自洽（59 = 48+3+8、lib 23）；④ 每篇 `docs/*.md` 都挂在 README 或 `docs/README.md`
  的索引上。
- **路径解析规则是踩出来的**：markdown 链接按**文档所在目录**解析、反引号代码段按**仓库根**
  解析。两种混着解析会产生数百条假阳性（首轮原型报 241–756 条），分成两套规则后只剩 12 条
  真缺失，且全部可归类：7 条历史引用（已删的 Lua 协议驱动、已改名或合并的脚本与测试）+
  5 条占位示例（`modules/X.lua`、`robots/xxx.sql` 这类）。12 条各带**写明理由**的豁免登记在
  脚本里，脚本还会反向报告「豁免已失效」（对应路径又出现了）——豁免表不会静默烂成死名单。
  这条审计因此从一次性核对变成了常设守门。
- **`scripts/check_llm_field_whitelist.py`（新）**：模型配置字段的白名单在 C++
  `src/llmconfig.cpp`（`kKnown`）与 Rust `agent/src/config/file.rs`（`parsed_fields!` 宏表
  展开的 `PARSED_FIELD_NAMES` + `KNOWN_UNPARSED_FIELDS`）各手写一份，`docs/agent.md` 声称
  两者同集合却没有任何测试钉住。现按纯文本解析双侧、断言**双向**相等，并自检「两个 Rust
  名单不重叠、无重复」；另加一道抽取自检（用宽松的字符串字面量正则数一遍、与识别到的字段数
  比对），防止正则写窄之后「两侧都抽到 0 个、于是相等」这种自证式绿。
- **接进守门**：两个脚本挂在 `test/run_all_selftest.sh` 第 10、11 段（`gate:release` 与
  pre-push 都跑），缺 `python3` 时**判失败**而不是跳过——跳过会让这两组静默失效。反向验证
  逐个做过：删掉一条模块表行、把一个计数改错、从两处索引同时摘掉一篇文档、在 C++ 侧加一个
  Rust 没有的字段、在 Rust 侧加一个怪键，五种破坏各自让对应脚本变红并指出具体位置，还原后
  复绿。
- **对使用者意味着什么**：行为零变化（纯守门）。此前「文档说 58 个模块、目录里其实 59 个」
  与「配置字段白名单两端漂移」都不会有任何一侧报错——前者只是读文档的人被误导，后者会让
  一个字段在其中一端被静默忽略。

### 2026-10-04 仿真器内部结构治理（行为保持）

- **2D 有向盒几何抽成 `sim/src/obb2d.{h,cpp}`（入 simcore，可 headless 单测）**：原先
  「旋进盒局部系 → 钳制最近点 → 算距离」在 `resolveSphereOBB2D` / `supportHeight` /
  `pick` 三处各抄一遍，「`(cos θ, -sin θ)` 前方向量」在 `queryRange` / `pick` / `place`
  三处各抄一遍——六份手抄同一条朝向约定（盒局部 +X 在世界系的方向与 `Robot::theta`
  同号），任一处符号写错都不会报错，只会表现为「测距偏一点」「拾取够不到」这类难查的
  几何偏差。现在这条约定只有一个定义点：`Obb2D::forwardOf` / `worldToLocal` /
  `localToWorld` / `clampToBox` / `distanceSqXZ` / `projRadius` / `overlapXZ` /
  `rayHit` / `resolveSphere`。函数体逐字搬迁、算法未改；`world.cpp` 634 → 487 行。
  设计约定原样保留：绕 Y 旋转是等距变换，**只旋位移增量、不旋绝对坐标**。
- **`World::boxesOverlapXZ` 删除**：它原本只是「供测试直调」而开的一扇后门（`place()`
  内部另有一份 `obbOverlapXZ`）。几何模块公开后，`sim/tests` 的九处断言直接调
  `Obb2D::overlapXZ`，同一函数不再有两扇门（两扇门 = 改了这边忘了那边）。
- **场景文件读写析出 `sim/src/scene_io.cpp`**：`readScene` / `writeScene` 与只被它们
  用到的三个魔数、`kMaxSceneRobots`、`parseFloats` 整体搬走（`world.cpp` 487 → 303 行）。
  `world.cpp` 讲的是「世界里的物体怎么相互作用」（碰撞推出、测距、拾放），场景 IO 讲的是
  「这些物体怎么落盘、怎么读回来」，两件事只在数据类型上相交。声明仍留在 `world.h` 尾部——
  `SceneState` / `Obstacle` / `RobotSpawn` 三个类型都在那儿，声明跟着类型走，故没有配套的
  `scene_io.h`（拆出去会与 `world.h` 互相 include）。所有既有调用方（`sim_engine.h`、
  `main.cpp`、`sim/tests`）的 include 一行未改，行文本格式、告警文案、0600 权限与
  temp+rename 的原子写全部逐字保持。
- **`sim/tests/main.cpp`（1621 行 / 25 个套件）按域拆成 8 个 TU**：`test_robot.cpp`
  （运动学 + 相对运动）、`test_world.cpp`（测距/碰撞/搬运/OBB 几何/放置解算）、
  `test_scene.cpp`（场景文件 IO + SimPaths）、`test_bridges.cpp`（三座桥 + 请求-应答
  基类）、`test_contract.cpp`（动作表内部不变量 + 与 `modules/Sim.lua` 的跨语言一致性 +
  逐码派发）、`test_engine.cpp`（引擎 headless：场景装卸、多机器人寻址、跳跃/支撑面），
  加共享底座 `harness.{h,cpp}`（断言记账 + AF_UNIX 客户端助手 + `writeTextFile`）与
  `suites.h`（套件声明）。`main.cpp` 只剩名册与汇总行。断言记账改成 `harness.cpp` 单点
  持有并 `extern` 声明——拆成多 TU 后若各文件各留一份文件级全局，汇总行只会报最后链接
  那个文件的账，「381 通过」会静默缩水而退出码照样是 0。
- 拆分是搬迁不是重写，两处可核：① 断言总数与拆分前**逐条相同**（381 通过 / 0 失败）；
  ② 原文件 1441 行非空内容里，1404 行按原顺序逐字出现在新文件中，余下 35 行是头部
  注释/include/`namespace` 两行（按域重新分配），2 行是助手注释上提到 `harness.h`
  的声明处（本仓惯例：契约写在声明处，实现处不重述）。`action_table.h` 与
  `scripts/check_sim_contract.py` 里指向旧文件的三处路径注释同步更新。
- **`sim/src/main.cpp` 的三条平行 vector 收成 `PerRobotRig`**：命令桥 / 测距桥 / 蜘蛛
  步态按同一个下标一一对应，此前是三条 `vector<unique_ptr<...>>`，「同长同序」只靠
  `buildPerRobot` 把三者建在同一个循环里维持——而主循环里 `wantRange` 按
  `sensorBridges` 的长度分配、却用 `cmdBridges` 的下标去写，两者一旦不同长就是越界写。
  收成一条 `vector<PerRobotRig>` 后这个状态写不出来，且旧名字全部消失、漏改的调用点
  一律编译不过（这类重构的失败模式落在编译期而非运行期，才敢在没有单测的文件里动）。
  日志文本、socket 派生规则、渲染顺序逐字未动。
- **`main()` 抽 `renderFrame` 评估后不做**（计划里标为可砍项）：那段渲染 43 行要读
  window / phong / lines / meshes / 两个光照常量 / engine / rigs 八样东西（`g_camera`
  是文件级全局、不用传），抽出去就得传八个参数或再造一个上下文结构体，行数不减、
  反多一层间接。更关键的是 `main.cpp` 是 `sim/` 里唯一碰 GL 的文件、**没有 headless
  模式**：抽不抽都进不了 simcore 那张 387 条断言的网——本轮 obb2d / scene_io / 测试
  拆分之所以划算，正因为它们把代码搬进了有网的地方，这里搬完仍在网外，验证手段还是
  肉眼看窗口 + 冒烟脚本。而这段代码的价值恰恰在**顺序**（poll 在画之前、测距应答在
  `step()` 之后、抓帧在 `SwapBuffers` 之前），拆成两个函数会把这三条约束从「顺着读
  一遍就看见」变成「跨调用边界记在脑子里」。留着不改。
- 命名去掉 `obb` / `2D` 前缀，由 namespace 承载（与既有 `SimPaths` / `SimConfig` 同款），
  对照表写在 `obb2d.h` 顶部。`robot.cpp:17-18/31-32` 那两处同一条前向约定**刻意未并入**：
  它们在速度积分器里、乘的是 `v_*dt` 而非单位方向探测，为两行把 `Robot` 耦合到几何模块
  不划算——记下这处出界，便于日后判断是否要收。对使用者意味着什么：无（`sim/` 内部结构，
  线上协议、三条 socket 缺省路径、动作码表、场景文件格式均未动）。

### 2026-10-04 仿真器 place 失败日志细分（可观测面变更）

- **`place[N]` 的失败日志不再一律说「未持有物体」**：`World::place` 有三条 `return false`
  路径（没背东西 / 推出距离超限 / 兜底后仍 3D 重叠），日志只认得第一条，后两种几何不可行
  也被打成「未持有物体」。最误导人的是它**与事实相反**：那两种情形下机器人明明还背着箱子
  （`place` 失败不清搬运态、不改场景），操作者却会照日志去查「pick 怎么没生效」，方向从
  一开始就是错的。现在 `place()` 多一个可选出参 `World::PlaceFail* why = nullptr`——
  默认参数，`sim/tests` 与 `sim_engine` 之外的既有调用方一行未改；三种取值直接对应
  `world.h` 契约里早已写明的「没背东西 / ① / ②」，没有新增语义。
- **日志前后对照**（`place[0]`；成功行 `place[N]: 放置成功` 未动）：
  未持有物体 `place[0]: 未持有物体，放置失败` → 逐字不变；
  推出距离超限 `place[0]: 未持有物体，放置失败` → `place[0]: 推出距离超限 (被墙连环顶死)，放置失败`；
  兜底后仍重叠 `place[0]: 未持有物体，放置失败` → `place[0]: 落点仍与障碍 3D 重叠，放置失败`。
  这三行只出现在 sim 进程的 stderr（给人看的诊断），不在线上协议里：`.mt` 动作码、三条
  socket 缺省路径、场景文件格式均未动，仓内也没有任何脚本或测试解析这段文本
  （`docs/sim.md` 的「放置解算」段记了 `why` 出参与这三种文案）。
- `placeFailText` 的 switch 刻意不写 `default`（与 `bridge.cpp` 的 `commandName` 同款），
  新增 `PlaceFail` 取值而漏了 case 会被 simcore 的 `-Werror=switch` 挡在编译期。三种原因
  各补一条断言，出参先填一个**错的**取值以免「place 忘了写 why」也判绿；其中「仍重叠」
  此前没有确定性复现用例（只有不预判收敛/放弃的「连环墙」间接经过），现新造一个：悬空
  横梁底面 0.60 高过释放底面 0.50，按「墙」的定义不被推开、按「支撑」的定义又不参与
  落地，落定后箱顶 0.90 撞进它的竖直带——正是写回前那次事实性 3D 校验存在的理由。
  sim_tests 381 → 387 通过 / 0 失败。

### 2026-10-04 演示脚本 `sim_agent_demo.sh` 退出后泄漏两个进程（修复）

- **cleanup 杀错了对象**：实时工具流是 `( tail -n +1 -F 审计 | python3 格式化 ) &`，
  `FEED_PID=$!` 拿到的是**那个子壳**的 PID，而 `tail` 与 `python3` 是子壳的子进程。
  cleanup 只 `kill "$FEED_PID"`：子壳死了，两个子进程被 reparent 到 init 接着跑。
  对使用者意味着什么：① 每跑一次 demo 永久泄漏两个进程（`tail -F` 还一直握着审计文件）；
  ② 它们同时握着 demo 的 stdout 管道不放，**脚本早已退出、读管道的一方却永远等不到
  EOF**——把 demo 输出接进 `| tail`、`| tee` 或任何采集器的用法都会挂死在那儿。
  交互式终端下看不出来（进程静默留在后台），正是它一直没被发现的原因。
- 改为先 `pkill -P "$FEED_PID"` 杀子进程、再杀子壳。验证不起 LLM：`AGENT_BIN=/bin/false`
  跑通「起 sim + 起后端 + 起 feed + 立刻拆链路」整条 cleanup 路径，改后管道立刻读到 EOF、
  `pgrep` 查无残留；反向验证用 `git show HEAD:` 取回改前脚本副本同样跑一遍，`tail`
  与 `python3` 两个孤儿如期出现（实测 PID 1013661/1013662），确认修的就是这个。

### 2026-10-04 Rust agent 残留结构治理（行为保持）

- **`config/summarize.rs` 补密钥泄漏守门与分支单测**：`--check-config` 的摘要此前只被
  `config_debug_and_display_never_leak_the_api_key` 间接管住——那条钉的是 `Config` 的
  `Debug`/`Display`（2026-09-30 审计 P1 之后由 `Secret` 包装在类型层面封死），而 `summarize`
  是**另一条独立的渲染面**：它自己拼字符串，`Secret` 帮不上忙，只要有人把那一句的
  `key_source` 换成 `cfg.api_key.expose()`，密钥就直接进了 stdout。现由一条用例对四种来源
  逐个断言「摘要里不出现密钥、且写明『值不显示』」。同批钉住会话持久化栏的六个分支
  （来源 × `--resume` × `state_dir_ok` × `--new-session`）、四处 `0 → 不限` 的哨兵渲染、
  历史摘要开/关两态，以及「`配置告警` 栏只在有告警时出现」。
- **`config/cli.rs` 补选项名册的双向漂移守门**：`parse_args` 的 match 臂与 `usage()` 的文本
  是两处各自维护的事实源，此前只有 `--prompt-file` 累积与两条报错路径被测过。现在测试侧
  独立登记一份名册（14 个带值选项 + 6 个开关 + 2 个短别名），两条方向都钉：名册里的每个
  选项必须出现在 `--help` 里（漏文档 = 用户发现不了），`--help` 里出现的每个 `--xxx` 必须被
  `parse_args` 认得（文档承诺了却没实现 = 用户照着抄就吃「未知选项」退出码 2）。
  三个合法例外写死在用例里并注明理由：`--mcp-backend` / `--mcp-socket` 是后端 `mybot`
  自己的命令行（解释 `--socket` 时引用），`--llm-key` 是**刻意不存在**的选项（argv 对同
  用户进程可读，密钥不给命令行入口）。另补一条「每个带值选项都把值落到自己那一个字段」的
  用例——给每个选项喂一个由选项名派生的独特值再逐字段核对，专打复制粘贴串线
  （`--llm-max-turn` 的臂写成 `cli.llm_max_tokens` 这类）。
- **`ica/specs.rs` 补规格面 → 派发面的跨文件一致性守门**：`specs()` 的八条 schema 声明与
  `IcaLedger::execute` 的八个分支臂自 2026-09-29 B1 拆成两个文件后，「声明了但没人派发」
  这条缝没有任何东西看着——症状是模型拿到一条工具描述、照着调用、只吃「未知 ICA 工具」。
  现由一条用例逐个真调 `execute` 并只盯这一种错误（缺必填字段的参数校验错误放过）。
  反向（有分支无声明）不会让模型出错、只是死代码，由 `len() == 8` 与评审兜住，不在测试里
  读源码扫描。同批钉住声明面自身的完整性：名字唯一、全部 `ica_` 前缀（`merge_specs` 靠这个
  前缀与后端工具名分域）、schema 顶层是 object、`required` 里的每一项都在 `properties` 里
  声明过。
- 以上三条守门对使用者意味着什么：无（纯新增测试）。反向验证按「破坏实现侧、看对应用例转红」做，
  三处破坏（摘要改渲染 `api_key.expose()`、`usage()` 多写一个未实现的 `--llm-thinking`、
  删掉 `execute` 的 `ica_depend` 分支臂）逐个转红且报出的正是对应的名字，还原后复绿。
  `cargo test` 134 → 147 通过 / 0 失败，`clippy --all-targets -D warnings` 与 `fmt --check` 同绿。

- **`config::build` 由 183 行收到 120 行**：两段各归其位。§8 会话持久化（62 行，含「配置文件
  来源不能授权 `--resume` 读取」这条判据的全部论证）析出为 `resolve_session`，与同文件既有的
  `resolve_driver_and_var` / `resolve_endpoint_and_model` / `resolve_api_key` 同形状（`§N` 文档
  注释 + `Result`），`build` 里只剩一行调用。§5b 的四条命令行数值覆盖（32 行、四份复制粘贴的
  `if !cli.xxx.is_empty() { cfg.yyy = parse_int_option(...) }`）析出为 `override_number`，
  **重复被消掉而不是搬走**；四处的先后顺序就是四条报错的优先序，故调用点仍逐条列出、不折进循环。
  剩下的 48 行是那张逐字段的 `Config { ... }` 合并表，它本身就是一张表，不再拆。
- **代价当场补掉**：`override_number` 把四处形状收成一处之后，「串线」（把 `max_turns` 的区间
  配给 `max_tokens`、或把值写进邻居字段）从编译期错误降级成了运行期错误，而既有的
  `int_option_ranges_are_enforced` 只测 `parse_int_option` 本身、测不到接线。新增一条端到端
  用例把「各自落到自己字段」与「各自用自己区间」一起钉住（含 `0` 只对 `--llm-task-timeout`
  合法这条不对称，以及报错必须点名选项）。反向验证：把第一条覆盖的 `&mut cfg.max_turns`
  改成 `&mut cfg.max_tokens`，该用例立刻转红并报出 `left: 8, right: 12`（默认值没被覆盖），
  其余 147 条不受影响，还原复绿。
- 以上对使用者意味着什么：无（纯拆解 + 一条新增测试，报错文案与优先序逐字保持）。

### 2026-10-04 Rust 侧三项评估收口（判不改，理由留档）

- **`Config` 的 37 个 pub 字段（Phase 15 遗留项「收 `pub(crate)` / 按域分组」）——判两者都不做**：
  先说可见性。`mybot-agent` 是**纯 bin crate**（`Cargo.toml` 只有 `[[bin]]`，没有 `src/lib.rs`），
  crate 外没有任何东西能命名 `config::Config`——对所有可能的消费者而言 `pub` 与 `pub(crate)`
  **不可区分**。把 37 个字段改窄是一份 37 行、零可观测效果的 diff，而且会反向误导：
  `pub(crate)` 读起来像「这对某个人藏着」，事实是「没有那个人」。真要加 `lib.rs` 的那天，
  正是该**有意**决定公开面的时候，不是现在预先收窄。
  再说按域分组（`cfg.budgets.max_tokens` 这类）：它会改动 5 个文件里 **139 处生产读点**
  （实测 `cfg.` 计数，不含测试）、行为零变化，代价却是破坏本文件头注声明的那条纪律——
  `Config` 的字段**逐条镜像** C++ 侧的 `AgentModeConfig`（两侧常常吃同一份 `mybot.llm.json`）。
  扁平形状正是「C++ 与 Rust 逐字段对账」能一眼做完的原因；分组之后每次对账都要先解开一层嵌套。
  另外，字段多也不等于「内部状态被外面乱改」：实测全仓对 `cfg.<字段>` 的**写入**只有两处，
  且都在启动装配的延长线上——`main.rs` 的 `prepare_session` 把解析出的会话落点写回
  `cfg.session_file`（不写回则缺省来源下下游 ICA 账本落盘与 `/clear` 分界拿到空串、账本静默
  不落盘），`load_prompt_pack` 填 `cfg.prompt_pack`。其余全是读，且读得很散（生产段计数：
  `cfg.session_file` 13 处，`cfg.resume` / `cfg.model` / `cfg.max_task_seconds` /
  `cfg.config_warning` 各 7 处，横跨 `repl/mod.rs`、`provider.rs`、`main.rs`、`config::build`、
  `config::summarize` 5 个文件）。
  加 37 个 getter 只是把 `cfg.x` 换成 `cfg.x()`——名义上的封装，反而把两处**有意为之**的写回
  藏进 setter。真正需要保护的那一个字段（`api_key`）2026-09-30 起已由 `Secret` 在类型层面封死
  （Debug/Display 一律 redacted，唯一取值口 `expose()`），本轮又补上了 `summarize()` 这条独立
  渲染面的守门。重新评估的触发条件：加了 `lib.rs`，或出现第三处启动期之外的写入。
- **`session.rs`（1289 行）——判不拆 codec/IO**：行数是全仓 `.rs` 之首，但拆开看是
  **前 727 行生产代码（非空 688）+ 后 562 行测试（非空 523）**，生产侧已由五条 `// ----`
  分节标出（落盘 / 载入 / 协议自洽 / 读 / 写），而五节都伺候同一份东西：**会话文件的行格式**。
  那条格式是对外契约（与已删除的
  C++ `agenthistory.cpp` 逐字节兼容，存量文件要能被 `--resume` 读回），而 `encode_message` 与
  `decode_message` 是这份契约的两半——把它们分到两个文件，正是格式漂移最容易藏身的地方
  （现在盯着它们的是同文件里的 `roundtrip_messages`）。代价也是实的：拆 codec/IO 要把 8 个
  私有件（`encode_user` / `encode_assistant` / `reasoning_text` / `tool_result_text` /
  `DecodeState` / `decode_message` / `decode_assistant` / `parse_head`）升成 `pub(crate)`，
  与本轮 C++ 侧判 `SingleConsumerCheck`「不再拆」同一条理由：把私有状态升级成跨模块契约，
  换不到行为收益。688 行、五条分节、单一契约，不是上帝文件。
- **`signals.rs`——判不补单测（但把「已覆盖到哪」说清）**：这个文件 74 行里没有近纯函数面
  ——通篇是 tokio 的信号流、`libc::signal` 的 `unsafe` 还原缺省处置，以及第二次信号时的
  `std::process::exit(130)`。单测要碰它就得在测试进程里真发信号，而升级路径**会把测试进程
  自己打死**。如实说清覆盖面：**第一次信号**的语义（置停止标志、在飞模型调用立即被放弃、
  本轮以「请求被中断」收场、不再受理新任务、REPL 优雅退出 0 而不是被信号打死）已由
  `scripts/agent_e2e.sh` 的 **R8b**（`sleep 2` 后 `kill -INT`，断言 stderr 有「收到停止信号」
  与「请求被中断」、0 条慢响应、用时 < 5s、退出码 0）与 **R16**（终端 raw 模式下 Ctrl-C
  走同一条路、退出码 0）从**外部**钉住——这也正是它唯一能被诚实钉住的位置。
  **第二次信号的升级路径目前没有守门**：`state.stop.swap(true)` 返回旧值即升级，是个两行
  原子操作、与 C++ `onStopSignal` 同款；要在 e2e 里摆出这个局面，就得让第二个信号恰好落在
  「标志已置」与「优雅退出完成」之间，那是一条竞时的 flaky 红——不做。若哪天这段长出真正的
  逻辑（比如按信号种类分级、或升级前先落一条审计），再补。
- 以上对使用者意味着什么：无（三条都是判不改）。

### 2026-10-04 C++ 残留结构治理（行为保持）

- **`MtVars_Daemon` 的类声明迁到 `daemonconf.hpp`**：它的 `Factory`/`play`/`build`/`destroy`
  四个成员函数实现本就全在 `daemonconf.cpp`，声明却留在 `daemonvar.hpp`。迁移后
  `daemonvar.hpp` 的范围收窄成「共享槽布局 + 停机协议位 + 线程机 + 守护运行器」，对
  hardware 域的依赖也从整个 `hardware.hpp` 缩到只需 `json.hpp`（`readSample` 的返回类型）；
  `hw_factory.cpp` 不再为自己用不到的槽布局与线程机引 `daemonvar.hpp`。
  对使用者意味着什么：无（纯声明归属与包含关系调整）。
- **脚本输入的字节上限四件套析出叶子头 `scriptlimit.hpp`**：`kMaxScriptBytes` / `ScriptTooLarge` /
  `readAllLimited` / `LimitedReader` 原在 `actionsource.hpp`；离线矩阵工具（`mtmerge`/`mtinclude`）、
  agent 工具层与它们的单测为了拿一个上限常量，被迫拉进整个 CLI/动作源域头（`CliOptions` 的四组
  嵌套字段、TCP 读取器声明、`<sys/socket.h>`）。这四样本是同一个关注点（「输入多大、进程就吃多大
  内存」这条护栏），现独立成叶子头，四个 TU 改引它；`CliOptions` 与三个读取器留守原头。
- **`actionsource.hpp` 的包含清单收窄**：摘 `<sys/socket.h>`（头内只有注释提 `sockaddr_un.sun_path`
  与 `SO_PEERCRED`，`actionsource.cpp` 自己已引）、`<map>`（头内一处未用）、`<istream>`（随
  `readAllLimited` 搬走）。`<iostream>` **评估后维持**——`FileReader::read` 的 I/O 错误告警要
  `std::cerr` 这个对象，`<ostream>` 只给类型不给对象。
- 以上两条对使用者意味着什么：无（纯搬迁 + 包含关系调整）。核查：`-Werror` 全新构建、Release
  全量、ASan+UBSan 全量三跑同绿（271 通过 / 0 失败 / 1 跳过，C++ 单测 620 条不变）；另用两个
  负向编译探针确认「叶子头里已看不到 `CliOptions`」「`actionsource.hpp` 已不再提供 `sockaddr_un`」
  ——收窄是真的，不是把 include 挪个位置。
- **`StructureStore` 门面补直接单测**（`test/cpp/test_structure_store.cpp`）：这个门面 2026-10-03
  才从 `RobotStruct::matrices_` 析出，此前只经 `test_robot*` 的集成路径间接覆盖。它的头注释把
  「容器是 `std::deque`」写成**不变式**（`MetaProvider` 终身持有指向表元素的裸指针，`std::vector`
  扩容会让它们全部悬垂），现由一条比地址的用例钉成可判定事实，而不是靠「没崩」；其余用例钉
  装载声明序、`appendRows` 交回的首个新行号、两条回滚路径、热删除的保序擦除、整表撤下、按名
  查找与必须在 `active()` 之前可判定的 `declaresVariable`。对使用者意味着什么：无（纯新增测试）。
- **`HwRegistry` 与 `MetaRegistry` 两个门面补直接单测**（`test_hw_registry.cpp` /
  `test_meta_registry.cpp`）。注册表侧钉的是热插事务回滚（`mark`/`rollbackTo`，含流引用快照的精确
  还原）与热删除撤销（`removeBatch` 的「先全量验存在性、再统一重排」，以及删中间条目后存活者的
  销毁序）——这两组是集成用例红了以后最难定位的：三容器锁步增长，任何「先记下标、再逐个 erase」
  的写法都会在第一次 erase 之后错位，症状是后续某次派发按名查到已删变量。元数据侧钉的是
  `dropRows` 的「矩阵地址 + 行号」判据（同一行号在两张表上是两个身份）、`shiftRowsAfter` 的
  **严格小于**、整表撤下的两阶段重锚定（`captureTableNames` → `rebaseByName`，查不到即跳过），
  以及 `validateFrom` 的增量收窄。原 `test_robot_dispatch.cpp` 首段的三条 `hw_registry_*` 用例
  逐字迁来，`dummyHw` 上收为 `fx::dummyHw` 供两边共用。
- **`context_service` 评估后不新建同名测试文件**：`buildContextData` 的唯一调用方是 MCP 后端的
  `context.get`，而既有的 `mcpbackend_context_get_returns_quoted_material` 已断言它 10 个键里的
  9 个（含类型与「数据不是指令」定界）——另立一个文件只为一条分支属于为覆盖率而测。真正的缺口
  是「结构脚本读不到」的回落分支，补一条同域用例把回落文案钉住：提示词素材里「这段读不到」与
  「这个机器人没有结构脚本」必须可区分。
- **`robot_application` 评估后不补**：它只有一个入口 `runRobotAndCleanup`，通篇是上电 / 运行 /
  清理的副作用编排，没有近纯函数面可在单测里锚定；其行为已由 `test_robot_lifecycle.cpp` 的
  集成用例覆盖。
- 以上对使用者意味着什么：无（纯新增测试 + 一次同域搬迁）。反向验证按「破坏实现侧、看对应用例
  转红」做，五处破坏（`removeBatch` 的存在性预检、`rollbackTo` 的流引用还原、`dropRows` 的行号
  单判据、`shiftRowsAfter` 的 `<=`、回落文案改空串）逐个转红且**只**转红对应用例，还原后复绿。
  C++ 单测 620 → 649 通过 / 0 失败。

### 2026-10-04 三项评估收口（判不改，理由留档）

- **`kJsonMaxBytes` 与 `kMaxScriptBytes` 同为 `8u << 20`——判不合并**：形状确实一样
  （一个上限常量 + 一条 "exceeds the byte limit of N" 的 `IllegalArgumentException`），
  但两者的**约束来源**不同。`kJsonMaxBytes` 是一条跨进程契约的中间环节：
  `mcpbackend_frame_limit_matches_rust_twin` 同时钉住 `kMcpMaxFrameBytes == kJsonMaxBytes`
  与 `kMcpMaxFrameBytes == agent/src/frame.rs::MAX_FRAME_BYTES`（测试期直接读 Rust 源）。
  `kMaxScriptBytes` 则是纯本地的内存护栏（整段驻留后再装配，实测 100MB 动作输入 → RSS 4.2GB）。
  合成一个常量就等于让一个数伺候两个主人：哪天为受限主机把脚本护栏调到 2 MiB，MCP 帧上限
  会跟着缩、与 `frame.rs` 脱钩，而孪生守门报出的位置指向错误的文件。`jsonparse.hpp` 的注释
  本来也只承诺「与脚本输入上限**同量级**」，不是「同一个常量」。异常文案同理不该并：
  `ScriptTooLarge` 点名的是「runaway or malicious producer + 整段缓冲后才播放」这套威胁模型，
  对一段模型 JSON 响应并不成立。
- **`hw_factory.cpp` 的 `SingleConsumerCheck`（135 行 TU 私有结构）——判不再拆**：它本身就是
  2026-09-26 批次 5 的拆分产物（原 `checkSingleConsumer` 127 行把总线路径表、两个捕获状态的
  lambda、Daemon 分支、常规硬件分支全内联在一处，拆完只剩「建状态 → 扫总线表 → 逐表分派」的
  编排）。再往独立 TU 搬，就得把文件私有的 `declaresSingleConsumer` 与 `scCache` 的作用域策略
  （**批内**缓存、跨批刻意不缓存，好让热插两批之间被替换的模块文件生效）一并导出——把一个
  函数的私有状态升级成跨 TU 契约，换不到任何行为收益。守门侧也不缺：
  7 条 `single_consumer_*` 直接用例 + 1 条热 SQL 复用预检的用例。
- **`agenttools.cpp` 的 SQL/硬件工具三件套（`makeApplySqlTool` / `makeListHardwareTool` /
  `makeQuerySqlTool`，共 92 行）——判不外移**：搬出去就得把三个 `make*Tool` 声明放进
  `agenttools_internal.hpp`，而那个头自己的准入规则写着「加到这里的东西以 "多 TU 都要用" 为准，
  别把单 TU 私有件挪进来」——它们只被 `makeAgentTools` 一个 TU 消费。对照 `agenttools_wait.cpp`
  为什么**该**拆：那是 304 行的轮询状态机，有自己的生命周期；这三件套 92 行里绝大部分是模型
  面向的工具描述文本，三处执行体各 9～13 行。顺带一条基线事实：`src/` 下最大的 .cpp 是 593 行
  （`sqlparse.cpp`），`agenttools.cpp` 以 579 行居第二——上帝文件在 2026-10-01 那两轮已拆完，
  剩下的长度差是内容差，不是没拆完。重新评估的触发条件：这三件套长出真正的逻辑（而不是更多
  提示词文本）。

### 2026-10-04 仿真器默认场景：内置回落与播种模板对齐，并接进零编译守门

- **改了什么**：`World::defaultScene()`（无场景文件时的内置回落）此前按 体积×`kDefaultDensity`(800)
  统一算质量，而 `sim/scenes/default.scene`（有文件时用户实际拿到的世界，也是首次运行拷进家目录
  地图目录的播种模板）是每箱手调的质量——两边几何与颜色一致、质量却差 2～6.7 倍，而模板顶部
  一直写着「与 `defaultScene()` 保持同值」。现让 `defaultScene()` 直接带上模板那 11 个显式质量，
  两份真的同值；`scripts/check_sim_contract.py` 增第 4 组，零编译比对两边 box 的十个字段。
- **对使用者意味着什么**：有场景文件的正常路径**行为不变**。只有「地图目录里没有 default.scene、
  又不给 `--scene`」这条回落路径变了：搬最轻的红箱从 409.6kg 变 61.44kg，按
  `Robot::kLoadSlowdownPerKg` 折算的最高速度从 0.31 m/s 回到 0.99 m/s——回落不再是「同一个世界
  的困难模式」。
- **为什么是 C++ 向模板靠、不是反过来**：模板的轻质量才是用户实际体验到的那份；把模板统一改成
  800 kg/m³ 会让 11 个箱子全重 2～6.7 倍，搬运基本推不动。场景文件里 `mass` 缺省仍按 体积×800 补
  （`world.cpp` 的解析回落，`sim/tests` 有钉子），未改。

### 2026-10-04 跨源一致性：状态目录取值链有了规则单点与三处拼法守门

- **改了什么**：`MYBOT_HOME` 非空 > `$HOME/.mybot` > 不可用——这条链在三处各手写一份
  （Rust agent 的 `derive_state_dir`、sim 的 `SimPaths::mybotHome`、`scripts/sim_agent_demo.sh`），
  此前没有任何一处被指认为准，也没有守门。现把规则单点写进 `docs/reference.md` 的环境变量表
  （新增 `MYBOT_HOME` 行），三处注释各自指向它，并由 `run_all_selftest.sh` 钉住「三处都仍按
  这条链取、缺省后缀都仍拼作 `/.mybot`」。
- **对使用者意味着什么**：行为不变。此前某一处的缺省目录名漂移不会有任何一侧报错——agent 的
  会话历史与 sim 的地图会各落一棵树、互不可见。

### 2026-10-04 守门自检：C++ 单测清单的第三份手抄表删掉

- **改了什么**：`CMakeLists.txt` 的 C3 自检此前比的是「磁盘上的 `test/cpp/test_*.cpp`」与一份
  手抄的 `MYBOT_CPP_TEST_REGISTERED` 裸名清单——而真正决定编不编的是
  `add_executable(mybot_tests ...)`，等于三份表里有两份互不相干，且只校验单向。现改为直接读
  target 的 `SOURCES` 属性，双向比对。
- **对使用者意味着什么**：加 C++ 用例仍然只需在 `add_executable` 里补一行；忘了补，configure
  期就 `FATAL_ERROR`（此前也红，但红的是手抄清单那一层）。清单里留着已删/改名的旧条目现在
  也会红。少一份要人肉同步的表。

### 2026-10-04 跨源一致性：仿真器契约的零编译守门从「动作码表」扩到三组

- **改了什么**：`scripts/check_sim_action_table.py` 更名为 `scripts/check_sim_contract.py`，
  在原有的动作码表之外再钉住两组手写双份的契约：① 三条 AF_UNIX 缺省 socket 路径
  （`sim/src/sim_config.h` ↔ `modules/Sim.lua`，含「三条互不相同」「均为 /tmp 下绝对路径」
  两条自检）；② 一问一答桥的请求关键字（`sensor.cpp` 的 `range`、`capture.cpp` 的
  `capture` ↔ `Sim.lua` 的 `range()`、`src/agentimage.cpp`、`Camera.lua` 的 `capture_live()`）。
- **为什么下沉到零编译脚本**：动作码表与 socket 路径在 `sim/tests/main.cpp` 里本来就有运行期
  交叉校验（`testActionTable` / `testSharedConstants`），但 `sim/` 不在任何 CI job 的构建范围内，
  那份只有开发者手动 `build_sim.sh` 时才会触发；请求关键字那一组此前两侧都没有任何校验。
- **对使用者意味着什么**：线协议字节、三条缺省路径、动作码 0–10 全不变（纯守门扩容）；
  此后漏改一侧在 push 前就红，而不是等到仿真器起来才发现驱动连不上或请求被拒。

### 2026-10-04 跨源一致性：MCP 线协议字段名收敛到单点 + 孪生守门

- **改了什么**：C++ 侧 `mcpbackend.cpp`/`mcptransport.cpp` 散着的 `"success"`/`"data"`/
  `"error"`/`"tools"`/`"_route"`/`"auth"`/`"token"` 收进 `src/mcp_protocol.hpp`（补 `kToken`）；
  Rust 侧 `frame.rs`/`bridge.rs` 散着的 `"token"`/`"name"`/`"arguments"`/`"success"`/`"error"`
  收进 `agent/src/protocol.rs`（补 5 个常量）。
- **新守门**：一条跨语言孪生用例比对两侧的字段**名单**，并反向禁止三个生产 TU 再出现裸协议
  字面量——否则常量表只是并存的第二轨。唯一合法的单侧名是 `ping`（只有后端应答，agent 不发）。
- **对使用者意味着什么**：线协议字节不变（纯命名收敛）。此前字段名漂移**两侧都不报错**：serde
  对多余键静默忽略、对缺失键取 default，回执会整块退化成空值而全绿。
- **附带清理**：删掉测试里三个上一轮写了却没接上的 `[[maybe_unused]]` 抽取助手（已被行为版的
  七字段用例取代）。

### 2026-10-04 修复：连不上仿真 socket 时段错误打死宿主进程

- **改了什么**：`modules/Sim.lua` 与 `modules/lib/unix_socket_ffi.lua` 原先把 glibc 的 `errno`
  声明成函数 `int errno(void)` 再调；`errno` 实为数据符号，按函数调等于跳到一个 int 值当代码
  执行，稳定 SIGSEGV。改用 LuaJIT 内建 `ffi.errno()`（与 HAL 的 glibc 后端同口径），删掉两处
  伪声明。
- **对使用者意味着什么**：仿真器没起来（或 socket 路径不对）时，`Sim` / `Camera` 的仿真链路
  从「整个 mybot 进程段错误」变回如实返回一句 ERR。错误文案、线上协议、三条 socket 缺省路径、
  动作码 0–10 全不变。
- **附带**：`test/test_sim.lua` 自该声明引入起在本机零输出即崩（此前被记为「LuaJIT FFI 环境
  问题」），整条 Sim 驱动守门等于没在跑；`test_camera.lua` 的「无监听端 → 响亮 ERR」用例同因
  崩溃。修复后两条守门恢复，全量守门回到全绿。

### 2026-10-04 守门可信度：ICA 两进程校验接进主干 + 离线脚本不再写真实家目录

- **接进守门**：`scripts/ica_e2e.sh`（ICA 证据账本的两进程离线校验）此前只有
  `docs/ica.md` 提到、没有任何守门调用它——离线脚本没人跑就会静默腐烂。现挂进
  `run_all.sh` 的 agent 段（`ica-e2e`），产物依赖与 SKIP 口径同 `agent_e2e.sh`
  （缺 `mybot-agent` 或产物陈旧按 SKIP，缺 `build/mybot` 判失败）。
- **对使用者意味着什么**：跑一次该脚本不再往真实 `~/.mybot/robots/<机器人>/sessions/`
  落一段会话历史（此前会，且缺省续接语义下这段垃圾上下文可能被下一次真跑接上）；
  脚本现无条件把 `MYBOT_HOME` 指向自己的临时目录，并支持 `BOT`/`AGENT_BIN` 覆盖产物路径。
- **评估后不改**：`run_all_selftest.sh` 的 `e2e_cases`/`parse_cases` 条数检查点（37/96）
  保持硬编码——它是「用例不得静默消失」的刻意防线（误删用例立刻红，失败信息直接指出
  要同步哪里），改成从用例表派生会让检查退化成自证。
- **刻意差异留档**：本地 `run_all.sh` 与 CI 调 `agent_e2e.sh` 的外层 timeout 不同（90 / 180，
  CI 机器慢，本地 90 实测够用）；环境隔离的单点在脚本自身，CI 命令前那个
  `BOT4CJ_LLM_CONFIG=none` 只是冗余保险。两处都补了注释，免得后来者「顺手统一」改错一边。

### 2026-10-03 守门可信度：仿真器动作码表的一致性不再需要构建 sim/ 才检查

- **改了什么**：新增 `scripts/check_sim_action_table.py`——纯文本解析 `sim/src/action_table.cpp`
  的 `kActions[]` 与 `modules/Sim.lua` 的 `CODES`/`PARAM_CODES`/`PARAM2_CODES`/`CODE_MAX`，
  比对两侧动作码表，接进 `test/run_all_selftest.sh`（pre-push 与 CI `gate:release` 都跑）。
- **为什么**：`sim/tests` 里本来就有 `testActionTable` 做同一件事的运行期交叉校验，但 `sim/`
  不在任何 CI job 的构建范围内（CI 镜像无 GL/X11、musl 也建不了 GLFW，见 `sim/CMakeLists.txt`
  顶部注释），那份校验只在开发者**手动**跑 `build_sim.sh` 时才触发——等于一张从未被强制执行过
  的安全网。下沉成零编译脚本后，「加了动作只改 C++ 表、忘了改 Lua」在 push 前就红。
- **两套实现刻意不共享代码**：脚本与 `sim/tests/test_contract.cpp` 各写一份解析（连「取平衡
  花括号块」的算法都各自独立实现一遍）——共享实现等于共享同一个错误，两份独立实现才能互相
  印证。用故意制造的漂移样本验证过脚本确实检得出。
- **对使用者意味着什么**：线协议、动作码 0–10、三条 socket 路径全不变（纯守门）。该脚本
  次日更名为 `check_sim_contract.py` 并从 1 组契约扩到 4 组。

### 2026-10-03 仿真器拾放：不安全的状态改判失败，不再静默把场景改坏

- **`pick()` 拒绝抽取被压住的下层**：目标箱上方若贴着另一只箱子（`kGroundSnapTol` 容差 +
  XZ 重叠，只搭边角也算），拾取直接失败。此前会把下层抽走、上层留在原地——静态箱没有重力，
  画面上就是一只箱子凭空悬着。
- **`place()` 先在局部副本上解算，写回前做一次事实性 3D 重叠校验**：几何不可行（累计推出
  距离超过新增的 `kMaxPlacePush` 上限、或兜底后仍重叠）时返回 `false`，**物体保留搬运状态、
  场景一字不改**，而不是硬塞一个可能重叠的结果。
- **回归验证抓到一个真 bug**：12 道链式薄墙的场景在旧实现下会悄悄产生一个真实的几何重叠却
  返回成功——新增的最终校验修的是这个潜伏问题，不只是加固。
- **对使用者意味着什么**：`.mt` 动作码、线上语法、场景文件格式不变。行为变化只有一处：
  以前「报成功但世界坏了」的两种情形现在如实报失败，机器人仍背着箱子（可换个落点再试）。
  新增 `testPickBlockedByStack` / `testPlaceInfeasible` 两条守门。

### 2026-10-03 家目录持久化：对话历史与地图缺省落 `~/.mybot`

- **状态目录**：`MYBOT_HOME` > `$HOME/.mybot`（首次自动逐级建目录 `0700`）。agent 的
  对话历史与 sim 的地图共用这一条取值链。
- **对话历史缺省落盘**：不给 `--session-file` 时，历史按机器人归档到
  `$MYBOT_HOME/robots/<机器人>/sessions/<UTC 时间戳>.jsonl`（`sessions/latest` 指针记
  当前段；段 `0600`、`O_EXCL` 原子创建、同名加后缀）。**交互与 `--task` 都默认续接**
  该机器人上一次；新增 `--new-session` 强制新开（与 `--resume` 互斥，同给退 2）。
  启动时把实际落点打到 stderr。
- **续接失败不中止**：缺省路径下段损坏 / 指纹不符 / 协议不自洽时告警一次并自动新开
  一段、旧段原样保留、退 0；显式 `--resume` 仍按三道判据报错退 2。显式
  `--session-file <路径>` 语义不变（本机制只作用于「不给路径」的缺省情形）。
- **sim 地图缺省落家目录**：场景路径 `argv[4] > MYBOT_SIM_SCENE > 缺省裸名 default`，
  裸名解析为 `$MYBOT_HOME/maps/<名字>.scene`，缺失时从仓库模板 `sim/scenes/<名字>.scene`
  播种（临时文件 + rename、`0600`、绝不覆盖）；仓库 `sim/scenes/*` 从此只读。取不到
  家目录 / 模板 / 建目录或播种失败则退回内置默认场景并告警。
- **配套**：`sim_agent_demo.sh` 记 `last_robot`（下次续用同一只机器人）、缺省给
  `--new-session`（`DEMO_RESUME=1` 演示续接）；`test/run_all.sh` 与 `agent_e2e.sh`
  把状态目录隔离到临时目录（R18 守「缺省新建 / 续接 / `--new-session` / 损坏降级」）。
- **不影响**：REPL 行历史仍走既有 XDG 位置；MCP 协议、工具门控与安全语义零改动。

### 2026-10-03 仿真器场景 v3：盒子可转向，落点做几何解算因而能叠放

- **场景 v3：`box` 行追加可选 `yaw`**，盒按 OBB（有向包围盒）参与碰撞、测距、拾取与支撑面
  解算；被搬运的物体跟随机器人朝向（旋进盒局部系复用轴对齐数学、再旋回世界系）。v1/v2 场景
  照读，`yaw` 缺省为 0。
- **`World::place` 从「放到前方 0.5m」升级为一次几何解算**：落点与「墙」在 XZ 上重叠时，沿
  机器人前方**只向近侧**水平推开；落地高度取脚印下最高支撑面。于是「机器人站上箱顶松手」
  就能把箱子叠上去，平地上则一律贴地并推开被占的箱子，**箱与箱永不重合**。
- **已知限制（记入 `docs/sim.md`）**：静态箱无重力——取走堆叠的下层后，上层会悬空留着。
  同日的下一条把「抽取被压住的下层」改判失败，正是为了不让这个限制以穿帮画面的形式出现。
- **提示词同步收紧**（`prompts/sim_carry_task.md`）：前进任务改为只用 `sim_fwd` 小步走
  （每次 ≤1m、累计约 ≤5m，测距连续两次 10m 即停），禁用 `sim_forward`；补「拾取只看朝向与
  <0.6m 距离、无体积/质量上限」；说明相机是固定第三人称视角、方向判断以拍照回执里的测距
  读数为准。此前蜘蛛会一路冲出障碍区。

### 2026-10-03 C++ 宿主领域门面化：十个提交把 `RobotStruct` 从上帝类拆成可单测的门面（行为保持）

- **`src/robot.cpp` 772 → 225 行、`src/main.cpp` 348 → 261 行**，`src/` 新增 12 个文件
  （90 → 102），整批 35 个文件 +1654/−1062。`RobotStruct` 此前一个人扛着四件事：生命周期、
  结构矩阵容器、元注册表、守护线程注册表 + 停机栅栏，外加热插表的整套提交状态机——每个域
  都没有可单独实例化的入口，想测就得把整个机器人连真机资源一起建起来。
- **五个域各自成门面**：`StructureStore`（结构矩阵 + `deque` 引用稳定性契约）、`MetaRegistry`、
  `DaemonRegistry`（含从 `robot.cpp` 搬过来的守护停机栅栏）、`HotSqlService`（热插表的提交 /
  增量装配 / 上电 / 热删除 / 整表撤下 / 生效摘要；`applySql` 退化为「判生命周期 + 转交」）、
  `ContextService`（`context.get` 的材料装配，把动作与后端管线的细节从 MCP 路由里摘掉）。
- **生命周期状态显式化**：`bool deactivated_` → `LifecycleState{Loaded, Active, Deactivating,
  Inactive}`，转移收进门面：`active()` 拒绝在 Active/Deactivating 上重复上电（防既有变量二次
  build）、`deactivate()` 天然幂等（显式重复调用与析构兜底都不会二次 destroy）、`run()` 只拒绝
  Inactive。**`Loaded` 刻意放行**——`run()` 的解析/注册/派发前校验本就不申请真机资源，那是
  `test_robot.cpp` 顶注写明的「离线预演」可测契约，锁进 Active 会改掉它。
- **入口与协议层**：`RobotApplication` 把动作管线与 MCP 管线的执行 + 统一清理搬出 `main.cpp`，
  入口只剩启动准备与模式选择；`src/mcp_protocol.hpp` 收拢 C++ 侧跨进程字段名，Rust 侧新增
  `agent/src/protocol.rs` 承载类型化载荷（帧格式与字节不变），并把「读源码文本比对 schema」的
  测试换成运行期的响应契约用例。
- **测试专用的后门关掉**：`RobotStruct` 新增 `structures()`/`metas()`/`daemons()` 三个只读门面
  访问器，下线 `matrices()` 与 `declaresVariable()`（后者逻辑迁入 `StructureStore`）；只为测试
  而活的 `metaProvidersMatchLiveRows()` 移出生产类，改由测试经两个门面在本地组装。全部调用点
  （`agentview`/`main` 与四个测试文件）改经门面。
- **对使用者意味着什么**：无。SQL 语法、MCP 线协议字节、工具集、配置字段、退出码全部逐字
  保持，十个提交各自独立全绿（当时 619 通过 / 0 失败）。这批门面在次日各补了直接单测
  （`test_structure_store.cpp` / `test_hw_registry.cpp` / `test_meta_registry.cpp`）——**先拆出
  可实例化的入口，才谈得上给单个域写测试**，这是本轮 C++ 侧的主要收益。

### 2026-10-03 仿真器退出即保存：ESC / 关窗 / Ctrl-C / SIGTERM 走同一条路

- **改了什么**：四种退出方式（ESC、点窗口 X、Ctrl-C、demo 拆链路时发的 SIGTERM）现在统一
  「先 `saveScene()` 再退」。`main.cpp` 把 SIGTERM/SIGINT 折成一个 async-signal-safe 的关窗
  标志（信号处理器里不碰 GLFW/IO），主循环轮询到即退出。未配置场景路径时 `saveScene` 自行
  no-op；SIGKILL 不可捕获，故那种情况下不保存。
- **顺带修一个被掩盖的退出段错误**：`Shader`/`Mesh`/`Spider` 的析构里是 `glDelete*`，必须早于
  `glfwTerminate`。此前 `main` 末尾显式 terminate、而它们是更早声明的局部量（析构序在 terminate
  之后）——只因 demo 一直用 SIGTERM 瞬杀、析构根本没跑才没暴露；改成优雅退出立刻崩（rc=139）。
  修法是早声明一个 RAII 守卫、由它在最后 terminate（析构序 = 声明序逆序），先释放全部 GL 再关
  GLFW。
- **对使用者意味着什么**：agent 搬动过的环境从此在 demo 正常收尾后存续（此前 SIGTERM 一杀就
  丢）。**副作用要知道**：每次跑 demo 会续用上次保存的场景，想从原始布局开始得另指一个临时
  场景文件或先复位。验证：SIGTERM 退出 rc=0，`fdist 1.0` 后场景里 robot 行 0 → 1.0049。

### 2026-10-03 `run_action_script` 加 `vars`：调用时注入会话别名（像函数传参）

- **改了什么**：`run_action_script` 新增可选 `vars` 对象，键值在本次 `run()` 的 `SessionScope`
  内绑成临时别名，脚本里 `$(键)` 就地取值、**出口即撤销，绝不跨 run 残留**。绑定发生在内联
  `@Alias`/`@Vars` 之后，故调用方实参覆盖脚本内默认值。键过 `isSafeIdent` 并受既有 override
  守护；`vars` 参数错误按「工具参数错误」回传（`executed=false`），不谎报「停在半途」。
- **动机**：sim agent 转向抖动的根因是旧脚本按**时长**转、角度不可控。有了 `vars`，一个
  参数化脚本就能精确转任意角度——`sim_turn.mt`（`turn $(deg)`）配 `vars={"deg":35}`、
  `sim_fwd.mt`（`fdist $(dist)`）配 `vars={"dist":0.5}`，替代一堆写死角度的脚本。
- **配套**：`robots/spider_sim.sql` 补 `P→param` 别名（精确 turn/fdist 的参数列才有着落）；
  新增 `scripts/sim_turn.mt` / `sim_fwd.mt` / `relay-test-var.mt`；`prompts/sim_carry_task.md`
  与 `docs/agent.md` 同步。
- **对使用者意味着什么**：纯加法。不传 `vars` 时行为与之前逐字相同；sim 线上协议、三条 socket
  路径、动作码全未动——参数化发生在宿主侧的 `.mt` 求值，线上仍只发「动作 + 数值」。守门：
  `run()` 级 5 例 + 工具级传值/参数错误各 1 例，并用 socat 抓包确认 `P→param→Sim.lua` 真的
  发出了 turn/fdist。

### 2026-10-03 仿真蜘蛛跳跃：竖直动力学 + 支撑面 + 高度感知碰撞

- **新动作码 10=jump**（`jump <米> <高度>`，两个数值参数）：向前弹道跳，可跳过矮
  障碍、跳上箱子。纯加法——线上语法、三桥路径、动作码 0-9 全不变（冻结契约「只增
  不改」）。`SimEvent` 增 `param2` 承载第二参数，`bridge.cpp` 严格解析双参、坏值整行
  忽略；`action_table` 增 `needsParam2`，`spider_sim.sql` 增 `Q→param2` 别名，
  `modules/Sim.lua` 增 `CODES[10]`/`PARAM2_CODES`（`sim/tests` 交叉校验随之扩展）。
- **仿真首个竖直动力学**：`Robot` 加 `y_/vy_/airborne_`，起跳折算竖直冲量
  `vy0=√(2g·height)` 与水平发射速度，空中水平速度守恒做弹道、朝向冻结，落地由
  `applySupport` 吸附。`baseTransform` 带上 `y_`，整只蜘蛛（含腿）随跳跃离地。
- **支撑面 + 高度感知碰撞**：`World::supportHeight` 每帧算脚下最高箱顶（无箱则地面
  0），据此落地/走下箱沿自动坠落；`resolveCollisions` 把顶面不高于脚下的障碍当「地板」
  不推、只有比脚高的「墙」才推——故跳够高能越过/落上箱顶，不够高被挡回。2cm 台阶
  容差意味着地面行驶几乎不自动爬箱（碰撞语义与加跳跃前一致），要上箱必须跳。
- **不影响**：射线测距仍是 XZ 平面读数；搬运物 y 随机体升降；相机跟随带上机体 y。
- `.mt` 侧 `scripts/sim_jump.mt`（第二参数列 `sim.Q`）配
  `run_action_script(vars={"dist":..,"height":..})` 注入。

### 2026-10-02 仿真器多机器人：搬运状态下沉到每只 Robot（C1）+ 每只一套 socket（C2）

- **C1（核心内部实体化）**：搬运状态（`carriedIdx`/`carriedMass`/`effectiveMaxSpeed`）此前是
  `World` 级单例，两只机器人会互相覆盖——核心里最后一处写死单机器人的地方。现下沉到每只
  `Robot` 各自持有；`World` 去掉这几个成员与 `isCarrying`/`carriedIndex`/`carriedMass`/
  `effectiveMaxSpeed`，碰撞、测距、拾放、背负跟随全改为按传入的 `Robot` 参数化的**无状态**
  交互（`pick`/`place` 由 `const Robot&` 改 `Robot&` 以写回各自的搬运态）。`SimEngine` 改持
  `vector<Robot>`、`step()` 逐只推进。
- **C2（上线）**：场景 v2 允许多条 `robot` 行决定机器人数量（`readScene` 兼容 v1、`writeScene`
  一律写 v2）；壳按 `robotCount()` 建对应套数的命令桥与测距桥——**robot 0 沿用原缺省路径
  （字节不变、旧客户端无感），robot i>0 在 `.sock` 前插 `-i`**；拍照桥仍单条。新增
  `sim/scenes/two_robots.scene` 示例，`docs/sim.md` 补「多机器人」节。
- **对使用者意味着什么**：线协议、三条缺省路径、动作码全部不变，`modules/Sim.lua` 与
  `Camera.lua` 一行未改——多机器人是**加法式**扩展（场景填多只 + socket 路径派生），不是协议
  改版。要驱动第 2 只，把它的 `MYBOT_SIM_SOCK` 指到 `/tmp/mybot-sim-1.sock` 即可。
- **守门**：`testMultiRobot`（两只在同一 `World` 各自拾取/背负/按自身质量减速/跟随/放置，
  互不覆盖）、`testEngineMultiRobot`（两只各拾各的、朝相反方向各走各的，验证 idx 隔离）、
  `testSceneIo` 重写（v2 多 robot round-trip、v1 兼容、缺 robot 行补默认、carried 越界归一、
  v2 魔数）。

### 2026-10-02 修复：搬运放下后方块悬在空中

- `place()` 只改了落点的 x/z、没还原 `center.y`——物体被 `updateCarried` 抬到身体上方后，
  放下时那个抬高的 y 留着不动，方块就悬在空中。现把 `center.y` 还原到静置高度 `half.y`
  （底面贴地 y=0，与场景 `box` 行的约定一致）。`testPickPlace`/`testSimEngine` 各加一条
  落回地面断言。

### 2026-10-02 仿真器面向扩展性重构：动作表、请求-应答桥、引擎化、共享常量、编译期派发守门（行为保持）

- **动作单一事实源**：新增 `sim/src/action_table.{h,cpp}`，把每个动作的 `{码, 线上名, 别名,
  持续型/一次性, 是否带参}` 集中成一张表；命令桥 `handleLine` 改为查表分派，删掉 11 分支的
  if-else 字符串链。加一个动作从「改枚举 + 改字符串链 + 改派发」收敛为「表里加一行」。
- **请求-应答桥抽基类**：新增 `RequestResponseBridge`，封装拍照/测距两桥的共性（请求标志、
  关键字匹配、poll 口径、EOF/断开复位、ERR 帧），`CaptureBridge`/`SensorBridge` 收敛为只补
  各自 OK 帧化的薄子类。新增一个同步传感器 = 派生 + 一个 `sendXxx`（由 `PingBridge` 最小
  示例守门）。
- **引擎化（核心脱开 GL）**：新增 simcore 的 `SimEngine`，把散在 `main.cpp`（GLFW 壳）里的
  `commandToInput`、场景装卸与每帧物理（相对运动、位姿更新、碰撞解算、搬运跟随）收拢进来；
  `main.cpp` 441 → 322 行，只剩窗口/输入/渲染/抓帧的薄壳。于是 fdist 到点、turn 转角、新持续
  指令打断相对运动、pick/place、save→挪动→reload 往返都能在**无窗口**下断言（`testSimEngine`）。
- **共享常量单一事实源**：三条缺省 socket 路径落成 `sim_config.h` 里的 `inline constexpr`
  （原先是 `main.cpp` 里各写一遍的字面量，头文件里只有一条注释掉的 TODO），可调参数一并集中。
  Lua 侧仍各写一份——两套构建系统之间**刻意不做代码生成**（跨 build 生成脆弱），改由
  `sim/tests` 的 `testSharedConstants` 运行期读 `modules/Sim.lua` 交叉校验字面量一致。拍照
  路径 `Camera.lua` 只取自 env、无硬编码缺省，按设计不参与校验。
- **编译期派发守门**：`SimCommand` / `SimEvent::Type` 加枚举值后若忘了在 `commandToInput` /
  `applyEvent` 补 case，动作会**静默 no-op**，而既有测试测不出（表驱动派发只验命令桥入队/置态、
  不验引擎行为）。这两处 switch 刻意不写 `default`，并对本地三个 target 单挑 `-Werror=switch`
  升级为错误——漏 case 直接编译不过。第三方头不受影响（`external/stb` 改标 SYSTEM include 压掉
  它自带的噪声告警，glfw/glm/glad 是独立 target、不继承这些选项）。
- **对使用者意味着什么**：无。线上协议、三条 socket、数字码、场景文件 v1 全不变；这批重构的
  意义在于「加一个动作 / 加一个传感器」的成本与漏改的可见性。次日那条零编译守门正是接着这里
  做的——`sim/tests` 不在 CI 的构建范围内，运行期交叉校验等于没人跑。

### 2026-10-02 Lua 侧 AF_UNIX 连接样板收成公共模块

- `modules/Sim.lua` 与 `modules/Camera.lua` 各自 `ffi.cdef` 了一份 socket 系统调用
  （`socket`/`connect`/`send`/`recv`/`close`/`errno`），两份声明与两段连接样板并行维护。现抽出
  `modules/lib/unix_socket_ffi.lua`（新增 160 行）承载 cdef 与 `connect/send/recv/close`，两个
  驱动改为调用它（`Camera.lua` +22/−53、`Sim.lua` +18/−47）。
- **对使用者意味着什么**：行为不变（同样的系统调用、同样的 `MSG_NOSIGNAL`、同样的 errno 口径）。
- **收敛没覆盖到的部分，记在这里备查**：`poll` 没进公共模块（各驱动的 `pollfd` 结构名不同），
  `Sim.lua` 因此仍留着自己的一段 cdef——而 `int errno(void)` 这条伪声明在**公共模块与那段里
  各有一份**，2026-10-04 那条段错误修复因此要改两处。glibc 的 `errno` 是数据符号（宏
  `(*__errno_location())`），按函数调等于跳到一个 int 值当代码执行。

### 2026-10-02 相对运动精度：到量刹车 + fdist/turn 顺序执行

- **到量刹车**：`tickRelative` 在剩余量小于一阶滞后的滑行尾程（`v/kStopRate`）
  时提前松油门，实际位姿贴着目标。旧行为是满速到量才松开，减速尾程会多滑
  `v/6 ≈ 0.27m`、多转 `ω/6 ≈ 21°`——`turn 45` 实停 67°，场景保存把失真位姿
  一并落盘后才暴露。`kStopRate`（6/s）作为 update() 滞后常数与刹车尾程估算
  的单一事实源。
- **fdist/turn 顺序执行**：`setRelativeTurn` 不再清掉 `remainingDist`，两通道
  各记各的、先走完再转。旧行为下 `.mt` 里「fdist 2 后 turn 90」两行连续下发
  （同帧到达）时走路段被静默丢弃。同通道新目标覆盖旧目标、持续型指令打断
  相对运动的语义不变。
- **守门**：sim_tests 扩到 109 例（到量后 90 帧尾程不再多滑/多转的两条回归、
  fdist+turn 同帧设定的顺序执行）。

### 2026-10-02 仿真场景数据化：环境与代码解耦 + 可保存

- **场景文件**：障碍布局从 C++ 硬编码挪到 `sim/scenes/default.scene`（行文本
  v1：魔数行 + `box`/`robot` 行，注释/空行忽略，坏行告警跳过，mass 缺省按
  体积×800kg/m³）；`readScene`/`writeScene`/`World::applyScene` 落在 simcore，
  C++ 侧 `World::defaultScene()` 保留为无文件时的内置回落。
- **路径与保存**：场景路径 `argv[4] > MYBOT_SIM_SCENE > 内置`（与三桥同规矩）；
  `S` 键或命令桥 `save` 一次性事件（Sim.lua 动作码 **9**）把当前世界状态
  （障碍+机器人位姿+搬运索引）**原子写回**（tmp+rename）。线上不收路径参数，
  只写启动时配置的文件——沿用「不给 socket 任意路径原语」的安全口径。
- **R 键升级**：复位从「只归零机器人」改为**从场景源头整体重载**（文件版重读
  文件、内置版回默认），被搬走的障碍可恢复。
- **demo 接线**：`sim_agent_demo.sh` 缺省给 sim 传 `sim/scenes/default.scene`，
  agent 搬动过的环境可存续。
- **守门**：`sim/tests` 扩到 105 例（场景 round-trip、容错解析、缺魔数拒绝、
  carried 越界、原子写不残留 tmp、save 事件）；`test_sim.lua` 扩到 32 例
  （fdist/turn/pick/place/save 线上逐字核对，越界上界随码表上调）。

### 2026-10-02 搬运任务链路 + 仿真器核心重构

- **搬运任务链路**：命令桥增 `fdist <m>`/`turn <deg>`（相对运动到量自停）与
  `pick`/`place`（拾取前方 0.6m 内最近障碍、按质量折算减速、放置前方 0.5m 处，
  动作码扩到 0..8）；新增第三条 AF_UNIX 测距桥（`range\n` → `OK <mm>\n`），
  `Sim.lua` 的 `range()`（仿真器缺席=响亮 ERR）与 `capture_image` 的照片描述
  附加（C++ 侧 `querySimRange`）共用它；蜘蛛碰撞改为球-AABB 推出（被搬运物
  豁免）；`prompts/sim_carry_task.md` 配 `scripts/sim_pick.mt`/`sim_place.mt`。
- **仿真器核心重构（分四步提交，行为由测试锁住）**：
  - **simcore/GL 壳分层**——GL 无关核心（运动学/场景交互/三桥）拆 `libsimcore`
    静态库，新增 `sim/tests/`（85 例，无 GL 依赖即可跑），`build_sim.sh`
    构建即 ctest，坏核心逻辑过不了构建；simcore 后续可单独挂 CI。
  - **`UnixLineServer` 统一三桥公共骨架**（路径/bind/新顶旧 accept/按行派发/
    带 deadline 发送），EOF 语义统一为「延迟断开」——修复 capture/sensor 对
    「发完即关写端」（socat 手测模式）的请求会被 EOF 连带清掉的潜伏 bug；
    发送 deadline 统一 3s。
  - **`World` 承载场景交互**（障碍场景/碰撞解算/射线测距/拾取放置背负），
    `Robot` 回归纯运动学（`update` 显式收 maxSpeed）；Obstacle、碰撞半径、
    默认场景数据迁出 robot.h 与 main.cpp，物理常量归位。
  - **命令桥改「持续状态 + 一次性事件队列」双通道**：fdist/turn/pick/place
    逐条入队、主循环 drain 到空（队列上限 32，断连作废未执行事件）——从结构
    上修掉「同枚举重复下发（连续两次不同距离的 fdist、两次 pick）第二条被
    静默吞掉」的问题。
- **文档**：新增 `docs/sim.md`——三条 socket 协议、动作码表、成败口径、
  simcore/GL 壳分层与 socat 手测法的单一事实源；`Sim.lua` 内命令/sensor 两段
  连接样板合并为 `unixConnect`。
- **守门**：`test/test_sim.lua` 27 例全过；`sim/tests` 85 例（含碰撞推出、
  连续两次 fdist 均入队的回归守门、半关写端请求不丢）。另修 `Sim.lua` 的
  `destroy` 先于 `dropSensor` 定义导致关连接报错（Lua 局部函数前向声明）。

### 2026-10-02 仿真蜘蛛机器人：simulator 迁入 + 语义动作桥接 + 仿真内拍照

- **仿真器迁入仓库**：原独立项目 mybot-simulator 复制为 `sim/`，改为独立 CMake
  工程（`project(mybot-sim)`，产物 `sim/build/mybot-sim`），根 CMakeLists 不
  `add_subdirectory` 它——CI 镜像无 GL/X11、musl 亦无法构建 GLFW，挂靠会拖红
  全量目标构建；构建入口 `scripts/build_sim.sh`（仿 build_agent.sh）。
- **小车换成蜘蛛**：删除差速小车的外观与键盘驾驶，改为程序化四足蜘蛛
  （`sim/src/spider.cpp`，trot 步态由 bodySpeed 驱动相位）；差速运动学保留在
  `robot.cpp` 作为底盘位姿来源。蜘蛛在语义动作层被驱动，不暴露关节角。
- **AF_UNIX 命令桥**：`sim/src/bridge.cpp` 起单客户端行文本服务，收
  `forward|backward|left|right|stop [speed]`，命令持续到下一条为止，控制器断开
  即停蜘蛛。socket 路径单一事实源为 env `MYBOT_SIM_SOCK`（默认
  `/tmp/mybot-sim.sock`），sim 与驱动两侧同读。
- **仿真内拍照（agent 能「看」仿真画面）**：第二条 AF_UNIX 取帧桥
  `sim/src/capture.cpp`（一问一答：收 `capture\n`，SwapBuffers 前 `glReadPixels`
  抓 GL_BACK → 垂直翻转 → public-domain stb_image_write 编 JPEG → 回
  `OK jpeg <len> <w> <h>\n`+体）。`modules/Camera.lua` 增第三来源 sim-live：
  env `MYBOT_SIM_CAPTURE_SOCK` 非空且无文件 src 时走它，`source="sim"` 回传。
  socket 路径只取自 env（杜绝「连任意 socket」原语）；与 Sim.lua 动作的静默成功
  相反，拍照抓不到即响亮 ERR。`spider_sim.sql` 注册 `Camera 'cam0'` 使宿主
  `capture_image` 工具出现。与命令桥分两条 socket：Sim/Camera 是各自独立驱动
  实例，无法共用一条连接。
- **mybot 侧零 C++ 改动**：新增 SQL 表即挂上通用「表名→`modules/<表>.lua`」
  注册链——`modules/Sim.lua`（驱动，惰性连接、无监听端时静默成功、坏动作码响亮
  ERR、send 带 MSG_NOSIGNAL 防 SIGPIPE）、`robots/spider_sim.sql`、
  `scripts/sim_*.mt`（动作码 0=停 1=前 2=后 3=左 4=右，S=speed 0~1）。
- **守门**：新增 `test/test_sim.lua`，并在 `test/test_camera.lua` 增 sim-live 用例
  （fork 子进程充当假仿真器做请求-响应回环）；用例名单与计数以
  `bash test/run_all.sh` 实时输出为准，不在此复述。

### 2026-10-01 审计批次 A：迁 Rust 残留清退 + 安全设限三补（收口）

- **promptpack 死模块退场**（P1-4）：`loadPromptPack` 在 src/ 内零调用者
  （`--prompt-file` 已被 cliargs 判 loop-only 拒绝），src/promptpack.{cpp,hpp}
  （227 行）与 test_promptpack.cpp（16 例）删除，CMakeLists 三处登记与 harness
  作用域表同步退场；C++ 单测 630→614。
- **llmconfig「批次 2」落地**（P1-5，agentmode.hpp 自计划项）：模型侧 20+
  字段（driver/var/endpoint/model/api_key/采样/预算/重试/thinking/prompt_files/
  session_file 等）的解析与 llm_limits 模型侧区间常量随 agent 循环迁 Rust
  退役——本侧解析后无消费者，唯一事实源是 Rust config.rs。`LlmFileConfig`
  只保留后端真实消费面（scripts_dir/摄像头/审计/策略/能力门控/工具层超时）。
  **白名单 kKnown 全量保留**：两端共用一份 mybot.llm.json，模型侧键仍是
  mybot-agent 的合法配置，C++ 只登记不校验（类型/值域坏值由 Rust 独占拒绝，
  新增 pass_through 用例 18 例钉住）。R5m 互钉面如实收窄：字节预算摘要行与
  test_cross_lang_limits（C++ 不再镜像任何 Rust 常量）退役，类型错镜像改用
  audit_log（双消费字段）。
- **agentaudit 死字段清退**（P1-6）：kind="model" 分支、session 行的
  driver/model/endpoint/api_key_source/resumed 与 adoptSessionId/sanitizeEndpoint
  在 C++ 侧零写入者（模型轮次审计由 agent/src/audit.rs 写进 agent 侧文件，
  agent_e2e 已钉住），随 10 个死字段一并删除。
- **安全设限三补**（P1-1/P1-2/P1-3）：
  - Rust 审计日志新建以 0600 建立（此前随 umask 常 0644，而 model 行的 error
    字段可能带回显片段）+ 既有宽权限文件响亮提醒一次（判据与
    session::perms_warning 同口径）。
  - 会话文件与 ICA sidecar 读取前 metadata 设限（上限与 BUDGET_BYTES_MAX 同源
    64 MiB）——误指向大文件不整份进内存，与配置 64 KiB/提示词 1 MiB/密钥
    64 KiB 的处处设限纪律对齐。
  - C++ `sendFrame` 写侧 deadline（对齐 Rust 0333f1a 的修法）：MSG_DONTWAIT +
    短拍休眠 + 进程内绝对 deadline，移除对 SO_SNDTIMEO setsockopt 的依赖
    （其失败只告警的路径上防线静默缺位）；刻意不用 poll(POLLOUT)——AF_UNIX
    在对端接收缓冲已满时仍报告可写，实测恒就绪而 send 永久阻塞。
- **顺手收口**：config 的 llm-max-turns 区间改引 limits::MAX_TURNS_MIN/MAX
  （区间单点）；hook::poisoned 的裸 eprintln 改检查式写出 errln；config 头注
  「命令行 > env > 文件 > 缺省」按字段分列如实陈述（env 层只覆盖 4 个变量，
  api_key 方向与其余字段相反）；dump_consts.lua 自 test/ 根移入 test/lib/
  （phase5 引入的存量 selftest 红，构建期工具误放测试用例目录）。
- 收口：干净重建零告警、run_all 269 通过/0 失败/1 跳过、ASan/UBSan 双跑全绿、
  mcp_e2e 354 项、agent_e2e R1–R17、cargo 四检全绿（Rust 单测 118→120、
  C++ 单测 630→611）。

### 2026-10-01 Rust agent 结构治理二批 + CliOptions 分组化（行为保持）

- **解 `bridge ↔ ica` 类型环**（上轮 Phase 3 项 8 关联遗留）：`ToolSpec` 自
  bridge.rs 下沉叶子模块 toolspec.rs（placeholder.rs 解环同款手法），ica/specs
  的工具 schema 声明不再反向依赖传输层；bridge/provider/main 改引新路径。
- **ICA 收边**（上轮可选项落地）：BOT4CJ_ICA 开关读取、账本持有与
  观察/清单/清空/落盘/恢复收进 ica 模块的 `IcaSession` 门面（锁中毒统一
  hook::poisoned），bridge 只留工具清单合并与 ica_* 派发两个取用点，
  repl/main 经 `bridge.ica` 使用——启停判据归 ica 单点。
- **循环语义可测接缝 + 测试补强**（上轮 Phase 4 残留，单测 111→118）：
  rig 的 HookContext 为 pub(crate) 构造，钩子方法进不了单测——判定核心
  （判定点①②墙钟/停止/单轮上限）自钩子方法抽成 `gate_completion`/
  `gate_tool_call` 状态接缝。新增：hook 循环语义五例（单轮上限随 turn 重置、
  0=不限、停止锁存跨 begin_task、两处墙钟文案逐字钉住、begin_task 清观测
  不清锁存）、ica/persist 落盘往返（0600 权限 + 会话长度漂移拒恢复）、
  main 的 resume 判据接缝（`resume_from` 抽出，临时 listener 离线构造 Bridge，
  缺文件 fail-closed 与头不符拒恢复两例）。repl 的 run_turn/drive_stream
  单测接缝评估后不做：编排路径已由 agent_e2e R1–R17 双进程场景覆盖，单元
  接缝需重排 Session 内部，收益低于改动面（config/file.rs 的区间校验面已由
  config::tests 的 bad.json 表驱动用例群覆盖）。
- **HookState 治理收口**：判定核心已入 gates；pub 面收紧 pub(crate)（六个
  消费者全在本 crate）。字段分组评估后**保持单结构**：12 字段同属一轮任务
  生命周期（begin_task 一并清零），拆子结构只加间接层不改任何消费者契约。
- **repl 拆 slash 域**：/help /clear /status /hardware /scripts /sql 命令
  分派、SQL 变更语句甄别（batch_has_write_ops，判据同 C++）与用法/帮助文案
  析出 repl/slash.rs（子模块形态，经 super::Session 调会话方法，不引跨模块环）。
- **config build() 三段取值链拆解**（227→约 130 行）：协议驱动/变量名、
  endpoint/model、API key 三段各成 Result 函数，build 只留装配。
- **CliOptions 分组化**（45 字段三模式域）：actionsource.hpp 的 CliOptions
  按域分组成嵌套子结构——tcp（TCP 动作源 7 字段）、agent（agent 模式 27 字段，
  MCP 后端复用其工具上下文门控）、mcp（后端模式 6 字段），全局/输入源 5 字段
  留顶层；模式布尔改组内 enabled，其余字段名不变（调用点只加一段路径）。
  改写采用接收者限定（o/opts/options/o2 均核实为 CliOptions；同名无关字段
  如 AgentModeConfig.cameraSrcDir 明确不动），约 140 处调用点机械迁移。
- 收口：干净重建零告警；run_all 269 通过/0 失败/1 跳过、ASan/UBSan 双跑
  同口径全绿（C++ 630 单测）；cargo 四检全绿 + agent_e2e R1–R17 通过
  （Rust 118 单测）。

### 2026-10-01 C++ 伞头收窄 + TU 二连拆 + 两项评估收口（行为保持）

- **mcpbackend 域拆分**：mcpbackend.cpp（617→268 行）留守路由类 McpBackend；
  serveConnection/断连安全动作（--on-disconnect）/acceptLoop/runMcpBackend 析出
  mcplisten.cpp（396 行）；帧传输与纯协议件声明自 mcpbackend.hpp（253→188 行）
  析出 mcptransport.hpp（88 行）——声明归属与实现 TU 对齐，mcplisten 只经公开面
  驱动路由类。
- **agent_subprocess 三分**：环境清洗域（敏感词判据/清洗/抹零，即 agentaudit
  复用的判据锚地）→ agent_envscrub.cpp；子进程侧 fd 卫生与 execChild →
  agent_fdguard.cpp；spawn/pump/reap/回执格式化留守；跨 TU 接缝入新建
  agent_subprocess_internal.hpp（agenttools_internal.hpp 同款形态）。
- **伞头声明析出**：daemonconf.hpp（interval 解析与 chunk 路径寻址，自
  daemonvar.hpp）、agentview.hpp（可读变量清单判据/回执渲染/清单描述，自
  agenttools.hpp）——消费者（hw_factory/hotsql_plan/agentmode/mcpbackend 等）
  改引窄头；capture_image 的 kMaxImageBase64Bytes 随装配判据入 internal 头。
- **wantXxx 六同形校验器骨架单点化**（上轮 Phase 2 项 6 遗留可选项）：
  「字段缺失 → 通过」+「类型不符 → typeError」两步收敛为 fetchField 单点
  （类型判据谓词表传入），各校验器只剩自己的类型语义（空串口径/值域/取整/
  元素遍历），错误文案逐字不变。
- **Lua 头扇出余量核查（Phase 10-2）**：daemonvar.hpp 对 Lua 符号的引用只在
  注释里——摘除 luaend.hpp 传导后 robot/daemonconf 两个 TU 脱离 LuaJIT 头
  编译集（gcc -H 实证 0 引用；11→9 个 TU 碰 Lua 头），daemonvar.cpp 自引。
  评估后刻意不动：hw_registry（luavar.hpp 的 MtVars_Lua 构造接口直接收
  lua_State，切断需拆 MtVars_Alias/Vars 窄头，接口手术收益低）与 agenttools
  （luaexec.hpp 为预算钩子挂摘点，phase6 已判真依赖）。
- **秘密判据「双轨」评估（Phase 10-1）——判不改，已是单点分层**：
  agentaudit 的 maskedWord = isSensitiveEnvName ∥ {authorization, bearer}——
  基础词表（kWORDS/kCOMPOUNDS）与整词切词法（util::anyWordOf）均复用环境清洗
  单点（现 agent_envscrub.cpp），仅遮蔽词是命令文本特有扩展（注释自述「没有
  叫这个的环境变量要挡」）。两处威胁模型（子进程环境 vs 命令文本回显）刻意
  分层，共享底座不漂移。
- 收口：干净重建零告警、run_all 269 通过/0 失败/1 跳过（与基线一致）、
  ASan/UBSan 双跑全绿、C++ 单测 630。

### 2026-10-01 测试侧上帝文件拆分：test_robot 七分 + test_agenttools 六分 + 传输/方言域析出（行为保持）

- **test_robot.cpp（2812→311 行）一拆七**：结构装配与 run() 前置校验留守，
  派发/注册表域 → test_robot_dispatch.cpp（@Reflex 并入）、生命周期与硬件
  独占 → test_robot_lifecycle.cpp、热 SQL 只读计划层 → test_robot_hotsql_plan.cpp、
  热插/热删撤/热更新 → test_robot_hot{insert,delete,update}.cpp；跨域共用
  夹具（kBasicSql/sqlError/runTexts/loadMatrices/cell/markerOf/probeSql/
  hotSqlError）上收 robot_fixtures.hpp（namespace rfx，各 TU 以 using 引名，
  用例体零改动）。125 用例数守门不变。
- **test_agenttools.cpp（2242→444 行）一拆六**：镜像 src 侧 TU 边界——wait 域 →
  test_agenttools_wait.cpp、SQL/硬件清单 → test_agenttools_sql.cpp、
  capture_image → test_agenttools_image.cpp、run_command →
  test_agenttools_runcommand.cpp、子进程与动作脚本预算 → test_agenttools_subproc.cpp；
  readAll/makeRelayRobot/writeFile/execText 上收 agenttools_fixtures.hpp（afx），
  test_mcpbackend.cpp 的同款副本一并消除。73 用例数守门不变。
- **test_mcpbackend.cpp（814→约 400 行）析出 test_mcptransport.cpp**：镜像
  src/mcptransport.cpp 边界（帧编码/UTF-8/SO_PEERCRED/socket 占用探测/
  空闲 deadline/Rust 孪生契约钉子/首帧认证三态/AuthFailGate）；共享假工具与
  回执判读助手（echoTool/countingTool/respOk/respData/makeBackend 等）上收
  mcp_fixtures.hpp（mfx）。
- **test_sqlparse.cpp（923→约 500 行）析出 test_sqlparse_query.cpp**：
  SELECT/WHERE/AND/DROP/UPDATE/SHOW 查询方言段整体迁出；事件流助手
  events() 上收 fixtures.hpp（fx::tokens 的姊妹件）。
- **test_llmconfig.cpp（978 行）评估后不拆**：llmconfig_*/agentconfig_* 两前缀
  按装载/装配面交错排布，NS 夹具（NoKeyEnv/baseOptions）首尾共用，域纯净对应
  src/llmconfig.cpp+agentmode.cpp——强拆需逐用例拣选，收益低于共享头成本。
- 收口：干净重建后 run_all 269 通过/0 失败/1 跳过（与基线一致）、ASan/UBSan
  双跑同口径全绿、C++ 单测 630 与 TH_TEST 分布逐文件核对不变。

### 2026-10-01 守门可靠性：config 单测密钥链 env 竞态加互斥

- **cargo 单测 env 竞态修复**：`build_is_fail_closed…` 与
  `config_debug_and_display_never_leak…` 两个用例都会 remove/set
  `BOT4CJ_LLM_KEY`——cargo 单测默认多线程并行，一方的 remove_var 落在另一方
  set_var 与 `build()` 之间即假红「缺少 API key」（基线复核实跑竞出一次；
  旧注释「测试串行由 cargo 保证」系误记，已改正）。修复：config::tests 模块级
  `ENV_LOCK` 互斥，动密钥链 env 的用例持锁串行；连跑 5 次全量稳定绿。

### 2026-10-01 编译扇出收窄：matrix.hpp 冷路径下放 + hardware.hpp 摘 Lua 头（行为保持）

- **matrix.hpp 冷路径方法下放 matrix.cpp**：buildInherited（惰性继承填充）/
  requireCol/save/appendRow/popRows/removeRows 六段大 inline（约 110 行）从
  11 个 TU 共享的头下放——它们都是解析期/生命周期级调用，不在逐格热路径上；
  operator()/inherited/indexOfCol 逐格热路径留在头内 inline。
- **hardware.hpp 摘掉 luaexec.hpp 传导**：基类只在 armExecBudget 挂接点用
  `const LuaExecBudget*` 指针形态，前置声明即可——此前经 luaexec→luaend 把
  lua.h/lauxlib/lualib 传进半个仓库；真正构造预算/挂摘钩子的 TU
  （agenttools/luavar）自引 luaexec.hpp，依赖图回到「谁碰 Lua 谁引头」。
- 实测（本机干净检出，同机前后各一次）：构建墙钟 52.0s → 50.8s（-2%）、
  user CPU 14m06s → 13m58s（-1%）——收益主要在依赖诚实而非秒表；行为零变化
  （630 C++ 单测 + 269 道守门 + ASan/UBSan 双跑全绿）。量化如实记录，不夸大。

### 2026-10-01 gen_const_check 加固：consts 表改执行期求值（P0 复发预防）

2026-10-01 P0（方案 A 下沉搬走常量、抽取脚本漏改 → 干净检出配置必败）的教训
「抽取模式与被抽文件的书写风格是同一份契约」由此收口：

- **hal_backend_glibc.lua 的 consts 表改执行期求值**：配置期以 luajit 加载后端、
  `test/dump_consts.lua` 倒出 `M.consts` 表（键排序保证输出确定，只收 number 值）。
  抽取契约从「正则模式 ↔ 书写风格」收窄为「后端导出 M.consts 表」——这条本就是
  后端接口注释里写明的契约，书写风格不再有可断的隐式耦合。
- **反向验证两条**：① 把 `O_CLOEXEC = 0x80000,` 改写成 `["O_CLOEXEC"] = 0x80000,`
  （合法 Lua、旧正则会漏抽）——新机制照常抽取；② 移除 `consts = consts,` 导出——
  配置期 FATAL_ERROR 并点名「后端接口契约被破坏」。
- Camera.lua 的 V4L2 常量/ABI 尺寸表与 HAL.lua 波特率表保持纯文本抽取（理由在
  脚本头注：Camera 顶层会执行整套 HAL 加载 + 运行期 glibc 调用，不宜在配置期跑；
  且它们是函数内局部量、无导出接口）。任何一方抽取失效仍响亮失败，不静默降级。
- 新依赖 `luajit` 可执行文件（与 libluajit-5.1-dev 分开装的包）：CI 各 job 早已
  安装（测试期 LUACMD 依赖），README 依赖行同步补上。

### 2026-10-01 测试体系补强：camera 入 agent-all 守门 + Rust 编排层补测（行为保持）

- **`agent-all` 的 Lua 段改走 roster 精确清单，纳入 `test_camera.lua`**：此前改
  `modules/Camera.lua` 或图像链路不会被 agent-all 拦住（摄像头协议面刻意留在
  过滤器外——子串过滤器分不开仿真用例 `test_camera.lua` 与真机的
  `test_camera_v4l2.lua`，纳入会连带真机用例）。现在 `agent_lua_roster` 点名
  仿真用例，Lua 段对 agent-all 按清单精确执行；自证同步升级（①只查存在性、
  ②新增「camera 仿真用例文件在而漏登记即红」的反向检查），`test_camera_v4l2.lua`
  仍只由全量跑/真机上显式跑。docs/testing.md 同步。
- **Rust 编排层补测**（agent 单测 106 → 111）：`SessionAudit` 三个用例钉住
  拆分后的核心语义——turn 映射（rig 1-based → C++ 0-based 口径）、摘要轮
  `track_tokens=false` 不跟进「最近一次 token」、`turn<0` 不写字段的落形、
  session start/end 两行落审计；`placeholder.rs` 的 `source_len` 六变体全覆盖
  与占位判据往返。test_camera 入 agent-all 后该过滤器实测 14 条全过。

### 2026-10-01 结构重构三批：C++ 判据单点化/TU 拆分 + Rust agent 结构治理（行为保持）

第三批（Rust agent）：模块环解环、判据单点化、上帝对象拆分，收口过 cargo
build/test/clippy/fmt 四检（106 测试全绿、零告警）与两进程 agent E2E。
行为保持纪律与前两批一致；唯一的有意行为差为**恢复期跳行告警的落形**
（文案逐字保持、只改构造方式）。

- **解 `session ↔ history` 模块环**：图片占位文本与后注判据、媒体体积估算
  （`image_placeholder` / `is_image_placeholder_notice` / `source_len`）下沉为
  独立叶子模块 `placeholder.rs`——此前 history 引用 session 的占位判据、session
  反向引用 history 的块起点，判据与体积三个口径各抄一份。
- **`BudgetModel` 重试骨架合一**：阻塞面 `completion` 与流式面 `stream` 两份
  近复制的重试循环（stop 检查→attempt→请求→分类→退避→二次检查）抽为
  `retry_open` + 分类修正器参数；顺带修正 `request_bytes_lower_bound` 抄自
  `response_text_bytes` 的错误 doc 注释。
- **mutex 中毒恢复单点化**：bridge.rs 的 ICA 账本 11 处匿名
  `unwrap_or_else(|e| e.into_inner())` 换用 `hook::poisoned`（带 warn 留痕——
  中毒是持锁线程 panic 过的信号，静默恢复会丢掉完整性问题的第一次现场）。
- **`repl::Session` 上帝对象拆分**：audit/stats/started 三字段抽为
  `SessionAudit`（会话统计 + 审计落行），构造参数 11 个打包成
  `SessionAssembly`/`ResumeContext`——全仓两处 `too_many_arguments` 豁免
  之一就此移除；普通轮与摘要轮的 CallRecord→ModelEntry 双循环合一，差异
  （turn/status/是否跟进「最近一次 token」）显式化为参数。
- **`main.rs run()` 再拆**：后端连接 + 提示词素材装配段抽为
  `assemble_backend`（`Err((消息, 退出码))` 收敛 9 个「errln! + return」出口，
  退出码口径逐字保持）；`config::summarize` 纯渲染器下沉
  `config/summarize.rs`，mod.rs 只留 Config/limits/合流。
- **恢复期跳行告警样板收敛**：`load_session_file` 的 8 处「会话文件…第 N 行
  …已跳过该行」手拼收敛为单点 `skip` 闭包。

### 2026-10-01 结构重构两批：安全判据单点化 + 五个上帝文件 TU 拆分（行为保持）

全仓行为保持的结构治理，分两批提交。每批收口过零告警构建（-Werror）、
`run_all.sh` 全量（269 通过 / 0 失败 / 1 跳过，与重构前基线一致）与
ASan/UBSan 双跑；新增/迁移代码以「搬迁不改写」为纪律（指针注释保留原判据
与审计编号锚点）。

- **安全判据四处单点化**（Phase 1）：
  - 恒时字符串比较合一——`util_str.hpp` 的 `safeEquals` 取严格版（长度差折叠
    进累计差、按较长一方全长走完），删除 mcpbackend.cpp 的 `constantTimeEqual`
    副本；TCP 口令与 MCP 首帧 token 两条凭据防线同一单点（审计 M1 + MCP-1）。
  - `AuthFailGate` 跨通道共用——结构体自 mcpbackend.hpp 移 util_cli.hpp（上限
    缺省同 `tcp_limits::kAuthMaxFailsDefault`，TCP 侧经构造参数接
    `--tcp-max-auth-attempts`），actionsource 的手写 authFails 计数换用同一点，
    语义与文案逐字保持。
  - `joinNames` 拼接单点（util_str.hpp）——收敛 robot.cpp 四处与 agentmode.cpp
    一处手写的「点名/成因列表」循环；test_mcpbackend 的本地副本删除、直接用
    生产单点。
  - `Lexer::next` 分隔符阶梯表驱动——五组同构 else-if（')'/'=' 两段带历史缩进
    错层）收敛为 `separatorType` 判据表 + 单分支，行为逐字保持。
  - 评估后不改：FdReadStatus 错误文案翻译层（agentview/mtinclude 两份 switch）——
    守门锚定各处原文（test_mtinclude.cpp、run_all.sh 的 grep 门），且两处异常
    类型/errno 口径刻意不同，util_file.hpp 头注明言文案留在调用方。
- **五个上帝文件 TU 拆分**（Phase 2，均为代码搬迁 + 跨 TU 接缝声明，净
  -1522 行 / +5 个新 TU）：
  - `robot.cpp`（1177→760 行）：HotSqlPlanner 只读计划层整体迁出
    `hotsql_plan.cpp`（hpp 早已独立）；applySql 尾部 30 行生效摘要抽为
    `summarizeHotOutcome()`；顺手清除上次拆分残留的重复注释块。
  - `agenttools.cpp`（796→521 行）：wait_variable 轮询机器（条件解析/判定/
    轮询/回执/装配）迁出 `agenttools_wait.cpp`，经 agenttools_internal.hpp
    接缝挂载；`requireString`/`stringParam` 随之升为模块内接缝。
  - `mcpbackend.cpp`（889→617 行）：帧编解码/deadline 收发/首帧密钥校验/
    SO_PEERCRED/socket 占用探测迁出 `mcptransport.cpp`；FrameResult 与两个
    轮询/超时常量升入 hpp 模块接缝。
  - `daemonvar.cpp`（714→445 行）：@Daemon 配置校验与装配（schema 校验/配置串
    组装/槽分配/Factory）迁出 `daemonconf.cpp`，线程机（DaemonRunner 逐拍循环
    与生命周期）留下；`mutexOf`/`releaseSlot` 升为模块内接缝。
  - `cliargs.cpp`（581→319 行）：全部帮助文案（全文 + 五个 topic + 分发 +
    printHelp）迁出 `cliargs_help.cpp` 数据性 TU——文案被字节级单测与
    docs/reference.md 黄金比对锚定，独立后不再与解析逻辑互相搅动。

### 2026-10-01 全仓审计收口：P0 构建断裂 + 三项 P2（安全/健壮性）

外部审计（C++ 宿主 / Rust agent / Lua 驱动 / 测试与文档四路）发现 1 个 P0、6 个 P2，
本轮收口 P0 + 三项本周 P2 + 一项被 lint 门拦下的 P3；其余（TCP 回环鉴权策略、流式
总时长预算、virt 逐次判定、孪生驱动收敛）记入待办。每项修复均带回归与反向验证。

- **P0：dev 干净检出无法配置构建**。方案 A 常量下沉（955a2c3）把 `HAL.lua` 的
  `HAL.X = <字面量>` 改成 `HAL.X = _c.X` 重导出（真值移入 `hal_backend_glibc.lua`
  的 consts 表），但 `test/gen_const_check.cmake` 仍按旧模式从 HAL.lua 抽取 → 配置期
  FATAL_ERROR。旧 build 目录缓存不触发重新配置，把断裂掩盖到推送之后（origin/dev
  挂着必红提交）。修复：抽取源改指 `hal_backend_glibc.lua`，`read_const` 剥离表项
  尾逗号；教训（抽取模式与被抽文件书写风格是同一份契约）已写进脚本头注。
  验证：`rm -rf build && cmake -S . -B build` 全新配置+构建全绿。
- **P2：MCP 断连安全动作加认证门**（src/mcpbackend.cpp）。此前 `--on-disconnect`
  动作脚本在**任何**连接结束后无条件执行——同 UID 对端反复 connect+close（或反复
  发错密钥）可反复驱动执行器到"安全姿态"，连败 5 次还会借停机断电链把后端打停；
  首帧认证门（MCP-1/AuthFailGate）只盖住工具派发路径，没盖住断连路径。修复：
  `serveConnection` 返回值从 `ConnAuth` 扩为 `ConnOutcome{auth, admitted}`，断连动作
  只对 admitted（进入过业务循环：无 token 模式恒真、token 模式认证通过才真）连接
  执行，跳过时日志点名。回归：mcp_e2e.sh M6 正面（认证通过 → 动作执行）+ 反面
  （auth-bad/auth-none → 审计无 disconnect_action + 日志点名跳过）。
- **P2：守护线程补第五个异常边界**（src/daemonvar.cpp）。detached 线程的 `body()`
  内多处裸 Lua C API 调用（injectDaemonGlobals 的 push、tick 循环的 getglobal）不经
  pcall——chunk 给 `_G` 装报错 `__index` 元表即可让异常穿出线程函数：
  反向实测旧代码 `PANIC: unprotected error in call to Lua API`，进程当场消亡，停机
  断电链整条被跳过（执行器带电滞留）。修复：`body()` 全体纳入 try/catch（含
  `catch(...)`——Lua 错误从 C++ 帧抛出不是 std::exception），异常折成 -1 + 响亮日志，
  promise 照常 set；`util_except.hpp` 边界清单四处 → 五处。回归：
  `daemon_body_exception_folds_to_error_not_terminate`（旧代码下该用例直接打死
  测试进程）。
- **P2：agent 帧写侧 deadline**（agent/src/frame.rs）。2026-09-30 的读侧修复漏了
  写侧：后端活着但停止**读** socket 时，超过内核 socket 缓冲（~200 KiB）的
  `write_all` 无限挂起——帧上限 8 MiB，模型可控的大参数帧（长 SQL）可必现，
  `--llm-task-timeout`/`HARD_READ_CEILING`/Ctrl-C 全都够不到这段 await。修复：
  新增 `write_with_deadline`（与读侧对称），`call()` 的整帧写+flush 纳入与读侧同一
  预算。回归：`call_write_times_out_when_backend_stops_reading`（4 MiB 请求 + 假后端
  只收帧头就睡死 → 150ms 内报"写请求帧超时"）。
- **P3：require_driver 的 mpath 兜底只取首段**（modules/lib/drv_common.lua）。驱动
  框架迁移（6966c9b/822b0eb）在 `__mpath` 注入缺位的兜底分支用了单段 match——
  多路径布局（BOT4CJ_MODULE_PATH 多段且首段没有 mpath.lua）下驱动加载必失败。
  这是 dev 上 `lint-module-path-bootstrap` 红门的成因（迁移期按过滤器分批跑测试，
  lint 在过滤器模式下跳过，全量未跑）。修复：改为与 virt_prepare（fix-plan 3-3）/
  HAL.lua 同规的逐段探测 + 仓库缺省布局兜底。

### 2026-10-01 驱动框架迁移：require_driver 高阶函数，39 驱动全量迁移

- **`drv_common.lua` 新增 `require_driver(bus, spec)`**：声明式驱动框架，收敛 39 份驱动文件
  里重复的四段样板（标准前导 5→1 行、arg_table 守卫、总线校验、空 destroy）。驱动只写
  业务 delta（build/call 闭包接收 `ctx.fd`/`ctx.addr`），框架自动生成全局 build/call/destroy。
- **三族总线统一入口**：`require_driver("i2c"|"spi"|"uart", { name=, build=, call=, destroy= })`，
  校验器按总线类型分派（i2c_pair / fd≥0 / uart fd≥0），错误文案与旧内联版本逐字对齐。
- **39 驱动全量迁移**（5 批），净减约 1000 行样板。6951 个 Lua 测试全部通过，零回归。
- **框架增强**：① UART 校验器补充 `@UART variable` 提示；② 新增 `i2c_addr_default` 字段
  支持固定地址器件（如 AMG8833 0x69），address 列可省。
- 新驱动写法见 [drivers.md](drivers.md) 的驱动契约节与 [development.md](development.md)
  的「10 分钟写一个新硬件驱动」节。

### 2026-09-30 P1 收口：沙箱 print 拦截 + UART 共享路径硬拒绝

- **Lua 沙箱 print → stderr**：`sandbox_lua()` 新增 `stderrPrint` C 闭包覆盖全局 `print`，
  驱动层全部输出统一走 stderr（与宿主日志/诊断流同通道）。此前 48+ 驱动的 print 混在 stdout
  污染业务输出，故障定位时无法按流过滤。单点覆盖无需逐驱动修改，语义对齐原生 print
  （tab 分隔、末尾换行、tostring 转换）。
- **UART 共享路径告警 → 硬拒绝**：`HwFactory::rejectSharedUartPaths`（原 `warnSharedUartPaths`）
  从打印告警升级为抛 `IllegalArgumentException`。此前告警容易被日志淹没、配置漂移无人感知；
  现强制使用者显式共享一个 UART 总线变量（`'$uart-x'` 引用）或走守护采样。守门用例
  `uart_shared_path_rejected`（原 `uart_shared_path_warning`）同步更新为断言异常抛出。
  `docs/faq.md` 相关条目同步更新。

### 2026-09-30 库层诊断语言统一 + 文档过期修正

- **库层诊断统一为英文**：`HAL.lua`（UART/PWM 提示 10 处）、`nn.lua`（error 9 处）、
  `sensor_daemon.lua` / `sensor_daemon_multi.lua`（虚拟硬件横幅各 2 处）、`drv_common.lua`
  （UART 缺路径提示 1 处）共 26 处中文诊断字符串转为英文，与驱动层 `err()` 英文策略对齐。
  `utf8.lua` 的 `MARKER` 与 C++ `kInvalidUtf8Marker`、Rust `INVALID_UTF8_MARKER` 三侧同步
  更新为英文（`[! invalid UTF-8 bytes omitted` / `bytes]`），对应测试断言同步修改。
- **`docs/architecture_analysis.html` 过期修正**：共享槽 `values[32]` → `values[64]`（5 处，
  对齐 `SLOT_MAXVALS=64` TD-3 变更）；软门禁描述更新为全部硬门禁（3 处，对齐 2026-09-25
  CI 全硬化）；CURL 常量自检移除（随 Lua 协议驱动轨道删除）；C++ 单测数量 600+ → 634。
- **`docs/development.md`**：库层诊断语言条目从"另议"更新为"已统一"。

### 2026-09-30 全仓审计九项收口（C++ 宿主 9 / Rust agent 8.5 / Lua 与测试 8.5 / 文档 8.5）

- **新人卡点两处（审计 P1，实测复现后修复）**：① `mybot.llm.json.example` 含 `//` 注释
  ——自家严格解析器（C++ `jsonparse` 与 Rust `serde_json` 均不容注释）拒载，README 推荐的
  「配置文件方式」首启即「不是合法 JSON: line 44」。注释移除（字段说明 docs/agent.md 字段表
  本就有），新增 C++ 单测 `llmconfig_example_template_loads_strictly` 钉住模板永不回漂。
  ② 根 README 快速开始只导出 KEY/ENDPOINT——`mybot-agent` 的 model 是 fail-closed 必填，
  首启报「缺少模型名」。快速开始补 `BOT4CJ_LLM_MODEL`，实测两条路径都能到达 REPL 提示符。
- **MCP 首帧认证连续失败门（审计 P1）**：同 UID 对端此前可无限次重连试探 token（每次成本
  仅一个连接周期），与 TCP 口令的 `authMaxFails=5` 不对称。新增 `AuthFailGate`（mcpbackend.hpp，
  纯结构可单测）：连续 5 次认证失败停止服务（fail-closed 走正常停机断电链），哑对端
  「连上不发帧」不动计数、认证通过才归零——试探者无法用「连了就断」刷新失败窗口。
  回归：`mcpbackend_auth_fail_gate_counts_consecutive_rejections`。
- **agent 后端链路两处健壮性（审计 P1）**：① 帧读超时——`BackendClient::call` 的
  `read_exact` 此前无 deadline，后端活着但不回帧（工具层卡死）时任务无限挂起，模型侧
  超时与 Ctrl-C 都够不到这条 await。读超时 = `--llm-task-timeout`（轮墙钟预算贯穿到
  工具调用内部；0=不限时用 3700s 兜底 = 后端工具执行上限 3600s + 余量，见
  `frame::HARD_READ_CEILING`）。② 连接判死后**下一次调用**透明重连（`bridge::Conn`）——
  此前后端中途重启 = agent 必须跟着重启。在途调用绝不重试：回执丢失的 `tool.call`
  可能已在后端执行，重发即重复物理动作（lock-step 物理安全语义不变）。`frame.rs` 头注释
  里「断连即 resync」的言过其实一并订正。回归：`frame` 两条 + `bridge::reconnect_tests`
  两条（真 AF_UNIX 监听双代后端场景）。
- **会话文件 0600 创建（审计 P1）**：`SessionFileWriter` 此前按 umask 落盘（常见 0644）
  且含完整对话内容，而同仓 REPL 历史 / ICA sidecar 都是 0600、`perms_warning` 又只对
  已存在文件生效——首次会话全程无告警。新建/截断重建一律 0600（`mode()` 只影响新建，
  append 既有文件不动）。回归：`writer_creates_session_file_with_0600`。
- **`Config` 密钥面类型收口（审计 P1）**：`api_key` 字段换 `Secret` newtype——Debug/Display
  均渲染 `<redacted>`，唯一取值口 `expose()`（仅 provider.rs 装配请求头三处）。此前派生
  Debug 含明文密钥，距一条 `errln!("{cfg:?}")` 把密钥写进 stderr/审计只差手滑。
  回归：`config_debug_and_display_never_leak_the_api_key`。
- **守护 chunk 热插 UPDATE 路径钉住（审计 P1 复核）**：复核确认机制已闭环——启动期脚本
  可信故允许绝对路径（robot.cpp 注释明示），热插 INSERT 的绝对路径/穿越拒绝早已落地，
  09-29 批次又补了守护 chunk 加载前的信任前提体检。审计指出的缺口只剩 UPDATE 改写行
  无测试：新增 `hot_sql_plan_update_daemon_chunk_hits_the_new_row_gate` 钉住「改写行计入
  insertedRows → 落进新增行校验区间」这座桥（桥若断，UPDATE 侧即绕过 INSERT 侧防线）。
- **文档同步（审计 P1/P2）**：守护共享槽 336B→600B（TD-3+BQ-1 后未同步）在 daemon.md、
  docs/README.md、architecture_dataflow.svg、architecture_analysis.html 共 7 处订正，
  daemon.md 的槽布局明细（values[64]/stop@584/quiesce@592）一并补齐；README「85 个 Lua
  测试」→86、agent.md「15 个 .rs」→16；reference.md 环境变量总表补 `BOT4CJ_TRACE_FDS`
  与 `BOT4CJ_ICA`（「全部可调接口」闭环）；ica.md 登记进 docs/README.md 与根 README 的
  文档索引；sqlparse.cpp 头注释「'' 双写转义仍不实现」与 #33 之后的实现矛盾——订正为
  实际语义（安全判据的注释漂移比代码漂移更危险）；run_coverage.sh 的「gate:coverage 是
  软门禁」过期注释订正（2026-09-25 起已硬）。

### 2026-09-29 三项安全收口：MCP 首帧共享密钥 / 停机灭火 / 信任前提体检

- **MCP 首帧共享密钥 `BOT4CJ_MCP_TOKEN`（审计 P2-1）**：设了它，`SO_PEERCRED` 之上每条连接
  还须先过 `{"_route":"auth","token":...}` 认证帧（常数时间比对，失败断连 + 审计
  `auth_failed`）。此前 `mcpbackend.hpp` 自陈的同 UID 直连绕过 agent 预算的残留风险从此
  有了可选项：密钥名含 token，被 `run_command` 的环境清洗自动剔除，模型驱动的 shell 拿不到。
  设置但为空/超 256 字节拒绝启动（不静默退回无认证）。C++ 与 Rust 两侧读同一环境变量，
  旧版不配对会在报错里点名（unknown route ↔ auth required）。
  回归：`test_mcpbackend` 三态单测、`mcp_e2e.sh` M6、`agent_e2e.sh` R17（两进程正反面）。
- **停机灭火协议（审计 P2-3）**：守护共享槽新增 `quiesce` 位（offset 592，槽 592→600B），
  `DaemonRunner` 停机序列重排为「等待（不弃管）→ 宽限 → 仍未退场才**先灭火再弃管**」。
  灭火后该守护经 HAL 的一切真实总线传输（i2c/spi/uart）在进内核前被拒——「弃管守护触
  总线」的竞态窗口从无界收敛到在途单次传输。与 `stop` 分离是刻意的：宽限内退场的守护
  还要跑 `daemon_cleanup` 去初始化，传输必须放行。回归：`test_hal_quiesce.lua`、
  `test_daemon.cpp` finishTeardown 两态、既有 `daemon_grace_ms` 文案锚点不变。
- **信任前提体检（审计 P2-2）**：驱动/守护 chunk/白名单 dofile 三个加载点在加载前体检
  文件权限，**组或其他人可写即响亮告警**（`auditTrustedFilePath`，每路径一次）——ffi 全
  能力文件的「可信域」前提被削弱时操作者必须看得见。`os.getenv`/`io.read` 有意保留不动：
  ffi 面前 Lua 级白名单是化装。另：`__mpath` 注入失败（找不到/加载坏）从静默 fail-open
  改为响亮告警（多段路径时），单段场景保持安静（测试夹具常态）。

### 2026-09-25 删除 Lua 协议驱动轨道（#8）

- **对使用者意味着什么**：`modules/LlmOpenAI.lua` / `LlmDeepSeek.lua` / `LlmAnthropic.lua`
  及配套 `lib/llm_http.lua`、`lib/llm_wire_*.lua` 整条 Lua 模型调用轨道删除（11 个文件：
  3 驱动 + 4 库 + 2 离线用例 + 1 常量自检 + 1 泄漏抑制文件）。模型调用唯一路径是 Rust
  `mybot-agent`（rig 直连，reqwest + rustls）；`LlmOpenAI`/`LlmDeepSeek`/`LlmAnthropic`
  三个名字现在只是 `--llm-driver` 的**协议族名**（`agent/src/provider.rs` 内建），不再是
  可装载的驱动表名。仓库不再有 libcurl 依赖：apt 行、CI 安装包、`test_curl_consts` 目标
  与 `asan.supp`（libcurl → libcrypto 的 LSan 残留抑制）一并移除。`test/run_all.sh` 的
  agent-all Lua 段此后**有意跑 0 个文件**（过滤器与 SAN-2 花名册机制保留，将来若再出现
  `test_llm_*.lua` 会被自动纳入自证）。
- **动机**：C++ agent 轨道清退后，Lua 协议驱动只剩「供 `.mt` 脚本/`@Daemon` 直接调用」
  一个下游；四份协议实现（三族 wire 整形 + libcurl ffi）的回归只能离线守整形规则，
  而模型侧行为已由 Rust 侧 R12/R13 全量覆盖——重叠维护不再值当。
- **保留**：`mybot.llm.json` 双端解析与 R5m 互钉（模型侧字段归 Rust，共享面 C++ 仍校验）、
  `utf8.lua`（与 C++ 侧同表向量互钉的通用库）、`agent_e2e.sh` 的历史场景索引与审计里的
  驱动名字段（R12/R13 仍按三族名验证现役 Rust 语义）。

### 2026-09-25 capture_image 可请求采集尺寸（width/height）

- **对使用者意味着什么**：智能体拍照片可以按需请求分辨率——需要看清细节时加大，
  图片逼近 2 MiB 历史预算时改小（此前「图像过大」的报错只能提示「降低分辨率」，
  却没有降低分辨率的工具入口）。`width`/`height` 为可选整数（16..8192，须同时给出），
  缺省仍是 640x480；换尺寸会重开设备重协商 `VIDIOC_S_FMT`，比同尺寸连拍贵。
- **如实语义**：设备常把请求就近调到支持档位（请求 300x200 得 320x240），回执报
  **实际**尺寸并说明「被设备就近调档」；仿真来源（`--camera-src`）不缩放图片，
  请求尺寸在回执里明说「无效」。驱动按**请求值**（而非协商值）判是否重开，避免
  同请求每次调用都因调档差异而白重开。
- **落点**：`modules/Camera.lua`（尺寸解析/校验 + 重开判据）、`src/agentimage.cpp`
  （schema 两个可选参数、界值单点、回执文案）、`test_camera.lua` 与
  `test_agenttools.cpp`（15 + 4 条新用例，S_FMT 桩验证「变尺寸重开/同尺寸不重开/
  调档值比对」）。参数错误（只给一个、越界、非整数）在触碰硬件之前按 paramError
  口径拒绝。

### 2026-09-25 CI 六条软门禁全部转硬（TSan / clang / clang-tidy / coverage / musl / agent）

- **对使用者意味着什么**：推送到任何分支，十条 job 全部必须绿；此前 `allow_failure`
  的六条不再能「红着合并」。转硬依据如实写在 `.gitlab-ci.yml` 文件头：原条件「CI 上
  连续 2 轮全绿」从仓库内部不可观测，改以同版本工具链的本地预跑为判据（LLVM 14.0.6、
  alpine 3.20、rustc 1.98.1、TSan 5 轮）。
- **预跑揪出并修掉的真问题**：
  - musl：`hal_backend_glibc.lua` 的 `version()` 契约是「非 glibc 返 nil」，实际在取
    `gnu_get_libc_version` 符号那步抛 `Symbol not found`（pcall 只包了 cdef），拖垮
    `test_uart` / `test_uart_baud` / `test_hal_backend_swap`；两个 UART 用例的独立推导
    同样直取符号，改为 pcall 并按非 glibc 期望旧编号表。`docs/faq.md`「非 glibc 未验证」
    改为「musl（alpine 3.20 x86_64）已实证」。
  - clang-tidy：新功能引入 3 条（`main.cpp` 的 `getenv`/`strerror` 未走 `util.hpp`
    单点、`agenttools.cpp` 一处 string 链式拼接），改走 `getEnvVar`/`errnoText`/`concatMsg`。
  - Rust：2 条 clippy `manual_async_fn`（`model.rs` 的 `RawStream` 两个 impl 改 `async fn`）
    + 5 个文件的 rustfmt 差异。
- **`gate:musl` 扩容**：从「三常量自检 + 能编则跑 mybot_tests」扩为「常量自检 →
  mybot_tests → 整段 `test/run_all.sh`」（alpine 加装 bash/coreutils/python3；Rust 段
  因镜像无产物按 SKIP 计）。`gate:clang-tidy` 的计数行同时数 `error:`（此前
  `--warnings-as-errors` 下永远报「告警总数 0」）。`gate:coverage` 的「硬」不设阈值，
  判采集链路完整（AG-11 结论不变）；本地基线 78.10%（5842/7480）。

### 2026-09-25 Rust agent 会话历史压缩：被裁任务块压成摘要挂进系统提示词

- **长会话不再「忘光」**：`max_history_bytes` 裁掉最早的任务块时，被裁块交给同一个模型
  压成一段摘要（与已有摘要合并），挂在系统提示词末尾的「更早对话的摘要」一节。摘要不进
  `messages`，块判据、失败回滚、图片后注序号一律不受影响；预算先扣掉摘要一节再裁。
- **切口只落在任务块起点**：一对 `tool_call`/`tool_result`、一问一答永远不会只剩一半
  ——被裁部分整块送摘要、留下部分整块送模型（单测钉死不变量）。恢复期把图片占位文本
  形态的后注也排除在块起点之外，否则「任务 + 最终回答」会被切开。
- **送去摘要的是数据不是指令**：转录只含文本（图片/思维链只留字节数），每条 4 KiB、
  总量 128 KiB 封顶，逐行 `> ` 定界并声明；摘要挂回提示词时再定界一次。
- **配置**：`history_summary`（缺省 `true`）、`max_summary_bytes`（256-1 MiB，缺省 8192，
  超出在字符边界截断并告警）；C++ 后端只登记不解析。`--check-config` 新增「历史摘要」行。
- **随会话文件恢复**：新记录 `session_summary`（`content` + `keep_blocks`），恢复时收口成
  最后 K 个块并带回摘要，就绪行报「含历史摘要 M 字节」；`/clear` 连摘要一起忘掉；摘要
  失败什么都不写（自愈）。
- **审计**：摘要请求单独成行 `kind=model, status=summary|summary_error`，不带 `turn`。
- 守门：`history.rs` / `summary.rs` / `session.rs` / `config.rs` 单测；`scripts/agent_e2e.sh`
  R11c（三任务 + `--resume` 接力，夹具 `summarize` 模式对摘要请求形态设 400 判据），
  R11b 显式 `history_summary=false` 作反向判据。

### 2026-09-25 wait_variable 工具：智能体能「等事件」了

- **新工具** `wait_variable(name, key, condition, value, timeout_sec, poll_ms)`：后端按
  `poll_ms`（缺省 200，50..5000）轮询 `--read-var` 清单内某个变量的返回表键，直到
  `gt/ge/lt/le/eq/ne/changed` 条件成立或 `timeout_sec`（缺省 30，上限 =
  `action_script_timeout_sec`）到期，一次调用给一个结论。此前模型要等「水位到 30」
  「守护计数推进」只能一轮一轮调 `read_variables`，每轮一次模型往返。
- **与 `read_variables` 同门控同副作用面**：同一 `--read-var` 解析集合、同样只走
  `playQuiet`；缺省不注册。**超时是结果不是失败**（`ok=true`，回执写明超时与最后读数，
  键不存在时列出样本现有键）；参数问题（非法条件、缺 `value`、越界、变量不在清单、键在
  而不是数值撞上排序条件）在触碰硬件前拦成 `paramError`。等待期间每 ≤100 ms 看一次停止
  标志，Ctrl-C 不会坐满超时。`summary` 给审计 `satisfied=… elapsed_ms=… samples=…`。
- 系统提示词在可读清单非空时同时引导 `wait_variable`；`--check-config` 摘要行改为
  `read/wait_variable`。

### 2026-09-25 Rust agent 交互 REPL 行编辑 / 历史 / 命令补全（rustyline）

- **终端上的 REPL 有了行编辑**：方向键、Ctrl-A/E/W/U、↑↓ 翻历史、Ctrl-R 搜索、`/命令`
  Tab 补全；提示符处 Ctrl-C 清当前行不退出（退出仍是 `/exit` / Ctrl-D），模型轮内
  Ctrl-C 的取消语义不变。此前是裸 `BufReader::lines()`，方向键只会打出转义序列。
- **历史跨会话保留**：`$XDG_STATE_HOME/mybot-agent/history`（退到
  `~/.local/state/…`），目录 0700 / 文件 0600，上限 1000 条；`BOT4CJ_AGENT_HISTORY=<路径|none>`
  改位置或关闭。已存在文件权限过宽会提醒，不擅自改。
- **管道面不变**：stdin 或 stdout 任一不是终端就走原来的裸逐行读——提示符写 stdout、
  一行一任务、EOF 退出，E2E 与脚本化调用的契约逐字节保留，也不碰历史文件。
- 新增直接依赖 `rustyline 18`（关缺省 feature，只开 `with-file-history`）。

### 2026-09-25 两进程启动即 `PR_SET_DUMPABLE=0`，同 UID 不能 ptrace / 读内存

- **`mybot` 与 `mybot-agent` 进入 `main` 第一步**（先于解析 argv）调
  `prctl(PR_SET_DUMPABLE, 0)`：同 UID 其他进程不能 `ptrace` 附着，`/proc/<pid>/environ`、
  `mem` 变为 root 属主不可读，崩溃不落 core——运行中进程内存里的模型密钥 / TCP 口令
  不再对同用户 shell 敞开。root / `CAP_SYS_PTRACE` 不受影响，`cmdline` 仍全机可读，落盘
  的密钥文件与此无关（docs/agent.md 安全说明第 3 条如实列出边界）。
- **调试逃生口** `BOT4CJ_ALLOW_PTRACE=1`：两进程都跳过这一步并各向 stderr 打一行「注意」。
  `prctl` 失败只告警不拒启。
- **sanitizer 构建编译期跳过**：`-DMYBOT_SANITIZE=...` 现在同时定义 `MYBOT_SANITIZE_BUILD`，
  `hardenAgainstInspection()` 据此直接返回——LeakSanitizer 的 StopTheWorld 靠 fork 出的
  tracer ptrace 回附父进程，不可 dump 会让 `gate:sanitizer` 结构性失效。

### 2026-09-25 后端断连安全动作 `--on-disconnect`

- **新选项** `mybot --mcp-backend --on-disconnect <脚本>`：连接结束（对端关闭 / agent 崩溃
  被杀 / 空闲或墙钟超时 / 协议违规——任何原因）后，后端自行执行 `--scripts-dir` 之下
  的动作脚本（如 `zero.mt`），把硬件带到操作者预设的安全姿态。缺省不执行，行为与此前
  一致（硬件停在最后一个姿态带电常驻）——docs/agent.md 已知限制「Rust 失败不会自动给
  后端断电」由此有了操作者可配的兜底。
- **上电前预检**：脚本越界 / 不存在 / 为空即退出 2，判据与 `run_action_script` 同源
  （`loadActionScriptFile` / `runActionScriptFile` 自工具里抽成公开函数，工具与断连
  动作共用一份）。`--check-config` 摘要新增「断连安全动作」行。
- **不经策略确认**：它是操作者配置、不是模型请求，`require_confirm` 不拦它。执行失败
  只记日志与审计（`kind=session, status=disconnect_action, summary=<脚本>`，失败带
  `error`；session 记录的 `summary` 为加法字段），后端继续 accept。SIGINT/SIGTERM 停机
  不触发（走 `deactivate()`）。命令行独占，非 `--mcp-backend` 下给出即拒绝。

### 2026-09-25 Rust agent 模型输出改流式（缺省），Ctrl-C 即时丢弃在途请求

- **流式输出成为缺省**：`mybot-agent` 请求带 `stream:true`，模型正文按 SSE delta
  逐段打到 stdout（每段即刻 flush，管道下游同步可见），工具轮之间换行分隔；
  `--llm-no-stream` 或配置文件新字段 `"stream": false`（布尔，两端识别，仅 Rust 消费）
  退回原先的整段阻塞输出。`/status` 与 `--check-config` 摘要新增「输出模式」行。
- **重试 / 超时 / 审计口径不因面而异**：首帧之前的非 2xx、连接失败、首帧超时按与阻塞面
  同一张表分类重试；`--llm-timeout` 在流式下封顶首帧与之后的帧间空闲；审计 model 行
  与 token 用量由流适配器在流收尾时落，两种面的最终回答逐字节一致。
- **如实偏差**（详见 docs/agent.md「流式与阻塞的如实偏差」）：流式面会打出工具轮里的
  中间文本；思维链 delta 不打印；首帧之后断流不重试、已打出的文本不撤回；流式 429 拿
  不到 `Retry-After`，退避只按本地指数。
- **取消收紧**：Ctrl-C 之后两个面都**立即丢弃在途模型请求**（此前阻塞面要等服务端
  返回或 `--llm-timeout` 到期），退避等待同样即时中止；此前记档的「在飞请求由
  `--llm-timeout` 封顶」偏差就此关闭。
- **假模型服务** `scripts/fake_llm_server.py` 按请求体 `stream` 字段输出 OpenAI /
  Anthropic 两种 SSE 形态，`FAKE_LLM_FRAME_DELAY_MS` 控制帧间隔；`agent_e2e.sh` 新增
  R15（读端计时证明逐 delta 到达、`--llm-no-stream` 整段到齐、两面 stdout 一致、配置
  与摘要互钉），R8b 改为断言在飞请求被丢弃且进程在信号后 5s 内退出。

### 2026-09-25 软门禁转硬本地预跑：clang / clang-tidy / musl 存量清障归零

- **背景**：CI 六条软门禁各带转硬条件（最迟 2026-10-31），其中 clang / clang-tidy /
  musl 三条此前如实标注「开发机无相应工具链，未经本地预跑」。本次在开发机**免 root**
  补齐预跑环境（LLVM 14.0.6 官方预编译包；musl 走 proot + alpine 3.20 minirootfs——
  本地 musl-gcc 包装系统 libgcc 的路走不通），三条门禁的本地预跑全部通过。
- **真 bug 修复 2 处**：`test_util.cpp` 内嵌 NUL 用例的断言消息把「字面量 + int」当
  拼接用（指针算术），改 `std::to_string`；`test/cpp/harness.hpp` 的 `out()` 无参调用
  改走 `fputs`（fmt 非 string literal，`-Wformat-security`；fmt 含 `%` 时原实现按
  格式符解析）。
- **musl 可移植**：`test_actionsource.cpp` 补 `<sys/time.h>`（timeval 在 glibc 间接
  可见、musl 不然）——alpine 下 `mybot_tests` 从编译不过到全绿。
- **clang-tidy 存量 85 条逐条处置归零**：文案拼接收敛到 `util.hpp` 新单点
  `concatMsg()`；13 条 `strerror` 收敛到线程安全单点 `errnoText()`（`strerror_r`，
  老 glibc 的静态缓冲是真竞态）；多余拷贝改 `const&`；`getenv`/`setenv`/`readdir`
  单线程窗口定点 NOLINT 注明理由；其余零散处置，`Padding` 两条按先例写入
  `.clang-tidy` 豁免。
- **死代码**：删 `sqlparse.cpp` 的 `kDialectStatements`（改逐语句文案时只删用法留了
  常量，注释声称的「单一事实源」已失真）。
- **CI 注释对齐**：三条门禁的「未经本地预跑」声明改为本地预跑结论；`allow_failure`
  一律不动——转硬条件里的「连续 2 轮 pipeline」证据仍须 CI 侧产生。

### 2026-09-25 批次 D 落地：Rust agent 会话持久化（`--session-file` / `--resume`），C++ 参考清退

- **新模块 `agent/src/session.rs`**：会话文件读写原语（编解码、三道恢复判据、权限
  告警）与 `--session-file <路径>` / `--resume` 的 CLI/配置接线，语义对齐 C++ 参考
  实现与 `docs/agent.md`「会话持久化」规格节。
- **行格式与 C++ 参考逐字节兼容**（JSON Lines：首行会话头、其后一消息一行），故 C++
  时代落下的会话文件仍可被 `--resume` 读回——格式是跨版本的外部契约，不随实现语言
  迁移漂移。
- **一处有意偏离 C++ 参考（已记档，非 bug）**：带图片的 user 消息落盘时保留原有的
  文本说明（caption）+ 追加占位行（C++ 参考把整条 `content` 换成占位文本）——恢复后
  模型仍能看到那句说明；图片字节本身仍然不落盘。
- **恢复三道判据**（任一不过即拒绝并退出 2）：① 逐行解析（损坏行跳过并点名行号，
  首行必须是合法会话头）；② 指纹核对（结构脚本名 / 工具集指纹 / 系统提示词指纹）；
  ③ 协议自洽（`tool_call_id` 无孤儿/无重复/无漏答）。
- **落盘点** = `repl.rs` 一轮任务成功 materialize 之后按「上次已落盘长度」追加新消息：
  失败任务里已完整跑完的轮次留在内存历史里、随下一次成功任务一起落盘；`/clear` 向
  文件写 `session_clear` 分界标记。审计联动：`--resume` 采纳被恢复会话的原 session id，
  并在 `start` 行标 `resumed: true`（加法字段，老消费方不受影响）。
- **C++ 侧清退**：`src/agenthistory.{hpp,cpp}`、`test/cpp/test_sessionfile.cpp`、
  CMakeLists 三处登记与失去唯一调用方的 `agentproto.hpp::ChatMessage` 一并删除
  （`ToolCall`/`ToolSpec`/`parseToolArguments` 仍在 MCP 帧协议活跃使用，保留）。
  **不动**：C++ 宿主侧（`--mcp-backend`）对这两个选项的既有拒绝与帮助文案——本机制
  完全在 `mybot-agent` 一侧。
- **E2E**：`agent_e2e.sh` 新增 R11 两进程接力（进程一落盘并完整跑完工具循环，重启
  假模型服务后进程二 `--resume`，验证「首个请求就带着完整历史」）+ 2 条免服务负路径
  （换 `--robot` 指纹不符 / `--resume` 缺 `--session-file`，都在连接端点前退出 2）。
- **批次字母口径**：批次 D 至此完整落地，功能面正式收敛为「C++ 只管硬件与工具执行，
  Rust 只管模型循环与会话记忆」。

### 2026-09-25 决策：agent over TCP（原批次 E）不做，设计存档下架

- **决策**：远程 REPL 形态（mybot-agent 监听 TCP、对端逐行下发任务）**不再规划**。
  远程场景回归既有路径：ssh 到机器人侧本地运行 `mybot-agent`（stdin 交互），动作
  脚本的远程投递走 `--tcp` 动作源（安全口径不变：回环缺省、跨机必须 `--tcp-token`）。
- **下架范围（纯文档/注释，零行为变更）**：`docs/agent.md` 的「远程对话」设计存档节
  收敛为决策存根；README 相关节删除；testing/守门「仍未覆盖」清单摘除。
  `test/run_all.sh` 的三条 `agent_tcp_*` 负路径钉子**保留**——它们钉的是
  「`--agent` 已移除」「`--mcp-backend` 与 `--tcp` 互斥」的拒绝行为，与远程 REPL 无关。
- 批次 D 不受影响；批次字母 E 随本决策作废，不再复用。

### 2026-09-24 重构四连：C++ 侧死代码清退 + 活代码收敛 + Rust 修复 + 文档对齐

- **C++ 死代码清退（分档）**：REPL 迁 Rust 后「仅测试可达」的孪生实现按「Rust 是否
  已有对等实现」分档删除（`agentbudget.*`、`agentdriver.*`、`agentproto` 的
  ChatTransport 一族、`buildAgentSystemPrompt`、`runSqlCommand`、actionsource 的
  TcpAgentSession 等），净 **-2331 行**。session-file 读写原语与 `test_sessionfile.cpp`
  暂留作 Rust 批次 D 参考（后随批次 D 清退）。
- **`--agent` 入口快速失败**：cliargs 校验层直接拒绝（退出 2 + 两进程替代用法指引），
  不再先做 LLM 驱动注入/哨兵装配/TCP accept 才打到桩。模型循环类选项
  （`--task`/`--llm-*`/`--prompt-file`/`--session-file`/`--resume`）一律拒绝并指路
  `mybot-agent`；主帮助补齐此前完全缺失的 `--mcp-backend` 段。
- **C++ 活代码收敛**：SQL 渲染错误通道类型化（`SqlOutcome{ok,text}`，废除「错误: 」
  字符串前缀当错误码的往返）；UTF-8 校验/FdGuard/ASCII 小写化/整词切词四组重复实现
  收敛到 util.hpp 单点；`buildAgentConfig` 由 242 行内联拆为具名步骤函数（判据/文案
  不变）。
- **Rust agent 修复**：交互 REPL 的「思考中…」动画重写——写出改 `libc::write` 直写
  fd 2（原裸 `eprint!` 对 EPIPE/ENOSPC 直接 panic）；SAN-9 请求体估算字节改按段求和，
  消除每轮一次 O(历史) 的序列化峰值；`PromptMaterial` 失败退出码 1→2 对齐
  「配置错误=2」。
- **R8b 场景修复（测试 bug）**：SIGINT 原打给管道末端的 `timeout` 包装进程（GNU
  timeout 不转发），信号被吞；改打 `pgrep -P` 找到的 mybot-agent 本体。
- **文档对齐**：docs/agent.md 顶部改为两进程拓扑横幅，README 全部 agent 示例改两进程
  形态；`Cargo.toml`/`hook.rs`/`agent_e2e.sh` 的 `.tmp/*` 失效引用改指 git 历史；
  8MiB 帧上限双侧常量注释互相点名（改一必须改二）。

### 2026-09-24 收尾：HEAD 恢复可编译 + agent 守门按归属分流

- **修一个不能编译的 HEAD**：上一次删除 `src/agentloop.{hpp,cpp}` 时留下对不完整类型
  取成员的 `summarizeSessionStatus` 与两个桩，`agentmode.cpp` 所在的两个目标全断，且
  已推到公开分支。按「生产零调用者」判据删除，活码（`summarizeAgentConfig` /
  `makeToolContext`）保留。
- **补回一处被连带删掉的判据**：`historyProtocolViolation`（「assistant.tool_calls
  与 tool.tool_call_id 一一对应」的可执行判据，SAN-1）唯一实现随删除消失，而 Rust 侧
  没有对等实现——已逐字归位 `agenthistory.{hpp,cpp}`。教训：删一个「表面」时，判据与
  注释里的依赖者不会跟着报错。
- **agent 守门按归属分流**（原 22 个 C++ 场景在桩上全线红）：属**后端工具面**的 6 条
  迁入 `scripts/mcp_e2e.sh`（纯标准库 Python 帧客户端直连 AF_UNIX 后端，不经模型、
  判据更硬：`apply_sql` 带电生效、热删除真摘变量、门控对照、策略拒绝零副作用）；属
  **循环层**的归 `agent_e2e.sh` 的 R 段（该文件成为 Rust 单一模式）。`--session-file`/
  `--resume` 两进程接力等如实记为仍未覆盖（批次 D 落）。
- 注释与数字归位：十余处指向已删 agentloop 的注释改为指名真实归属；README 的单测
  文件计数按实测更正。

### 2026-09-24 审计高危修复两连：掩蔽引号绕过（H1）/ Rust stdout 写失败 panic（H2）

- **H1（掩蔽绕过）**：`maskSecretsInText` 把引号一律当值终止符，值**以引号开头**时
  掩蔽区为空——`export KEY="sk-x"` 输出 `KEY=***"sk-x"`，引号后的密钥留在明文。泄漏
  双通道：审计 `args_raw` 落盘（append-only 不可撤回）与 `run_command` 回执进模型
  上下文前的净化是同一函数。修复：新增 `maskedValueEnd` 共用于三个形态——引号值连同
  定界引号**整体**遮蔽，未闭合遮到串尾（宁可多遮不可漏遮），引号段与裸段可拼接。
- **H2（stdout panic）**：Rust 运行时把 SIGPIPE 置 ignore，`println!`/`eprintln!` 对
  写失败（断管道 EPIPE、`>/dev/full` ENOSPC）直接 panic（退出码 101），`--task` 输出
  后的审计 session end 行随之丢失——C++ 的 printf 失败只设 errno 不崩溃，这是迁移
  引入的回归面。新增 `agent/src/out.rs` 把全仓 46 处 stdout/stderr 写点收敛为检查式
  宏：stdout 写失败经 `libc::write` 直写 stderr 告警**一次**，stderr 失败静默（stdout
  是模型输出与交互界面的保留通道）。

### 2026-09-24 Rust agent 循环层语义对齐 C++（rig 迁移·批次 C）

- **新增四个模块**（`agent/src/`）：`budget.rs`（任务内文本/图片预算）、`history.rs`
  （会话历史按整块裁剪，最新块无条件保留）、`hook.rs`（墙钟两判定点、单轮调用上限、
  失败回滚边界、取消标志、图片批次物化）、`model.rs`（图片后注 + 单次调用超时 +
  网络重试）。
- **循环语义对齐**：任务开始前裁剪旧历史并清零任务内预算 → 轮循环开头判「取消 / 墙钟
  耗尽」→ 逐条派发前再判墙钟 → 超过 `max_calls_per_round` 的调用**不派发**（模型看到
  「未派发」回执）→ 成功才把新消息并入历史，失败回滚到钩子记下的边界（半截
  tool_calls 序列不落盘）。
- **图片后注**：说明 + 图片追加到**含工具结果的那条 user 消息末尾**（OpenAI 协议不
  允许工具结果带图片），与 C++ 的独立 user 消息同序；注入只作用于**发出去的请求**，
  任务结束后按记录的位置物化回会话历史。
- **网络重试 / 取消 / 配置面**：`max_retries`（0 合法）+ 本地指数退避，退避取「本地 vs
  服务端 Retry-After」较大值；SIGINT/SIGTERM 首次只置停止标志，第二次强杀；
  `max_history_bytes` 等 6 个限额字段落地解析与区间校验（0 仍是「不限」/「不重试」的
  显式语义）。
- **审计**：model 行改为**逐次模型调用**一条，补 `req_bytes` / `resp_bytes` /
  `attempts`（>1 = 发生过重试）。
- **语义如实偏差（记录在案）**：① 单轮上限的呈现位置（C++ 剔除超限调用并把说明追加
  进 assistant 文本，rig 侧挂在工具回执上）；② `req_bytes` 口径（同为下界，非逐字节
  相同）；③ 重试分类（C++ 用 curl 退出码表，rig 用 HTTP 状态码 + 连接类错误）；④
  取消时效（rig 的在飞请求由 `--llm-timeout` 封顶）。

### 2026-09-24 agent 本体迁移到 Rust（rig）：`mybot-agent` 骨架落地（批次 B）

- **目录与产物**：`mcp-sidecar/` → `agent/`，包名/二进制 **`mybot-agent`**；MCP stdio
  协议层删除——agent 直连 `mybot --mcp-backend` 的 AF_UNIX 帧（4B BE）。
- **模块**：`main`（CLI/编排/退出码）、`config`（配置链：命令行 > 环境变量 > 配置
  文件 > 缺省，未知字段仍拒绝）、`provider`（rig 驱动按协议构造）、`bridge`（工具表
  →rig 工具、帧调用→工具回执）、`prompt`、`repl`、`audit`（session/model 行；**工具
  行仍归后端**，agent 不重复记）。
- `/hardware`、`/scripts`、`/sql` 一律经工具层（本进程不持有硬件）；后端没给工具时
  **明确点名缺的开关**（如 `--agent-hot-sql`），不假装执行过。图片注入、历史裁剪、
  会话持久化、取消等尚未搬（批次 C/D 落）。
- **E2E 与守门接线**：`agent_e2e.sh` 加 `--rust`（R1 工具循环 / R2 提示词包 / R3 审计
  分工 / R4 REPL + 能力门控 / R5 `--check-config` 正负路径）；`run_all.sh` 新增
  agent-rust-e2e 段；`mcp_e2e.sh` 删 MCP stdio 段，原判据改由直连帧的独立 Python 客户端
  接住（两份实现各自跑绿不证明字节兼容，第三方尺子才证明）；CI `gate:sidecar` →
  **`gate:agent`**。
- **夹具适配**：`fake_llm_server.py` 的 OpenAI 形态应答补 `id`/`model` 外壳（rig 0.42
  定为必填，真网关也都带；C++ 驱动只取 `choices` 故不受影响）。

### 2026-09-24 守门修复三连：curl POST 顺序 / 审计 exit_code / destroy 契约调和

- **`llm_http.lua` POST 选项顺序**：`CURLOPT_POSTFIELDSIZE` 必须写在
  `CURLOPT_COPYPOSTFIELDS` **之前**——curl 以「设 COPY 时的 postfieldsize」决定拷贝
  字节，后设的 size 会把它打回未定长态，实测 POST 退化为读流语义，**所有模型调用
  失败**（agent E2E 全线红）。
- **审计 `exit_code` 字段消失**：run_command 回执整体经 quoteUntrusted 定界后，
  `exit_code=` 行带前缀且不再位于文本开头，「必须是前缀」的解析假设失效——工具执行
  照常、审计行却静默丢失退出码。改为逐行扫描，数值约束不变。
- **PCA9685/UART destroy 契约调和**：arg_table 守卫放回 destroy（非表入参返 ERR，
  不抛错、不静默成功），UART 的 fd 释放**先行**（无泄漏要求保住）；表内资源缺失/
  写失败仍按既有语义静默 OK。

### 2026-09-24 帧后端新增只读路由 `context.get`（rig agent 迁移·批次 A）

- `--mcp-backend` 帧协议的第四条路由，**只读**——为 Rust agent 提供系统提示词素材：
  `robotName`/`structurePath`/`scriptsDir`/`allowShell`/`allowHotSql`/`hardwareCount`
  与三段**已定界**磁盘内容（结构脚本 16 KiB 摘录、硬件清单、脚本清单）+ 内联
  `readableVars`。
- **单一事实源**：素材读法与 `buildAgentSystemPrompt` 完全同款——消毒、截断、
  `quoteUntrusted` 定界都在 C++ 单点，Rust agent 只拼装不定界（「同一份磁盘内容，
  两条进上下文的通道必须有同一定界强度」）。`hardwareCount`/`hardwareList` 每次现取
  注册表，热插拔后不漂移。
- **口径**：与 `tool.list` 同判据——不走 `DispatchGate`、不占每连接预算；未绑定
  cfg/robot 的手搓后端响亮回错误。

### 2026-09-23 只读查询：`SHOW TABLES`

- 方言第七条语句，**只读**、不受 `--agent-hot-sql` 门控：在 `query_sql` 与 REPL
  `/sql` 里列出当前结构的全部表名与数据行数。agent 模式下宿主会自动注入
  `LlmOpenAI`/`Camera` 等表，它们不在结构脚本摘录里——`SHOW TABLES` 是「当前到底有
  哪些表」的唯一权威答案。
- 不带 `WHERE`/`LIKE`（要筛行用 `SELECT * FROM 表 WHERE ...`）；误写的尾随词元由
  词元级报错兜住。

### 2026-09-23 热更新：`UPDATE 表 SET 列 = 值 [, ...] [WHERE ...]`

- 方言第六条语句，在 REPL `/sql` 与 agent 的 `apply_sql`（`--agent-hot-sql` 门控）里
  按**断电重建**改写命中行——执行语义 = 「DELETE 命中行（物理断电摘除）+ INSERT
  改后行（重新装配上电）」，改后行取 `inherited()` 解析值物化；`name` 列可改（= 换名
  重建）；无 WHERE = 改全表。`isUnsupportedStatement` 随之整体移除（清单清空）。
- **先删后建执行序**：含 UPDATE 的批次把物理删除提前到插入之前（注册表重名即抛，
  同名重建必须先摘旧件）。失败窗口如实：新件装配失败 ⇒ 已摘旧件不复原，错误点名
  丢失的变量。
- **引用完整性预检扩面**为「提交后合并视图」；被引用（controller/trigger/$/表达式）
  的变量拒改，与热删除同一条防线。
- **批内独占**：UPDATE 与该表的其它行级语句双向不得同批，本批次新建的表上也不可用。
- 生效摘要（G2）报「已热更新 N 行（断电重建）」，删除/新增计数照报。

### 2026-09-23 整表撤下：`DROP TABLE ...`

- 在 REPL `/sql` 与 agent 的 `apply_sql`（`--agent-hot-sql` 门控）里把上电常驻机器人
  的**整张表**当场撤下：表内全部硬件变量逆序 `destroy()` 断电 → 撤注册 → 擦结构行 →
  把表壳连同元数据提供者、守护登记一起摘除。**空表（只有列头）可撤**。
- **不带 WHERE**（标准 DDL 语义），误写在**解析期**被词元级报错拒绝、活体一字未动。
  要按条件删部分行用 `DELETE`。
- **实现顺序是死的**：先走与 DELETE 完全相同的逐行擦除机器（断电/撤注册/引用完整性
  防线全部复用），再摘表壳——MetaProvider 按「矩阵地址 + 行号」定位，先摘表壳会让
  其后所有表的目标错位。
- **新增不变式：MetaProvider 重锚定**——矩阵容器中间 `erase` 会让其后元素引用失效，
  撤表后按表名（唯一且稳定）把存活提供者重锚定回新地址。
- 批次内 DROP 必须是该表的唯一动作；`CREATE` 后同批 `DROP` 同名表有专门路径；
  `Action` 瞬态表拒绝 DROP。DROP 之后可**同名重建**。`drop` 成为关键字（带引号仍是
  合法 NAME）。

### 2026-09-23 守门检查点同步：`parse_cases` 95 → 96

- `test/run_all_selftest.sh` 的条数检查点在此前新增解析用例时漏改，使 pre-push 守门
  自检恒红；本批同步到实际条数。纯守门簿记，无行为变化。

### 2026-09-23 第三个协议驱动 LlmAnthropic（批次 3）

- **`LlmAnthropic`**（`--llm-driver LlmAnthropic`）：Anthropic Messages 协议驱动。
  价值不在「多支持一家厂商」，而在**验证规范化协议抽象是否真的成立**：它与 OpenAI 系
  形状差异最大，本批落地以**宿主零改**（`git diff --stat src/` 为空）兑现「换模型 =
  换 lua 文件」对所有协议成立。
- 四处形状差异全部收敛在 `lib/llm_wire_anthropic.lua`：① `system` 抽到顶层字段；
  ② 工具调用表达为 `tool_use` 块、`input` 是对象（驱动解码，非法 JSON 响亮拒绝）；
  ③ 工具结果并进一条 user 消息的 `tool_result` 块（附严格交替自检）；④ 鉴权用
  `x-api-key` + `anthropic-version` 头。
- **两处 provider 约束 fail-closed**：`temperature` 只接受 [0, 1]（越界响亮拒绝，
  不静默钳制）；`max_tokens` 必填。
- 响应侧同样抹平：text 段拼接、`tool_use` 转规范化 `tool_calls`、非文本非工具块丢弃
  但**计数入审计**、`usage` 映射到审计认得的字段、`stop_reason` 进 `extras`。
- API key 读取链追加 `ANTHROPIC_API_KEY`。

### 2026-09-23 agent over TCP（`--agent --tcp`，批次 2）【已于 2026-09-25 决策下架】

- 远程 REPL：对端每行一条任务，模型输出与工具回显按既有 REPL 格式回写同一条连接；
  `--task --tcp`、`--agent --tcp --input` 仍被拒。
- 安全口径逐条继承动作源 TCP：缺省只监听回环、跨机必须 `--tcp-token`（或
  `BOT4CJ_TCP_TOKEN`）、口令失败上限相同；取消哨兵、任务墙钟、策略钩子、审计在远程
  形态照常生效。
- **超时口径**：远程 REPL **不套用连接总时长预算**（那是给一次性脚本投递设的），只
  保留空闲超时（缺省 600s，只在等新任务行时计时）。
- **远程取消**：客户端 Ctrl-C / 断开连接 = 与本地 SIGINT 完全相同（置停止标志 + 创建
  取消哨兵，在飞的模型调用立即中止，随后优雅收尾）。
- 接受连接在硬件上电之前（没有对端时不让执行器带电空转）；REPL 的 `log` 流仍走
  stderr 不并入连接（宿主自己的诊断不污染对端看到的回答）。

### 2026-09-23 会话持久化与恢复（`--session-file` / `--resume`，批次 1）【C++ 参考实现，语义已随批次 D 迁 Rust】

- **`--session-file`（写入）**：每个任务块完结后把新增消息逐行追加进 JSON Lines 会话
  文件（首行会话头：结构脚本名 / 工具集指纹 / 系统提示词指纹）。机器人在重启后不再
  「忘掉刚才聊了什么」——此前唯一出路是重启进程，而重启会丢掉硬件常驻状态，现场代价
  不小。文件含完整对话内容，权限宽于 `600` 时启动告警。
- **`--resume`（恢复）**：**路径必须来自命令行**——恢复是把磁盘内容读进模型上下文，
  配置文件不能授权一次读取（与 `camera_src_dir` / `read_vars` 同判据）。三道判据全过
  才恢复，任一不符即退出 2 并点名差异：逐行解析、指纹核对、工具调用协议自洽（恢复
  的历史按不可信输入对待）。
- **图片与 system 提示词不进文件**：图片字节以占位落盘（体积与隐私两个方向都要求
  字节不出现在磁盘上）；system 提示词恢复时由当前进程重建，再按指纹核对。
- **`/clear` 联动**：向会话文件写 `session_clear` 分界标记，恢复时丢弃此前全部消息
  ——否则「忘掉对话」会在下次恢复时复活。
- **审计 schema 加法演进**：`kind="session"` 的 `start` 记录新增 `"resumed": true` 并
  沿用原 session id——既有字段名、顺序、语义一个不动，老消费方逐字节不变。

### 2026-09-23 热删除：`DELETE FROM ... [WHERE ...]`（含 AND 多条件）

- 方言新增语句：在 REPL `/sql` 与 agent 的 `apply_sql`（`--agent-hot-sql` 门控）里把
  上电常驻的器件**带电摘掉**——逆序 `destroy()` 断电 → 摘元数据/守护登记 → 擦结构行
  → 撤注册。无条件写法删空该表。结构脚本 `.sql` 里写 DELETE 仍在解析期报错（机器人
  未上电，没有可变的结构）。
- **`AND` 成为关键字**；WHERE 条件列表解析收敛到 `Parser::nextConditionList` 单点，
  SELECT 与 DELETE 共用。`OR`/比较运算符/`IN` 仍不在方言内。
- **原子性口径：逻辑原子、物理不可逆**。`destroy()` 是物理断电，删了没有回滚可言
  （回滚 ≠ 复活器件），故防线放在**事前判全**：目标表、命中行、引用完整性、全结构
  判据全部在动第一个器件之前跑完；过关后物理 `destroy` 失败则立即中止并如实报
  「已删除 N 个（名单）、M 个未删除」，不假装回滚。
- **引用完整性**：被 `controller`/`trigger`（含编号列）、`$name`、`$(...)` 引用着的
  变量**拒绝删除**，报错逐条点名引用位置——这些引用在框架内部是终身缓存的裸指针，
  热删除第一次打破「pool_ 只增不减」的不变式，必须把「不留悬垂」重新建立为构造保证。
  「引用方与被引用者一起删」合法。
- 删除按行号倒序执行（销毁序 = 声明序倒序是总线 fd 的资源不变式）；条件命不中任何行
  时**报错而非静默成功**；删除后 `flowRefs_` 摘名（删掉再热插同名变量仍是合法恢复
  路径）。

### 2026-09-23 SELECT 支持基本 WHERE 等值条件（`=`）

- `SELECT * FROM 表 WHERE 列 = 值;`：单条件等值、精确文本匹配（比对 `inherited` 显示
  值，数值按文本比），`query_sql` 工具与 REPL `/sql` 同一生效。AND/OR、比较运算符、
  多条件不在方言内；0 行命中显式报「0 行匹配」，模型不会把「没有行」误读成渲染故障。
- 词法器 `=` 成为第五个分隔词元（紧贴写法 `col=value` 靠挂起标志切分）；匹配单点是
  `mt::matchRows`，为将来 DELETE/UPDATE 选行复用。

### 2026-09-23 文档勘误：工具表 / 场景数 / 不存在的 --agent-verbose / 单测基线

- 纯文档修正：agent 工具表补齐 `query_sql` / `apply_sql` / `read_variables` 三行并标
  出各自开关；`--agent-verbose` / `--verbose` 从来不是本项目的选项，引用改为如实描述；
  E2E 场景数统一为实数；单测基线 `659 / 0` 为误记（「通过」是用例数），已就地更正。

### 2026-09-23 回读通道（A）：--read-var 与 read_variables 工具

- **模型此前无法「主动读」**：新增 `read_variables` 工具（无参数，一次读全部可读
  变量）——此前除摄像头外，模型对硬件状态全盲，按读数闭环类任务做不了。
- **可读集合必须由人列名**：agent **无法靠种类区分传感器与执行器**（`Servo`/`Relay`
  与传感器同属 `kind=lua`，对它们调 `play()` 会真驱动硬件）。故 `--read-var <名>`
  （可重复）缺省**不提供**该工具；配置文件写 `read_vars` 一律拒绝加载并退出 2（放宽
  读权限的字段只能命令行给）；执行时只放行已注册且种类为 `lua`/`daemon` 的名字，其余
  跳过并在启动时点名。
- 回执 8 KiB 上限、截断标注、UTF-8 消毒、「数据不是指令」定界。边界如实声明：读
  传感器也可能有副作用，这是一次派发，照走任务预算与审计。

### 2026-09-23 回读通道（D）：动作脚本回执带上驱动读数

- `run_action_script` 的回执此前只有一句「已执行」，器件读到的值走 stderr 回显——
  **操作者看得见、模型看不见**。现在执行窗口内挂一条同线程回收通道，回显追加进该次
  工具回执，随 `tool` 消息进模型上下文；上限 8 KiB、超限截断并显式标注、截断后 UTF-8
  消毒、「数据不是指令」定界。窗口外一切照旧。
- 配套：系统提示词与工具描述都告诉模型「回执里带读数，据此核对硬件状态，不要凭空
  假设」。

### 2026-09-23 tool_call id 的会话内唯一性（SAN-12）

- 合成 id `call_<turn>_<i>` 的 `turn` 每个任务都从 0 重数 ⇒ 第二个任务与第一个撞车，
  历史里同一 id 被回答两次，服务端 400 且那段历史已越过回滚边界——此后每个任务都以
  同一个 400 失败，只有 `/clear` 或重启能恢复。provider 在同一次响应里返回重复 id
  也是同一个口子。
- 现在：写历史前按「会话内全局唯一」改写 id（漏 id 时合成基线不变）——**没有任何
  调用被丢弃或跳过**（改写的只是不透明的 id，模型意图分毫未动）。改写成因打一行
  stderr 告警，不受 verbose 闸门约束。

### 2026-09-23 配置文件来源的提示词路径加根约束（H2）

- `prompt_files` 此前是「读任意文件」的原语：提示词包内容随首个模型请求发往 endpoint，
  而 endpoint 同样由配置文件决定——一份落在 CWD 的 `./mybot.llm.json` 写
  `{"endpoint":"<攻击者>","prompt_files":["/proc/self/environ"]}` 就能把操作者环境块
  （含重新注入的 `BOT4CJ_LLM_KEY`）整块外发。与 `camera_src` 同族（「配置文件不该
  授予读权限」），同判据收口。
- 现在：**命令行** `--prompt-file` 不受限（操作者本人授意）；**配置文件来源**的每一项
  必须落在 CWD 或 `BOT4CJ_PROMPT_DIR` 显式列出的目录内（`realpath` 判定，`..` 与软链
  逃逸都拦），越界在**上电之前**报错退出 2。

### 2026-09-23 agent 第三轮定点收口（G1/G2/G3）

- **G1（高）**：`run_command` 回执在**进模型上下文之前**过一次 `maskSecretsInText`
  ——子进程自己打印到 stdout/stderr 的 `KEY=value`（如模型执行 `env`）此前原样回传，
  可经同一配置可控的 endpoint 外发；发生替换时追加「已净化」提示。仍是文本级防线，
  与既有两道同档。
- **G2**：`apply_sql` 工具行新增 `summary` 字段记结构化生效信息（新增几张表/几个变量/
  几行）——只做加法，既有 schema 逐字节不变。
- **G3**：一次性任务非标准异常退出（Lua 错误的实际形态）也补写 `status=error` 的会话
  结束记录——原先 `catch(...)` 直接 return，审计里连一条 session 记录都没有。

### 2026-09-22 第七轮审计修复批次·第二批（H1 + 三条 P1 + P2 批）

- **`capture_image` 的模拟图片白名单恒真（H1，高）**：`src_root` 由 `camera_src` 的
  所在目录推出——src 的所在目录当然包含 src，判据恒真，白名单形同不存在。可达链
  **不需要 `--agent-shell`**：CWD 里的 `./mybot.llm.json` 写 `"camera_src":"/etc/shadow"`
  就能把该文件读进模型上下文再外发，而注释此前明确声称「任意路径读被拒」。修复三条
  口径：① 新增 `--camera-src-dir` 显式根（**命令行独占**，配置文件写一律拒绝）；②
  没有显式根时只有命令行的 `--camera-src` 才用其所在目录当根（保住快速上手）；③
  配置文件来源的 `camera_src` 一律不推导，都没有即默认拒绝。
- **`SINGLE_CONSUMER` 判据三条静默旁路（P1）**：缺列此前被当成「不参与判据」，而守护
  侧对 proto/path 都有缺省、多器件清单在 `devices` 列里宿主从不解析——五种声明者的
  「直连 + 守护」双消费可让器件地址相位互踩、**双方都报 OK**，漏标没有任何运行期
  症状。现在缺列按守护侧同一规则补全后登记、`devices` 列逐个 spec 解析、`driver` 列
  与 `devices` 列互斥；缺省值两侧读同一 Lua 源比对（这类漂移没有运行期症状）。
- **`@Include` 归并只数输入、不数产物（P1）**：产物侧此前零限，几十字节的脚本能把
  终态撑到数百 MB——树莓派 OOM 是 SIGKILL 语义，`deactivate()` 整体被跳过。现在
  `mtmerge::mergeTwo`（两两归并单点）在写任何一行之前判行数/单元格预算，取值给真实
  用量留两个数量级以上余量。
- **P2 批（六处）**：内嵌 NUL 击穿「全串消费」判据（`90\0junk` 报 OK/90，尾随垃圾
  静默丢弃——`parseLongStrict`/`parseFiniteDouble`/工具参数入口各加前置判据）；
  `DAEMON_CONFIG` 的矩阵占位符 `-` 不再原样下发（`sign='-'` 让符号扩展静默失效）；
  `loadfile` 的错误返回值不再被沙箱包装吞掉；守护多值脚本的槽容量改读注入单点
  （写死 32 的后果是两侧各自漂移、尾部字段永不出值且无报错）；build 期响应停止标志
  （多驱动上电期间第一次 Ctrl-C 此前毫无可见效果，而第二次是强杀、已 build 的器件
  原地带电）；两处注释/文案与实现不符修正。
- **守门自身：产物陈旧不再静默**：`test/run_all.sh` 开头比对编译期输入与产物 mtime，
  陈旧即退出 2——此前改完源码直接跑守门，跑的是上一轮二进制，汇总照样给出像样的
  「N 通过」。这类假绿比缺产物更危险。
- **本批未做（如实留痕）**：弃管守护的「写能力掐断」仍未落地——tick 卡在不可中断的
  总线事务里被弃管后，该线程醒来仍可能再写器件一次，进程退出瞬间器件可能重新带电。

### 2026-09-21 第七轮审计修复批次（AG-27 + A1–A8：协议形状、墙钟预算、假红门禁）

- **A1（高）**：超限工具调用的截断说明此前写成**额外一条** tool 消息并复用最后一条
  已派发调用的 `tool_call_id` ⇒ 历史 N 条 tool_calls 对 N+1 条 tool 消息、一个 id 出现
  两次，且落在不可回滚的历史里——下一轮起**每条任务都以同一请求体拿 400**。改为把
  说明**并入该轮 assistant 文本**，历史形状恒为 N⇔N。触发不需要攻击者：缺省
  `maxCallsPerRound` 16，一句「把这 20 个脚本依次跑一遍」就会命中。
- **A2**：补齐单轮上限的钉子用例（形状合规、说明到达模型且措辞不谎报、`0` = 不限），
  并新增与 Lua 侧同判据的 C++ 助手 `pairingViolation`（数量/顺序/id 重复/多一条）。
- **A1b（出站一致性自检）**：`llm_wire_openai.check_tool_pairing` 在**发包前**校验
  `tool_calls` ⇔ `tool` 一一对应，不通过即 `error` → 折成 ERR 契约，宿主给出点名
  「第几条消息、期望几条、实际几条」的报错，不把不配对的报文发出去换含混 400。
- **A3**：脚本清单把「消毒名」当「寻址名」——文件名含非法字节时首注释静默消失、清单
  交给模型的名字在磁盘上不存在，模型照抄必失败。现在 raw（寻址）与 shown（展示）分开
  保存并显式标注。
- **A4（单任务墙钟预算）**：体积与条数上限管的都是「往上下文里灌多少」，没有一条管
  时间——agent 模式全程带电常驻，最坏一个任务能让执行器带电十几小时。新增
  `--llm-task-timeout` / 配置 `max_task_seconds`（缺省 **900s**，0 = 不限）。判定点
  两处、语义不同：轮循环开头按「达到预算」如实失败；一轮**中间**耗尽时剩余调用不
  派发、但每条各留一条「未派发」回执（少一条就是服务端 400）。实际超限量最多是
  「一次模型调用 + 一条工具」。
- **A5（低）**：agent 会话路径只接 `std::exception`——本机 LuaJIT 用 C++ 异常实现
  `luaD_throw`，从没有 Lua CFRAME 的 C++ 帧抛出的错误**不是** `std::exception`，会把
  「单轮失败」升级成「整个 REPL 结束」且已执行的动作被回滚出历史。四处各补
  `catch (...)`。
- **A6（低）**：fd 自报探针加编译期开关 `MYBOT_TEST_TRACE_FDS`——生产二进制里不再有
  「按环境变量向任意路径追加写」的能力。
- **A7（低）**：provider 回传的 `id`/`name` 加长度上限（`proto_limits`），超限整条
  响应拒绝 fail-closed——id 不能截断（截断破坏与 `tool_calls` 的配对）。
- **AG-27（树莓派假红）**：守门长期 261/1 的那条失败根因不在产品代码——用例
  `ffi.load("curl")` 与 `llm_http` 的候选名在本机解析成**两个不同 libcurl 文件**，用
  B 实例读 A 实例的句柄等于按另一套结构体布局解引用。修法：用例一律经 `M.libname()`
  拿**同一份**实例，并补阳性对照。红门禁长期没人修的代价：它从此不再具备判别力。
- **A8 三处瑕疵**：死语句；两处「`--agent-shell` 或 `allow_shell=true`」文案与实现
  不符（配置文件已不能开 shell）；deepseek 驱动死参。

### 2026-09-22 响应形变矩阵（SAN-11，纯测试 + 契约成文）

- 新增 `test/test_llm_wire_contract.lua`：表驱动形变矩阵覆盖两个内置驱动的全部响应
  形态（choices/message/content/tool_calls 的缺失、非表、畸形、合法与畸形混合；HTTP
  层六种形态），每行标结局：**点名报错 / 降级但记账 / 照常处理**。宿主侧对应矩阵在
  `test_agentproto.cpp`。此前 PL-3/PL-6 都是线上撞到才补一个、没有矩阵——下一个形变
  还要等线上撞。
- 契约成文：`docs/agent.md` 补完整响应契约表与维护约定——**遇到新的 provider 形变先
  往矩阵里加一行**。一处归一化：`content` 为数字/布尔时归一成空串（原样透传会让宿主
  的判据读成「没有文本」，报错完全不指向真因）。

### 2026-09-22 REPL 新增 `/status`（SAN-10）

- 现场最常见的三个问题——「为什么它忘了之前说的？」「为什么拍照没进来？」「刚才失败
  了几次、花了多久？」——都要翻 stderr 全文才能回答；答案其实都在内存里，只是没有
  出口。`/status` 读已存在的状态渲染成一览：模型/接入点/超时/轮次上限、会话轮数与
  耗时、工具调用数与失败数、最近 token 用量、任务块数与历史字节（含是否裁过）、任务
  内预算用量、审计落点与 session id、硬件变量数、脚本目录。
- **三条如实口径**（不显示假数字）：预算 0 显示「不限（已用 …）」、历史未裁剪显示
  「未触发裁剪」、审计未开显示「(未开启)」。`/status` **绝不触发模型调用**。

### 2026-09-22 审计轨迹补上会话边界与模型轮次（SAN-9，加法演进）

- 审计此前**只记工具调用**：模型说了什么、花了多久、这一轮有没有真的发出请求完全
  不可查；一次在模型调用阶段失败的会话在日志里**一条记录都没有**——而那恰恰是最需要
  复盘的时候。新增 `kind` 字段（显式写在每行）：`session`（会话头/尾；**绝不写密钥**
  ，只写来源枚举名）、`model`（每轮一条，失败也记）、`tool`（既有形态逐字不变）。
- **接入点脱敏**（顺带堵一个真实泄漏面）：`endpoint` 落盘前剥掉 query 与 userinfo
  ——部分网关把凭据放在 URL 里，而审计是 append-only 的落盘文件。
- token 只记 provider 给的数（usage 由驱动原样透传），不做推算或计费估算。

### 2026-09-22 参数错误不再说成「执行失败」，丢弃的调用如实报数（SAN-8）

- `AgentToolResult` 增 `paramError` 标志（判据是**异常类型** `IllegalArgumentException`
  ，不是文案匹配）：参数校验失败的回执改为「工具参数错误: …」且 `executed=false`——
  模型漏传参数时不再收到「动作脚本执行失败（状态可能停在半途）」的谎报，实际什么
  都没跑。
- 丢弃缺 `function.name` 的工具调用后**如实报数**（宿主丢弃数 + 驱动自报计数），不再
  谎称「模型既没有返回文本也没有工具调用」——模型确实返回了工具调用。

### 2026-09-22 中断语义与代码对齐（SAN-7）

- 三处旧文案写着「模型调用期间（最长 600s）**不可中断**，此时再按一次 Ctrl-C 即立即
  强杀」——与取消哨兵落地后的实际行为相反，且是**会导致物理后果的操作建议**：运维照
  它多按一次 Ctrl-C，就跳过 `deactivate()` 把执行器留在带电状态。
- 修法：`docs/reference.md` 的信号一节重写为**唯一权威描述**（首次 SIGINT 四步；第二
  次是**逃生口而非常规操作**），docs/agent.md 只留指向；处理器注释同步。
- 新增 E2E 判据：首次 SIGINT 之后断言退出码**恰好 1**（被信号打死 130、段错误 139、
  早退 0 都不是 1，证明走的是正常清理链），取消哨兵不残留。

### 2026-09-22 驱动不再合成 tool_call id，并补齐响应形态判据（SAN-6）

- **行为变更**：协议驱动不再自己编 `call_<i>`，id 缺失时透传空串由宿主填——驱动的
  合成 id 每轮从 1 重来，会遮蔽宿主按会话内唯一设计的合成方案，历史里同一 id 被回答
  两次（同一条消息内也会撞）。
- `choices`/`message` 非表 → **点名返回**（不再抛裸 Lua 内部报错逃出驱动「返 ERR 表、
  绝不抛」的契约）；`tool_calls` 的**元素**非表 → 跳过并计数
  （`extras.dropped_bad_tool_calls`），与 PL-3 同口径——丢弃但如实记账。一处修，
  两个驱动受益（DeepSeek 复用同一份 `parse_response`）。

### 2026-09-22 动作执行回显改走 stderr（SAN-5）

- `MtVars_Lua::callImpl` 的三处 `std::cout` 改 `std::cerr`——回显是诊断输出，`--task`
  的 stdout 此前混进分隔线与驱动结果表，**不能安全接管道/重定向**（`| jq`、写文件、
  被脚本消费全部受影响）。回显没有消失（终端上两股都是屏幕）。
- 如实声明的残余：Lua 驱动自己 `print()` 的进度行仍走 stdout——stdout 契约收窄为
  **「宿主不往里写东西」**而不是「里面只有模型回答」。

### 2026-09-22 预算降级不再把「没执行」说成「副作用已发生」（SAN-4，安全口径）

- `TaskBudget::admit` 此前对**所有**工具结果一视同仁，超限时无条件追加「工具本身已
  执行完毕（副作用已发生）」——而策略拒绝、未知工具、参数错误这三类**从未被派发**。
  对物理机器人，谎报状态是安全相关的错误：模型可能据此认为命令已经跑过而不再重试。
- 修法：`AgentToolResult` 增 `bool executed`（缺省 true），三条「没派发」的路径显式
  置 false 且**不做降级**（这类文本本身就是要给模型的事实）；工具**内部**失败仍是
  true——脚本跑到一半报错时机器人可能停在半途，那种警告必须保留（方向相反的谎报
  同样不可接受）。
- 记账不变：`executed=false` 的结果**仍计入额度**——否则「反复被拒」成了往历史里无界
  灌文本的免费通道。

### 2026-09-22 `agent-all` 真的覆盖 agent 全表面了（SAN-2，守门假绿修复）

- `test/run_all.sh` 的 `agent-all` 映射指向一个**从未存在**的文件模式，「agent 全
  表面」入口的 **Lua 段静默跑了 0 个文件**（Lua 循环对「命中 0 个文件」是合法状态）。
  真正的协议面 `test_llm_drivers.lua` 一条都没执行——改协议驱动三个文件跑 `agent-all`
  会全绿。
- 修法：过滤器改 `llm`，自证从「映射的形状」升级为「被映射到的文件」——roster 里每个
  文件必须存在且真的会被跑到，反向漏登记即红。

### 2026-09-22 重试真的认识真实错误了（SAN-3，含 429/5xx 与 Retry-After）

- **行为变更**：重试判据此前三条匹配人话子串，而生产路径产出的错误（curl 英文散文、
  `HTTP 503`）**一个都不匹配**——`max_retries` 事实上不生效，429/5xx/真实超时/连接被拒
  各只发 1 次请求；`Retry-After` 完全不可达（响应头在 libcurl 里被丢掉），本地退避与
  真实限流窗口毫无关系。
- 修法（结构化判据优先，字符串只作回落）：`llm_http` 挂头回调解析 `Retry-After`；
  非 200 分支给出 `http_status`/`retriable`/`retry_after_ms`——**429 与 5xx 可重试，
  其余 4xx 不可**（两者此前长得一模一样：要么限流不重试，要么把 401 也重试烧配额）；
  宿主 `classifyDriverError()` 纯函数判类；退避取「服务端 Retry-After vs 本地指数
  退避」较大值，钳到 [1s, 5min]（坏网关回天文数字不能把 agent 挂死一天）。

### 2026-09-22 单轮截断不再复用 `tool_call_id`（SAN-1，阻塞级缺陷修复）

- **行为变更**：一轮工具调用超上限时，多余的此前被折叠成一条 tool 消息挂在**最后一条
  已派发**调用的 id 上 ⇒ 历史**两条同 id** ⇒ 服务端 400 `answered more than once`。
  该轮派发已完成、回滚边界已越过——此后**每个新任务都重发同一段历史、拿同一个 400**，
  只有 `/clear` 或重启能恢复。
- 修复：截断说明写进该轮 `assistant.content`，历史**不再新增任何 tool 消息**（这是
  唯一既不违反协议、又不与 `tool_call_id` 一一对应绑定的位置）；截断事件**写日志**
  （此前这个安全相关的事实只到达模型，stderr 与审计一个字没有——这正是缺陷能潜伏的
  成因）。
- 新增可执行判据 `historyProtocolViolation()`：`tool_call_id` 无孤儿/无重复/无漏答，
  把注释里的约定变成可执行判据。

### 2026-09-22 动作脚本执行预算（PL-2）

- **行为变更**：`run_action_script` 是工具集里**唯一没有任何上界**的执行路径且默认
  开启；挂死只可能来自 Lua 驱动自己的 `call()`，一旦发生会话永久卡住。现在窗口内给
  每个 Lua 驱动状态挂**指令计数钩子**并**关掉 JIT**，超期（`action_script_timeout_sec`
  ：配置文件 0-3600、缺省 180、0 = 显式关闭）即以 Lua 错误中止本轮脚本；`stopRequested()`
  纳入判据，Ctrl-C 在窗口内也能中断失控驱动（此前只能强杀，而强杀跳过 `deactivate`、
  把执行器留在带电状态）。
- **「必须先关 JIT」是实测结论**：LuaJIT 的 count 钩子只在解释器派发路径触发，热点
  循环编译成 trace 后钩子永不触发。窗口内解释执行约 3.4× 慢，但绝对值与脚本自身耗时
  （硬件延时主导）相比可忽略；窗口只覆盖一次 `run()`，窗口外一切照旧。
- **边界（写进 docs/agent.md 已知限制）**：驱动阻塞在 `read()`/`ioctl()`/`usleep()`
  里时 VM 不派发指令，钩子没有执行机会——这一条无解。

### 2026-09-22 网关数组形态 content 归一化（PL-3）

- 新版 OpenAI 协议允许 `content` 为数组形态，部分兼容网关也这么返回；宿主只认字符串
  ⇒ 文本为空、整轮以「既没有返回文本也没有工具调用」告终，**报错完全不指向真因**。
- 现由驱动在 `parse_response` 归一化：text 段按序拼接，非 text 段丢弃但计数记进
  `extras`（「模型给了别的内容而宿主没看见」必须在审计里可见）。DeepSeek 复用同一份
  实现，一处修复覆盖两个驱动。

### 2026-09-22 `--check-config` 配置校验模式（PL-1）

- agent 专属：校验通过后打印配置摘要并以 **0** 退出，**不装载结构脚本、不上电、不
  发起任何网络请求**——配置文件写错不再要「真起一次 agent 会话」才报错（接了硬件还得
  反复上下电）。E2E 用「结构脚本可读但不是合法 SQL」对照钉住「它真的什么都不跑」。
- 摘要只渲染、不校验（判据单点仍在 `buildAgentConfig`，再造一套只会漂移）；endpoint
  未配置时明写「未配置，首次调用将失败」；API key 只报来源，值绝不出现；失败路径一律
  rc=2（漏 `--robot` 也算——否则打印帮助退出 0，CI 会读成「配置没问题」）。

### 2026-09-22 两处名不副实的测试钉子修正（PL-4）

- 无行为变更，改的是「用例看起来在守、其实没守」：① 「`--help` 优先于校验」的用例只
  调 `parseArgs` 而测不到 `main` 的顺序——判定抽成可链接的 `cliargs::preflightCli`；
  ② 2 MiB 图片上限的夹具尺寸由**被测常量**推导，常量改错时夹具跟着走——补
  `static_assert` 字面量钉子 + 字面量边界用例。

### 2026-09-22 文档/注释残留四处修正 + 工具 schema 深度加锚点（PL-5）

- 纯文档/注释：`LlmOpenAI.lua` 头注释自相矛盾（以事实为准）；`pthread.lua` 回退基准
  停留在旧槽布局（数字错了就失去比对意义）；非 agent 模式的拒绝文案漏列 camera 四项。
- 「工具 schema 最深处为 6 层」此前是人工统计、无锚点——给参数加一层嵌套不会有人
  发现。新增钉子用例锚定层数与余量。

### 2026-09-22 响应整形：tool_calls 形态非法时报点名原因（PL-6）

- 网关把 `tool_calls` 返回成字符串/数字/布尔时，此前抛的是裸 Lua 内部报错（
  `bad argument #1 to 'ipairs'`）——不点名字段、不给报文片段，与同一函数对缺失字段的
  诚实口径不一致。现非表即点名返回「响应字段 tool_calls 形态非法（期望数组，实际
  <type>）」，正常报文一字不变。DeepSeek 复用同一份实现。

### 2026-09-22 CLI 超时/上限参数化（F-1/F-2/F-3）

- 三个写死的运行期数值改为可选 CLI 参数（不传 = 现状缺省，零变更）：`--tcp-max-auth-attempts`
  （口令失败上限，原 5）、`--tcp-total-timeout`（连接总时长预算下限，原 600s）、
  `--daemon-grace-ms`（守护弃管宽限上限，原 30000ms；全局选项——任何声明了 `@Daemon`
  的结构脚本都可能触发，在 `robot.active()` 之前完成校验）。
- 三者都不属于 agent 配置面（daemon/TCP 是全局能力），故不进 `mybot.llm.json`；区间
  常量单点在 `util.hpp`，`--help`/校验共用避免判据漂移。

### 2026-09-22 agent 重构阶段 C6 + C7（提示词素材单次读取 / 小路径收口）

- **C6**：预检读到的结构脚本头与脚本清单两段成品被系统提示词复用——原先各读一次，
  两次读之间文件变动会让「预检口径」与「提示词口径」分叉。缓存只覆盖会话开场的系统
  提示词；`/scripts` 与工具的清单查询仍实时遍历（本该看到最新状态）。
- **C7**：`escapeLogMarkers` 挪到 util.hpp 文本处理单点；`call_capped` 死分支删除并
  补上可达路径的首个用例。行为保持型。

### 2026-09-22 agent 重构阶段 C5（重试日志可注入）

- 传输层的重试日志原先硬编码 `std::cerr`——离线用例抓不到它，宿主也没得选。
  `AgentDriverConfig` 新增 `std::ostream* log`（空 = stderr，即改动前行为）；stdout
  只放模型回答；不改审计 JSONL schema。

### 2026-09-22 agent 重构阶段 B4（五个运行期数值配置文件化）

- 原先写死在下游结构体里的五个数值可由**配置文件**覆盖（仅配置文件——不加 CLI 选项
  ：它们都是「一次定好、长期不变」的部署参数）：`max_calls_per_round`（1-1024，**拒绝
  0**——0 是「一轮不限量下发动作」的旧语义，对物理机器人没有正当用法）、`max_retries`
  （0-6）、`retry_backoff_ms`（0-10000）、`run_command_timeout_sec`（1-600）、
  `run_command_max_timeout_sec`（1-3600 且 ≥ 上一个）。
- `max_retries: 0` 的语义修对（该字段是「含首次的尝试次数」，0 = 只发一次不重试）；
  退避单次封顶 60s（放宽参数后 `backoff << attempt` 会长到小时级）；跨字段关系按生效
  值判（`run_command` 工具描述里那组「默认/最大」也改读生效值，原先配置一改就对模型
  说谎）。

### 2026-09-22 agent 重构阶段 A1 + A2（子进程骨架单点化 / 确认程序一致性）

- **真缺陷修复（F1）**：`run_command` 与 `--agent-policy` 确认程序各写了一份
  「fork + exec + 超时 + 进程组终止」且已漂移——确认路径超时后打的是**无超时**
  `waitpid`，SIGKILL 无效（D 态/权限）时 **agent 会话永久挂死**。现在
  `agent_subprocess` 新增 `runChild(ChildSpec)` 单点，差异全部经选项表达；确认路径
  随之获得有界回收（超时放弃并响亮告警）与 fd 吊销。
- **A2（显式决策，不是顺手统一）**：确认程序的**环境保持继承缺省**——它是操作者自己
  写的可信程序，合法的审批/通知脚本可能依赖自有 `…_TOKEN` 类变量，套用子进程密钥
  清洗会**静默砸掉**它们。
- **顺带修掉的真缺陷（EPIPE 待决信号）**：往确认程序 stdin 写 payload 遇 EPIPE 时
  内核产生的 SIGPIPE 在解除掩码那一刻按默认处置**杀掉整个 agent 进程**。修复：恢复
  掩码前用 `sigtimedwait` 零超时消费掉「自己制造的那条」。

### 2026-09-22 agent 重构阶段 B3（图片传输层降副本）

- 拍照场景峰值内存实测大幅下降（1.2 MB 图 49 → 28.5 MB）：图片字节以 `{"__bot_img": N}`
  游标穿过宿主 Json 与 Lua 边界（字节本体不再进请求 Json），wire 组包期才落值——
  同 payload 的全程副本从 ~7 份降到 3 份。通道未注册/取值报错一律 fail-closed 报错，
  不发半截报文。
- 顺带修掉两个隐性的量级放大点：`Camera.lua` 的 base64 不再把 ~2N 个单字符字符串塞进
  Lua 表（1.2 MB 图就是 ~160 万表项）；`json.lua` 编码对无需转义的串加零分配快路径。

### 2026-09-22 agent 重构阶段 C3（REPL 循环可离线单测）

- REPL 循环从 `runAgentMode` 拆出 `runAgentRepl`，I/O 全经参数注入，循环/命令/错误
  恢复可离线驱动（此前只能靠 E2E 兜底）；行为零变化。

### 2026-09-22 agent 重构阶段 C4（历史预算口径诚实化）

- 历史字节预算的记账**不含** system 提示词与工具 schema，单条估重不计 role 字符串、
  JSON 语法开销与非 ASCII 的 `\uXXXX` 转义膨胀（CJK 约 2×），是线上字数的**下界**
  ——原注释只披露了其中一类偏差。注释修正，实现零改动。

### 2026-09-22 agent 重构阶段 C1（REPL /clear）

- 上下文被一次错误结论污染后，此前唯一出路是重启进程——重启会释放硬件、执行器停在
  归零位，现场代价不小。新增 `/clear`：只裁会话历史（保留 system 提示词），机器人
  保持上电。

### 2026-09-22 agent 重构阶段 B2（LlmOpenAI 不再内置缺省接入点）【破坏性】

- 此前 CLI/配置/环境链都没配 endpoint 时会**静默回退**到内置缺省网关——「什么都没配
  就跑起来」的会话把含结构脚本、硬件清单、摄像头画面的上下文发往该网关。
- **新语义（fail-closed）**：endpoint 未配置时**第一次模型调用即失败**，ERR 点名三种
  配置途径（`--llm-endpoint` / `BOT4CJ_LLM_ENDPOINT` / 配置文件 `endpoint`），不发出
  任何报文。`LlmDeepSeek` 保留缺省（厂商官方 API 域），首次回退仍打 stderr 注记。

### 2026-09-22 agent 重构阶段 B1（上下文字节预算配置文件化）

- 三个上下文字节预算此前是编译期常量（docs 自认「只能改源码缺省」），现场调不了。
  配置文件新增 `max_history_bytes`（缺省 4 MiB）、`max_task_tool_result_bytes`（2 MiB）、
  `max_task_image_bytes`（4 MiB）；单位字节，`0` = 不限，上限 64 MiB；严格解析，
  未知字段照旧整档拒绝。

### 2026-09-22 agent 重构阶段 A2 + C2（系统提示词拆分 / 工具结果定界对齐）

- **A2**：系统提示词构建拆出 `agentprompt.{hpp,cpp}` 单点承载（结构脚本头读取、
  「数据不是指令」定界、总量上限）；行为逐字不变。
- **C2**：`list_action_scripts` 的工具结果与系统提示词三段共用同一个 `quoteUntrusted`
  ——此前同一份磁盘首行注释，在系统提示词里被包裹成「数据不是指令」、在工具结果里
  却是裸文本，两条通道定界强度不一致。

### 2026-09-22 agent 重构阶段 A1（AgentSession 拆分，行为逐字不变）

- 全仓注释最密、最容易改错的 `agentloop.cpp`（四套职责互相交织）拆出三个独立单元：
  `agenthistory`（历史所有权/字节估算/整块裁剪）、`agentbudget`（任务内预算）、
  `agentdispatch`（派发漏斗固定顺序单点「策略确认 → 执行 → 审计」）；对外 API 不变，
  存量用例零修改。

### 2026-09-21 文档正确性与可读性复核（README / docs / 架构页）

- 逐条复核入口文档、参考文档与三张架构页：模块计数改为可复算口径（「59 = 47 器件/
  执行器/系统接口 + 3 总线 + 9 框架与模型驱动」）；架构页的行数、lib 清单、守门快照
  按实测重写；「三份脚本设计与用法完全不变」补限定——移植过程修掉过几处**旧行为本身
  有问题**的地方。复核过确认无误的条目留痕以免重复核对。

### 2026-09-21 第三轮审计修复批次（AG-19/AG-20/AG-22：图像内存、连接复用、作用域守门）

- **`capture_image` 的整串拷贝与「先编码再拒绝」（AG-19，高）**：宿主里唯一搬运 MiB 级
  字符串的地方，此前对同一串 base64 拷 5-6 份，而体积判据又在**拿到 base64 之后**——
  「一张注定被拒的图」要先被编码、跨 Lua 边界才丢掉。现在判据调用点零拷贝、体积判据
  改两道且第一道在编码之前（10 MiB 图实测峰值 219 → 163 MiB）。单张图峰值仍是原图
  数十倍属传输层架构问题，已在 docs/agent.md 已知限制写明实测数字。
- **HTTP 连接每轮重建（AG-20，高）**：`llm_http.post` 每次请求都新建 curl 句柄——
  会话池化的理由（省掉每轮重新握手）在**任何生产路径上都没生效**：每轮重做
  DNS/TCP/TLS，历史里那张 base64 图每轮重新编码上传。现在整颗驱动共用一份**池化
  会话**；连接层失败自愈换新句柄、HTTP 非 200 不重建（那是应用层答复，连接是好的）；
  `Session:recreate` 必须排在 `curl_easy_reset` + `cb:free()` **之后**（顺序写错实测
  直接 SIGSEGV）。守门还逼出两个自身缺陷，均已修：池化后 `destroy` 不关池泄漏、失败
  请求自动重建句柄线性泄漏——净结果 **ASan 构建下 agent 路径零泄漏**。
- **`agent:` 作用域表漏登记（AG-22）**：守门改成**派生式比对**——注册表里所有用例名
  前缀要么在作用域表、要么带理由登记，漏登记一整族即红（旧守门结构上不可能发现
  漏登记）。
- **任务内预算的记账与终态（AG-21 + AG-4′）**：降级说明自己计入预算（否则「反复超限」
  可以往历史里持续灌说明文本）；图片额度用尽后如实告诉模型「再调用也不会成功，请改用
  文字结论」（旧文案让模型反复重试到轮次上限，全白烧）；降级说明与「画面已加入你的
  上下文」的成功断言不再自相矛盾。
- **门禁的 agent 过滤器口径（AG-23）**：一个 raw 过滤器要在四类分段上用**不同形态**
  匹配，此前没做区分——两条推荐命令**各只覆盖一半表面**（Lua 段命中 0 个文件、或
  跳过 agent E2E）。收敛为 `test/run_all.sh` 顶部单点映射，并新增覆盖全表面的入口
  `bash test/run_all.sh agent-all`。
- AG-24：RLIMIT_NOFILE 偏低时裸 `return` 的用例改显式 `TH_SKIP`——**跳过必须显式**，
  不能混进通过位。AG-25：两条 agent 记忆行为补覆盖（第 2 轮失败时第 1 轮已完整回传的
  回执必须留在历史；最终回答的 `extras` 必须随 assistant 消息留历史，否则 DeepSeek
  400）。

### 2026-09-21 第二轮审计修复批次（AG-11：覆盖率口径 + fork/exec 盲区）

- **结论：不加行覆盖阈值**——实测该指标既漏链路又有结构性盲区，设阈值只会把测试压力
  挤到测得到的区域。
- **fork/exec 区块在 gcov 报告里恒为 0%**，且不是「没测」而是「测了测不到」——计数在
  进程退出时落盘，而这两个函数永远以 `execve` 结束。AG-3 的标志错误正在这个盲区里藏
  了很久。解法是让子进程**自己报告**走了哪条路径（`BOT4CJ_TRACE_FDS` 轨迹），父进程
  读轨迹断言。
- 覆盖率口径修正（陈旧 `.gcda` 混数据、`mybot` 未插桩、两份 `.gcno` 互相覆盖）后，
  同一份源码 77.90% → 79.96%——e2e/TCP/Lua 链路第一次真正进入统计。

### 2026-09-21 第二轮审计修复批次（AG-4/AG-8/AG-18）

- **任务内聚合体积预算（AG-4）**：`maxHistoryBytes` 只管已完结的旧任务块，**正在进行的
  任务**没有任何体积上限——一旦网关以 413/400 拒绝，那块历史既不在回滚范围也不能被裁
  ，REPL 从此每条任务都以同一请求体失败。现在文本与图片分别计账（2 MiB / 4 MiB，
  0 = 不限），超限的那次结果**不进历史**、换成说明回给模型——为什么是「换掉」而不是
  「截断」：截断会让模型看到半截内容却以为完整（比没有更危险）。
- **提示词体积检查前移（AG-8）**：不依赖硬件的三段在 `buildAgentConfig` 就查，超限在
  **上电之前**失败（**破坏性**：退出码 1 → 2，报错文案对应更新）。
- **相机抓帧的边角语义（AG-18）**：`poll` 的 errno 不再一律报「等待帧超时」（EINTR 与
  句柄失效被说成「摄像头卡住」）；`V4L2_BUF_FLAG_ERROR`（内核明说本帧不可信）置位即
  整帧丢弃；buffer 越界两类成因分别点名且无论哪条路径都把缓冲还回队列。

### 2026-09-21 第二轮审计修复批次（AG-9/AG-13 余部）

- **`capture_image` 的 MIME 改由内容决定（AG-9）**：宿主此前恒拼 `image/jpeg` 标签，
  传一张 PNG 就得到「JPEG 标签 + PNG 字节」，严格网关直接 400。现在按**魔数**判定
  jpeg/png/gif/webp（不认识的格式响亮拒绝），宿主再做一次自洽交叉校验后才发出
  （**破坏性**：非图片文件从「照发」变为 build 阶段拒绝）。
- **文档与实现对齐**：图片消息契约改为宿主直接产出 content 数组（照旧文档写第三方
  驱动会永远收不到图片）；被拒选项清单补齐 camera 四项；随包提示词删掉「配置
  `allow_shell: true`」这种**会被拒绝**的开启方式。

### 2026-09-21 第二轮审计修复批次·测试骨架（AG-5）

- **零断言即失败**：一条断言都没跑就结束的用例此前照样记 `[PASS]`——「环境不满足就
  return」的守卫整条静默通过、什么都没验证却占了通过位。现在零断言**判失败**并要求
  显式 `TH_SKIP("原因")`；断言之后再跳过属语义矛盾，判失败。
- **骨架自证**（新守门分段）：用已知形态喂给骨架断言判定符合预期——它守的是**所有
  其它守门都依赖的那把尺子**：尺子失真时全部用例照样全绿，没有别的机制会发现。写
  自证时暴露并修掉三个骨架自身缺陷（跳过会抹掉真实失败位、失败不记账、全跳过误报为
  过滤器拼错）。

### 2026-09-21 第二轮审计修复批次·续（AG-6/AG-10/AG-13②/AG-17）

- `run_command` 超时的「**整组**终止」补有效断言（同组孙进程必须一起死；改成只杀直接
  子进程判据即红）。放弃回收不再静默（宽限内收不回子进程时 stderr 告警点名 pid 与
  「会保持僵尸」的后果——常驻 agent 自己还活着，init 不会接手）。
- DeepSeek 缺省端点也有 stderr 注记（「数据流向哪里」最该让操作者看见的链路反而
  静默回退）。`thinking = false`（布尔）能关思考模式了（此前只认字符串，与该函数
  自己的注释矛盾；且关闭时不再回传 `reasoning_content`）。

### 2026-09-21 第二轮审计修复批次（AG-1/AG-2/AG-3/AG-14/AG-15/AG-16，agent 子系统）

- **非法 UTF-8 不再进请求体（AG-1，高）**：任一坏字节（`cat` 到二进制、Latin-1 文件名
  、脚本首注释）都会让严格网关直接 400，而那条消息已落在**不可回滚**的历史里——
  REPL 会话从此每次以同一字节偏移失败。宿主侧在「数据进入模型上下文」的三个边界消毒
  （RFC 3629 判据，坏字节替换 U+FFFD）+ 发包前 Lua 侧兜底。**归因修正**：报文由
  `json.lua` 编码，不在宿主 `Json::toString()` 路径上——修复落点据此确定。
- **假网关补齐真机规则（AG-7）**：非法 UTF-8 → 400、`reasoning_content` 有状态要求、
  `tool_call_id` 严格一一配对——假网关此前比真网关宽松，缺陷在离线守门里结构上
  不可见。
- **`Camera.lua` 抓帧重试覆盖全局 `err`（AG-2，高）**：一个局部变量名把驱动 ERR 构造器
  覆盖掉，此后**同一 lua_State 里所有驱动**的错误构造全部崩坏，真实诊断 100% 不可达。
  改用局部变量 + 入口前置校验（fail-closed）。
- **`close_range` 标志修正（AG-3）**：`CLOSE_RANGE_CLOEXEC` 写错成 `1U` → 内核 EINVAL
  → 快速路径**从未执行**（每次白跑约 40ms 退化循环），注释却写着「一次系统调用」；
  硬上限兜底如实上报（不再伪装成「正常结束但退出码 -1」）。
- **API key 拒绝控制字符（AG-14）**：多行密钥会把换行后的内容变成**额外的请求头**发给
  网关（libcurl 对 slist 里的 CR/LF 不做拒绝）。宿主侧与驱动侧两条路一起收口。
- **必填字段显式断言（AG-15）**：`json.lua` 里「字段为 nil 即报错」的分支是**死代码**
  （Lua 表不可能有「键在、值为 nil」的项），必填缺失此前被静默丢掉、真机只回一个含混
  400。改由组装处点名断言（字段名 + 第几条消息）。
- **句柄收尾不留悬垂回调（AG-16）**：`llm_http` 按「**先 reset 再 free**」收尾——把
  「同句柄 perform 两次 / 加重试」这类重构从潜在的内存安全问题变成句柄自身的不变量
  （实测触发即 SIGSEGV）。

### 2026-09-21 F5 正确性批次（Json 零拷贝 / SQL 行列号 / UART 共享告警 / Lua 保护帧）

- `Json::find(key)` 零拷贝访问（热路径免整值拷贝）；SQL 词法报错带 1 起行列号，
  **未闭合引号跨越 EOF 从静默放行改为报错**（**破坏性语义修复**：值此前被无声截断）；
  BH1750 常量机械对照 lint（故意改测试常量即红，「两边同错不报警」的预言机缺口收口）；
  同物理串口多独立 fd 装配期告警；Lua C-API 移入保护帧（longjmp 构建下裸崩 → 折成
  既有 ERR 契约并保留真实报错）。

### 2026-09-21 收口批次（守护停机 / 子 shell fd / 驱动契约 / 守门防静默）

- **守护停机二段屏障**：`deactivate()` 在动硬件之前，先给「join 超时被弃管」的守护
  一次真正退场机会（宽限封顶 30s）——此前弃管线程仍可能在逆序 `destroy` 期间碰总线；
  耗尽仍活着则 stderr 点名（宁告警不挂死）。
- **子 shell 继承 fd 全量吊销**（真实 `RLIMIT_NOFILE` + fork 前按 `/proc/self/fd` 兜底
  ）——器件 fd 不再可能被子命令继承；驱动 `destroy(nil)` 补入参守卫返 ERR；7 处死
  绑定清理。
- **守门防静默**：`test/cpp/*.cpp` 未登记进 CMake 清单即 `FATAL_ERROR`；luajit 缺失时
  显式 SKIP 守卫（此前静默注册 0 个 Lua 用例）。JSON 往返保值属性测试。
- **系统提示词总量上限（64 KiB，fail-closed）**（**破坏性**：超限部署需先精简）；
  `--help` 补全选项区间（消除「配置文件接受、命令行拒绝」的判据漂移）；Lua 驱动层
  报错统一英文（**破坏性**：按报错整串匹配的调用方需同步）。

### 2026-09-20 审计收口·第四阶段（agent 模式六条 A–F）

- 环境密钥第二道防线（注入后就地抹零，同 UID 子命令经 `/proc/<pid>/environ` 读不到；
  `ptrace` 级残余如实声明）；轮次上限不再回滚整轮（已完整发生的动作历史保留——硬件
  不会倒回去）；`capture_image` 补注册前提（解析不到摄像头驱动就不注册——原先模型借
  「拍照」就能让舵机 `play()`）；`--camera-src` 不再回落 `.`（回落等于放行整个 CWD）；
  单张图 2 MiB 硬上限（一张仿真图 base64 后约 21 MB，一次拍照即可击穿历史预算）；磁盘
  内容统一按「数据不是指令」标注进系统提示词。

### 2026-09-20 审计收口·第三阶段（M2 重复判据单点化）

- 同一判据多份拷贝收敛为单一事实源，**无行为变更**：相对路径判据 → `util::checkRelPath`
  ；受限读取 → `util::readFdNoFollowLimited`；有限数值解析 → `util::parseFiniteDouble`
  ；十份驱动的本地 `clampi` → `drv_common.clampi`。

### 2026-09-20 审计收口·第二阶段（遗留三处）

- kalman 的 Q=±inf 不再产生 NaN（按极限取 `kg=1`，兑现「Q 无穷 = 只用测量值」的既有
  语义）；LTC2944 电量换算系数单一事实源（注释比代码多乘 1000，已修正）；json 数组
  null 有损点补声明。

### 2026-09-20 审计收口·第一阶段（门禁闭环 / 沙箱 / 非有限值防线）

- 门禁闭环：UBSan 补 `-fno-sanitize-recover`（此前检出未定义行为退出码仍为 0，硬门禁
  结构上不可能红）；ioctl/UART/V4L2/CURL 常量自检改由 Lua 源生成；`rc==125` 须有
  `[SKIP]` 横幅才记跳过（堵 `os.exit(125)` 洗白）。
- ⚠️ `require` 不再能按模块名加载文件（驱动加载统一走 `mpath.resolve` + `dofile`）；
  ⚠️ 非有限值（NaN/±inf）不再被静默接受（`GPIO` 的 `value="nan"` 不再把线驱动到高
  电平、`wait`/`timeout` 传 NaN 不再无界挂死等）。
- 守护字段数超槽宽加载期报错；守护退出路径 `pcall` 包裹 `destroy`；文档失实收口。

### 2026-09-20 评估批次（安全缺省翻转 / C2 同族收口 / 门禁硬化）

- ⚠️ **破坏性**：`run_command` 缺省不再提供，须 `--agent-shell` 显式开启（同日收紧：
  配置文件这条路也已关闭，只能由命令行开启）。
- ⚠️ **破坏性**：移除 `--llm-key`——命令行密钥经 `/proc/<pid>/cmdline` 暴露；密钥只走
  环境变量链或配置文件。
- ⚠️ SQL 词法层把括号当分隔符（`VALUES(1,2)` 紧凑写法此前报词法错）。
- C2 同族收口：`Ema`/`Kalman`/`BH1750`/`GPIO` 的 NaN 入参统一回落缺省，不再「发布
  NaN 却报 OK」；SPI/UART 参数校验下沉 `bus_contract` 单点（消除「真机静默 floor、
  仿真拒绝」的同参数两种结局）。
- CI 门禁硬化：clang-tidy 退出码修正（此前取管道末端恒 0）、三条软门禁设可判定转硬
  条件、新增 `gate:coverage` 与 `gate:musl`；「帮助 ↔ docs 选项表」改逐文档判据。

### 2026-09-20 审计修复批次（C1–C11）

- ⚠️ **崩溃修复**：Lua 驱动一次返回多个值时宿主段错误（`lua_next` 错栈槽）。
- ⚠️ INSERT 按列名落位：列序≠建表序此前**静默错列**且绕过 Number 校验；列名未知/
  重复/漏列解析期报错。
- NaN 不再穿透数值校验（`i2c_write_reg(fd, reg, NaN)` 此前返回成功但写 0x00）；矩阵
  单元格报错截断 + 转义；MAX30102 FIFO_CONFIG 位域按数据手册修正；Camera 打开失败
  统一释放 fd/mmap。
- 守门自纠四条：错误过滤器假绿、零用例 C++ 二进制判失败、退出码改 0/1（旧版 256 个
  失败回绕成 0）、孤儿夹具检测改整字段匹配。

### 2026-09-20 重构批次（CLI 可测化 / 卫生收口）

- CLI 迁 `src/cliargs.*`（`--help` 输出逐字节不变），新增「帮助 ↔ docs 选项表双向
  一致」守门；守门自检 `test/run_all_selftest.sh` 起步。
- ⚠️ `run_command` 环境清洗改**整词匹配** + 复合密钥名表（`KEYSTONE` 等无关变量不再被
  误剔，真密钥仍剔除）；词法报错文案格式微调。纵深防线与卫生收口若干。

### 2026-09-20 重构批次（会话作用域 / 模块路径 / I/O 语义）

- ⚠️ **破坏性**：动作脚本内联 `@Alias`/`@Vars` 作用域收敛到「本次执行」——此前进程级
  ，会永久遮蔽结构定义并跨脚本残留；跨脚本共享别名须写入结构脚本。
- 多路径模块加载对模块侧成立（宿主注入 `__mpath`）；`run_command` 的 EINTR 当重试、
  TCP 未认证对端不再能杀进程（只有凭据不匹配计入爆破上限）、超限文件报「超过字节
  上限」（不再伪装成语法错误）；协议驱动入 `arg_table` 契约 + 全模块 sweep；CI 构建
  带 `-DMYBOT_WERROR=ON`。

### agent 模式（`--agent`）【历史：2026-09-24 移除，现为两进程拓扑（`mybot --mcp-backend` + `mybot-agent`），见 2026-09-24/25 各条】

- 模式本体：LLM 工具循环为主执行流程，硬件常驻内存，会话结束才 `deactivate()`；
  `--task` 一次性任务，缺省交互 REPL。
- 工具集：`list_action_scripts` / `run_action_script`（路径沙箱 + 8 MiB 上限）/
  `list_hardware`（只读）/ `capture_image`（V4L2 + MJPEG，路径白名单）/ `run_command`
  （超时进程组终止 + 环境清洗，**缺省不提供**）——工具集在两进程拓扑下仍由 C++ 后端
  承载，并陆续扩了 `query_sql`/`apply_sql`/`read_variables`。
- 模型调用是一颗 Lua 协议驱动（`LlmOpenAI` / `LlmDeepSeek`，后补 `LlmAnthropic`）：
  换模型 = 换一个 Lua 文件，宿主零链接期依赖。
- 配置文件 `--llm-config`：优先级 命令行 > 环境变量 > 配置文件 > 驱动缺省，严格校验
  非法即退出 2（配置是数据不是代码）；提示词包 `--prompt-file`。

### 语言与 DSL

- **`$(...)` 表达式**：`@Vars`/元数据列/`@Action` 单元格三处可用（四则运算 + 括号，
  深度/运算符双上限）；文本操作数分派为拼接（别名入式），落点扩到 `@Action` 全部
  单元格；不引入裸 `+` 语法。
- **`@Action` 设备列 `@Alias`**：模板设备列经别名改绑复用片段；手写重复列由静默覆盖
  改报错。
- **`@Include` v2**：嵌套 + `*.once.mt` include-once + 主从覆盖（更靠近主脚本的定义
  获胜）；v1 内联归并复用 `mtmerge_core`，未知矩阵由静默忽略改报错。
- **controller/trigger 多列扇出**：一行多目标；controller 一个 ERR 即停止，trigger 按
  序前置调用。
- **`@Vars`** 命名数值变量（元数据列 `$<name>` 与参数位替换两种消费）；`Number` 列
  解析期校验；词法器引号内字面量与 8 MiB 上限；mtmerge 归并 `@Alias`/`@Vars`。

### 执行引擎与安全

- **守护槽地址注入改 `lightuserdata`**：「地址被 double 化后 ≥2^53 丢位」整类风险在
  语言层面消除。
- **TCP 加固**：非回环强制口令 + 恒时比较 + 空闲/总预算超时；口令可经
  `BOT4CJ_TCP_TOKEN` 传递。
- **动作脚本路径沙箱**：`O_NOFOLLOW` 打开，软链条目不进工具清单（目录外文件首注释
  不再回显进模型上下文）。
- **浮点窄化前判有限性与值域**（float-cast UB 响亮拒绝）；`$` 引用校验前移到任何
  `build` 之前；`destroy` 元数据恢复引用列（修复十余款器件静默丢失去初始化）。
- **派发热路径优化**：列头只解析一次、表达式 AST 按列缓存；沙箱拒绝 NUL 键值、未知
  `--key` 退出 2。
- **沙箱残余如实声明**（`load`/预编译字节码/`ffi`）：威胁模型内驱动可信，列为有意
  取舍；`os.tmpname` 并入中和。
- **异常体系归口** `IllegalArgumentException`；报错文案分语言惯例成文于
  [development.md](development.md)。

### 驱动与总线

- **26 个器件驱动 51 处内联类型检查迁 `arg_table`** + lint 防线（`proto=virt` 注入
  前置不再被绕过；Camera 有意豁免）。
- **多路径布局定位逐段探测**（`HAL`/`virtbus`/`drv_common`），配套非缺省布局冒烟用例。
- **PCA9685 退出 SLEEP 按 datasheet**（写 `AI|RESTART`）；**UART** 波特率改 `B*` 常量
  （旧版从未生效）、termios 快照恢复。
- **新增 `SCSCL` 总线舵机驱动**（飞特 FT-SCS 协议；`set_id` 站号烧写须结构期
  `allow_burn` 授权）。
- **virtbus/驱动加固**：读族校验对齐 HAL、先实例化再 attach、`guard_build` 全总线接入
  、`sensor_daemon` 浮点防线、`bitcompat` 符号扩展原语。

### 工程化与守门

- **大文件拆分**：`robot.cpp` 按类拆四个 TU（只剩 114 行编排门面）；`run_command`
  子进程机器独立 `agent_subprocess.*`；`ActionPlan::build` 抽为白盒可测单点。
- **结构重构**：`CliOptions` 类型化、`HwRegistry` 三容器、守护状态机收敛 `DaemonRunner`
  、`RobotStruct` 拆 Loader/Factory/Runner、守护槽所有权改 `shared_ptr`。
- **守门基建**：HAL 常量编译期自检、CI 三硬门禁 + 五软门禁、`test_curl_consts` 落地、
  `run_san.sh --asan-only`、agent CLI 负路径守门补齐。
- **重复代码九类收敛单点**（`isSafeIdent`/`makeErrResult` 等；三类经评估刻意不收敛并
  注明）；巨函数拆分十项。
- **覆盖补齐**：守护并发压测 + TSan、TCP 异常注入七场景、fd 泄漏检测、离线 ABI 回归
  ；文档全量通读排版。
