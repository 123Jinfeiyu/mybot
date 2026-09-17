# bot4cj 重构计划(dev_cxx · C++17 版)

> 基线:`dev_cxx` @ `509d8a7`(2026-09-17,工作树干净)。配套事实与未决修复项见
> `.tmp/cpp-report-20260917.md`(不入库,其 `F-xx` 编号本文直接引用)。
> 本文只谈**结构调整**(职责、边界、数据流),不重复罗列已入账的修复项;
> 每条重构标注吸收/联动哪些 `F-xx`。

## 落地进度(2026-09-17)

| 阶段 | 内容 | 提交 | 验证 |
| ---- | ---- | ---- | ---- |
| 0 | 分支归宿:用户拍板「暂不决策」(推进 1-3 不受影响) | — | — |
| 1 | 错误通道 stderr / util 收敛 / CliOptions / rule-of-three / Json::put / CMake | `ef1a8f2` | 双跑 141/0/0,单测 101 |
| 2 | HwRegistry 注册表统一 | `5a22744` | 双跑 141/0/0,单测 104 |
| 3 | 硬件变量虚方法化(controller 改注册表查名) | `08d0f05` | 双跑 141/0/0,单测 104 |
| 4 | MatrixBuilder / appendRow 解析管线解耦 | `6e74239` | 双跑 141/0/0,单测 104 |
| 5 | DaemonRunner 状态机内聚 + abandon/never-spawn 确定性夹具(F-08 尾项) | `41991aa` | 双跑 141/0/0,单测 106 |
| 6 | RobotStruct 拆分 ScriptLoader/HwFactory/ActionRunner | `781a160` | 双跑 141/0/0,单测 106 |
| 7 | C 常量自检接入守门第 8 段 + .gitlab-ci.yml + tcp_e2e 加固(F-22/24/28) | `63cacbf` | 双跑 142/0/0,单测 106 |
| 8 | F-27 文档漂移清扫(TCP 措辞/architecture_analysis 历史标注/virtbus 计数/README 版本) | 本提交 | 纯文档 |

剩余:阶段 0 分支归宿(用户暂不决策);推送待确认(阶段 7/8 两提交未推送)。

---

## 1. 现状快照

```
src/ 10 对 .hpp/.cpp ≈ 3100 行,零第三方依赖,-Wall -Wextra -Wpedantic 零告警
modules/ 56 个 Lua 驱动 + lib/ 运行时(HAL 1089 行 / virtbus 808 行)
test/cpp/ 106 单测;test/run_all.sh 八段守门(142 用例全绿)
```

数据流与职责边界(读码确认):

```
.sql --Reader--> sql4cj::Lexer/Parser --Event--> robot.cpp::readSQL 逐字符重建 MatrixObject
                                              --> RobotStruct::createHardVars (Alias/Lua/Daemon 分派)
                                              --> active() 两阶段 build
.mt --LimitedReader--> mt::MatrixParser --> @Alias 注册 + runAction 逐行 play
    --> deactivate():守护统一置 stop → 声明序逆序 destroy
```

关键设计(值得保持):每硬件变量独立 `lua_State`;守护共享槽 C++/Lua 双侧 static_assert
互锁(336B 布局);`call_lua` 单入口且栈深异常安全;负值契约测试文化;virtbus 让真实驱动
离线可测。

## 2. 结构问题诊断(重构候选)

| 编号 | 问题 | 证据 | 联动 |
| ---- | ---- | ---- | ---- |
| R-1 | **RobotStruct 上帝类**:8 项职责混于 380 行 | `robot.cpp`:SQL 装配(:340)、矩阵读取(:199)、注册表(:93)、工厂分派(:131)、别名(:105)、重放(:258)、生命周期(:207/:219)、save | — |
| R-2 | **注册表三容器手工同步**:`hardwareVars_`(名→裸指针)+`hwPool_`(所有权)+`hwSeq_`(顺序) | `robot.hpp:52-54`;会话期别名覆盖在 `robot.cpp:122` 直接改写 map,结构变量留在 `hwSeq_` 而 map 指向别名,不变式被打破 | F-25 同类 |
| R-3 | **std::function 回调 + 裸指针捕获**(仓颉移植痕迹):`MtVars_Daemon` 以 lambda 捕获裸 `DaemonHolder*`;`controller_` 为裸指针 | `daemonvar.cpp:325-327`、`luavar.hpp:30`;生命周期全靠"先声明后引用 + 逆序销毁"调用纪律 | F-14 同一主线 |
| R-4 | **解析管线双重中间态**:SQL Event → `readSQL` 逐字符 `addRune` 重建;`MatrixObject` 同时是增量构建器与值访问器(公开可变 API + mutable 继承缓存) | `robot.cpp:349-376`、`matrix.hpp:140-162` | — |
| R-5 | **错误类型与通道混乱**:5 种异常 + 全部走 `std::cout` | `util.hpp:23` / `matrix.hpp:16` / `hardware.cpp:21` 等;`src/` 内 `std::cerr` 0 命中 | F-21、F-26 |
| R-6 | **CLI 解析分散且重复**:`main.cpp::parseArgs`+白名单、`actionsource.cpp::parseLong`、`daemonvar.cpp::parseInterval` 三处独立整数解析校验 | `main.cpp:22-50`、`actionsource.cpp:157`、`daemonvar.cpp:189` | F-12 |
| R-7 | **DaemonHolder 状态机裸字段操作**:`spawned/abandoned/slot/promise` 由三个自由函数分散读写 | `daemonvar.cpp:56/221/244`;D2 竞态窗口只能靠注释约定"宁漏不崩" | §4.2 尾项 |
| R-8 | **并发靠纪律不靠类型**:`pthread.lua` 裸读写 `stop`;TSan 从未真正跑过一次 | `pthread.lua:183-188`;本机内核不支持 TSan | F-14 |
| R-9 | **仓库双历史并行**:`main`/`dev`(仓颉线)与 `dev_cxx`(C++ 线)无共同祖先;Lua 层漂移 14 文件 +1035/−87;`origin/mybot-cxx` 第三份 | §2.1 实测;`.tmp/fresh1/` 实物 | 战略项 |
| R-10 | **门禁缺席**:762 行守门脚本全凭人工;2 个 C 自检文件永不运行;5 份 `build*/` 目录 | `run_all.sh`、`test/test_ioctl_nums.c` | F-22、F-24、F-28 |

## 3. 分阶段重构计划

原则:每阶段独立可回归、可单独提交;结构改动不改用户可见行为;
`F-xx` 修复项顺路吸收,不顺路的不硬塞。

### 阶段 0 · 战略决策(前置,半天,不写代码)

**R-9 分支归宿二选一,先拍板再动工:**

- (a) **合流**:C++ 版入 `main`,仓颉版转 `legacy/cangjie` 分支或 tag;`modules/` 以
  `dev_cxx` 为准一次对齐。
- (b) **双轨**:承认两线独立,建立 Lua 层同步清单。

不决策的代价:后续每个 Lua 改动 ×2 维护成本,F-06/F-16 铺开前必须回答。
**建议 (a)**——C++ 版已全面超过仓颉版(141 用例、sanitizer 双跑、驱动数持平),
仓颉线保留为历史价值即可。

### 阶段 1 · 低风险机械重构(1 天,逐 commit)

| 步骤 | 内容 | 吸收 |
| ---- | ---- | ---- |
| 1.1 | **R-5 错误通道**:错误/告警统一走 `stderr`,数据走 `stdout`;建立"用户错误 vs 内部错误"两层异常命名(不动现有 catch 语义) | F-21 |
| 1.2 | **R-5/R-6 基础收敛**:整数解析+校验、`trim`、`getEnvVar` 进 `util`;`parseInterval`/`parseLong` 合并为一个带范围参数的助手 | F-26 之一 |
| 1.3 | **R-6 CLI 类型化**:`main.cpp` 出 `CliOptions` 结构(robot/tcp/host/token/idle/input/help 强类型),校验一次,`actionsource` 只收类型 | F-12 前置 |
| 1.4 | rule-of-three:`FileReader`/`TcpStreamReader` 删拷贝构造/赋值 | F-25 |
| 1.5 | `Json::put` 非对象显式报错(对齐 `asObject` 风格) | F-19 |
| 1.6 | CMake:`mybot`/`mybot_tests` 重复的编译选项/链接库收敛为对象库或 INTERFACE 目标 | — |

验证:每步 `bash test/run_all.sh`(141) + `mybot_tests`(101) 全绿;
sanitizer 双跑一次;零告警。

### 阶段 2 · 注册表统一(1 天)

**R-2**:以单一 `HwRegistry` 类取代三容器:

```cpp
class HwRegistry {
    std::vector<std::unique_ptr<MatrixVars_Hardware>> pool_;   // 所有权+声明序
    std::map<std::string, MatrixVars_Hardware*> byName_;       // 只读索引,随 pool_ 派生
public:
    void add(std::string name, std::unique_ptr<MatrixVars_Hardware> hw);  // 判重
    void override(std::string name, std::unique_ptr<MatrixVars_Hardware> hw); // 会话期别名,明确"被盖者仅退出索引、仍受销毁"语义
    MatrixVars_Hardware* find(const std::string&) const;
    // 顺序遍历/逆序遍历迭代器
};
```

把"会话期别名覆盖结构定义"的语义显式化(当前靠注释+散落代码维持)。
`RobotStruct` 只保留 registry 与装配流程。

验证:别名覆盖、重复名报错、逆序 destroy 各补 1 条单测(锁定当前行为)。

### 阶段 3 · 硬件变量虚方法化(1.5 天)

**R-3**:去掉 `PlayFn/NoArg` 回调注入,`MatrixVars_Hardware` 改纯虚接口:

```cpp
class MatrixVars_Hardware {
public:
    virtual Json play(const Json&) = 0;
    virtual Json read() { return play(JsonObject()); }
    virtual void build() {}
    virtual void destroy() {}
};
```

- `MtVars_Alias`:真实子类,持 `std::string alias_` 成员(取代 lambda 捕获)
- `MtVars_Daemon`:真实子类,持 `std::shared_ptr<DaemonHolder>` 成员(取代裸 `DaemonHolder*` 捕获)
- `MtVars_Lua`:`controller_` 从裸指针改 registry 查名(`std::string ctrlName_` + 运行期
  `find`),彻底消灭"声明序决定生死"的隐性依赖

风险点:`MtVars_Daemon` 改持 shared_ptr 后,holder 生命周期从"registry 独占"
变为"变量+registry 共持",`daemonFree` 的所有权移交逻辑(D1/D2)要逐分支复核。

验证:全量守门 + sanitizer;重点看守护 E2E(test_daemon/test_multidaemon)与 abandon 路径。

### 阶段 4 · 解析管线解耦(1 天)

**R-4**:
1. `MatrixObject` 拆出 `MatrixBuilder`(仅 `sqlparse`→矩阵 与 `MatrixParser` 使用);
   `MatrixObject` 公开面收敛为只读访问器 + `inherited()` 缓存。
2. `RobotStruct::readSQL` 不再逐字符 `addRune`,Event 直接构造
   `MatrixBuilder`/`MatrixObject`(字符串本就完整在 Event 里,逐字符是仓颉流 API 的残留)。

验证:`test_sqlparse.cpp`(304 行)+ 引号态/大输入用例全绿;行为零变化(此为纯管线替换)。

### 阶段 5 · Daemon 状态机封装(1.5 天)

**R-7/R-8**:把 `daemonRise/daemonFree/daemon_body` 收敛为 `DaemonRunner` 类,
状态机(未 spawn / 运行 / stop 请求 / join 完成 / abandoned)内聚:

- 槽的 allocate/release、stop 请求、join 超时、abandoned 移交全部成成员函数
- 对外只留 `start()` / `requestStop()` / `waitForExit(timeout)` / `readSample()`
- 保持 336B 槽 ABI 与 Lua 侧互锁不动(那是 FFI 契约,不是重构对象)
- 同批补:abandon / never-spawn 两条分支的确定性测试夹具(F-08 尾项);
  换宿主跑一次 TSan(本机内核不支持,CI 上补)

验证:守护全部 E2E + 新夹具;sanitizer 双跑。

### 阶段 6 · RobotStruct 拆分(1.5 天)

**R-1**:按职责切三个协作对象,`RobotStruct` 退化为薄编排门面:

| 新类型 | 职责 | 吸收自 |
| ------ | ---- | ------ |
| `ScriptLoader` | readSQL(readMatrix)、SQL Event→矩阵装配 | robot.cpp:199/:340 |
| `HwFactory` | createHardVars/createDaemonVars/createLuaVars、别名注册、MetaProvider 装配 | robot.cpp:105-187 |
| `ActionRunner` | runAction/parseActionCell/parseActionColumn、stop 检查 | robot.cpp:258-337 |

`MetaProvider` 保留但去掉对 `hardwareVars_` map 的裸指针(阶段 2 后收 `HwRegistry&`)。

验证:纯移动不改逻辑;全量守门;`git diff` 审"每处修改都只是搬家"。

### 阶段 7 · 门禁基建(1 天)

**R-10**(吸收 F-22/F-24/F-28):
1. 两个 C 自检文件接入 CMake 目标 + `run_all.sh` 一段
2. CI 最小可用:一条流水线跑 `bash test/run_all.sh`,进阶加 `run_san.sh`
3. `scripts/tcp_e2e.sh` 用 `mktemp`、断言服务端退出码
4. `.gitignore` 已含 `build*/`——根目录 5 份 build 目录确认属本地产物即可,不入库

### 阶段 8 · 文档对齐(0.5 天)

**R-9 决策落地后**:`architecture_analysis.html` 重写为 C++ 版或顶标"历史文档";
F-27 表逐条清扫;本计划完成度回填 `docs/changelog.md`。

## 4. 与审计台账(F-xx)的衔接

- 阶段 1 吸收:F-19、F-21、F-25、F-26(部分)
- 阶段 3 联动:F-14(同一"纪律改类型"主线)
- 阶段 5 吸收:F-08 尾项、F-14、TSan 复跑
- 阶段 7 吸收:F-22、F-24、F-28
- **不在本计划内**(纯修复项,按 `.tmp` 报告的 §9 次序另行推进):
  F-03(沙箱)、F-06(virt 执行器铺开)、F-09/F-10(断言补齐)、F-11(配置转义)、
  F-12(产品决策:流式播放 vs 改措辞)、F-13/F-16(virtbus 层)、F-17(驱动零散)、F-27(文档)

## 5. 风险与护栏

1. **行为锁定**:每阶段验收标准都是"141 守门 + 101 单测 + sanitizer 全绿、
   用户可见输出零变化";结构改动单独提交,禁止与功能修复混提交。
2. **阶段 3 是最大风险点**(生命周期语义改动),单独拉分支、守护 E2E 逐条比对输出。
3. **顺序依赖**:0 → 1 → 2 → 3 强顺序(2 为 3 铺路,3 为 6 铺路);
   4/5 可与 2/3 并行但建议串行以缩小 diff 面。
4. 每个阶段完成后立即 `git push`(pre-push 守门自动全量回归)。

## 6. 总体工期

约 8 个工作日(不含阶段 0 决策等待):结构重构 5.5 天 + 门禁 1 天 +
文档 0.5 天 + 缓冲 1 天。
