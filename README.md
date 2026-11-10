# BattleWear — 战损服装系统（NARUTO X BORUTO / NUCC / XFBIN）

分级、递增、**始终附着**的服装破损系统。面向 NARUTO X BORUTO Ultimate Ninja Storm
Connections（Steam，`NSUNSC.exe`，CyberConnect2 NUCC 引擎），可挂进现有 JAPI /
ModdingAPI / NSC Mod Manager 工作流，不替换任何原版文件。

## 0. 硬约束（全部由代码与测试强制，不是文档承诺）

| 编号 | 约束 | 落地方式 |
|---|---|---|
| C1 | 服装**永不**脱落、隐藏、卸载、整体渐隐 | `RenderOp` 枚举里只有 3 个**增量/改参**操作，无 hide/detach/unload/alpha=0；`ValidateFrame` 检测到即报 `GARMENT_REMOVED` |
| C2 | 每个逻辑部位**始终**绑定基础网格与基础材质 | `PanelVisual::baseMesh/baseMaterial` 为空即 `BASE_MESH_MISSING` |
| C3 | 破洞**只是布料缺口，缺口底下是内衬布**，不是皮肤 | 每帧强制 `underlayer >= holeCutout`，且每个 cutout 必须配一个 lining（`UNDERLAYER_MISSING`） |
| C4 | 单场战斗内损伤**单调不减** | 所有通道写入统一走 `Monotone()`，静态审计逐个写入点核对 |
| C5 | 覆盖率 ≥ 80%，总破损面积 ≤ 18%，单部位缺口 ≤ 22% | 数学上限由构造保证（面积归一 + 全局预算分配），并有逐帧断言 |
| C6 | 不绑定任何固定服饰资源，任意外部服装 MOD 都能挂 | `CostumeRegistry` 兜底生成通用绑定，缺失资源降级为材质/贴花层，不崩不穿模 |

> C3 是这套系统与「爆衣」的分界线：破损的尽头是**内衬外露的破洞**，不是裸露。

---

## 1. 目录

```
BattleWear/
├─ include/battlewear/
│   ├─ battlewear.h        # 全部数据结构与接口（单头文件契约）
│   └─ mini_json.h         # 无依赖 JSON 解析（不引入第三方，DLL 体积/ABI 干净）
├─ src/
│   ├─ config.cpp          # 配置加载 + 硬约束强制回写
│   ├─ damage_model.cpp    # ★ 逻辑层：累积、扩散、层级递进
│   ├─ costume_registry.cpp# ★ 动态服装绑定 + 通用兜底
│   ├─ visual_mapper.cpp   # ★ 表现层：层级 → 材质/贴花/附加网格
│   └─ invariants.cpp      # 不变式校验（Release 外可用，Debug 内逐帧）
├─ plugin/
│   ├─ game_adapter.h      # 引擎抽象接口（唯一的游戏侧依赖）
│   └─ bw_plugin.cpp       # 导出 DLL：BW_Create/BW_Tick/BW_OnCostumeChange/...
├─ config/
│   ├─ battlewear.config.json        # 全部可调参数（热改，不需重编译）
│   ├─ battlewear.schema.json        # 配置 Schema
│   └─ costume_bindings.example.json # 服装绑定样例
├─ tools/
│   ├─ bw_runtime.py       # 与 C++ 逐公式对应的可执行参考实现
│   ├─ verify_flow.py      # 全流程 + 边界条件验证（唯一真正跑起来的验证）
│   └─ cpp_audit.py        # C++ 静态审计（无编译器时的结构性校验）
├─ tests/test_main.cpp     # 同一套断言的 C++ 版本（需 MSVC 构建后运行）
├─ CMakeLists.txt / build.bat
└─ out/                    # 验证产物：轨迹、报告、一致性向量
```

---

## 2. 架构：四层，引擎依赖被压在最外层

```
战斗事件 (damage / hitbox / bone)
        │  HitTelemetry
        ▼
┌─────────────────────────────┐
│ 逻辑层 DamageModel          │  纯计算，无引擎调用、无分配、可单测
│ I(t) → wear/tear/hole       │
│ 阶段判定 + 滞回 + 驻留去抖   │
└──────────────┬──────────────┘
               │ DamageState（纯数据）
               ▼
┌─────────────────────────────┐        ┌──────────────────────────┐
│ 表现层 VisualMapper         │◄───────┤ 绑定层 CostumeRegistry   │
│ 层级 → 材质参数/贴花/网格   │        │ (角色码,变体ID) → 部位   │
└──────────────┬──────────────┘        └──────────────────────────┘
               │ VisualFrame（纯数据）
               ▼
┌─────────────────────────────┐
│ 不变式层 ValidateFrame/State│  违规帧直接丢弃，不上屏
└──────────────┬──────────────┘
               ▼
        IGameAdapter::ApplyFrame  ← 唯一碰游戏的地方
```

逻辑层与表现层都是**纯函数式**的：给定同一份 `DamageState` 必然产出同一帧指令，
这是能离线复现、能写一致性向量的前提。

---

## 3. 损伤状态模型

### 3.1 三个物理通道 + 一个战斗强度积分器

不是「打一下加一格」的计数器，而是三条并联的损伤通道，各自有不同的物理行为：

| 符号 | 含义 | 微分形式 | 物理依据 |
|---|---|---|---|
| `I(t)` | 战斗强度 | `dI = E/(norm·τ)`，衰减 `e^(-dt/τ)` | 漏积分器：连续挨打时强度叠高，停手后 ~1.2s 半衰 |
| `wear` | 表面磨损 | `dW ∝ Ew·(1−W)^1.35` | 磨料磨损饱和：越磨越难磨，永不溢出 1 |
| `tear` | 边缘撕裂长度 | `dT ∝ Et^0.85·(T+T₀)^0.65/G·(1−T)` | Paris 裂纹扩展律：裂纹尖端应力集中，长度越大扩展越快 |
| `hole` | 局部破洞面积比 | 需 `tear ≥ 0.32` 且 `drive ≥ need` | 断裂力学：先有裂口，才可能在裂口处崩开 |

其中 `G = 1 − 0.45W − 0.35T − 0.55H`（下限 0.28）是**剩余织物强度**：已经破损的布料承载更差，
所以同样的打击在后期造成更快扩展——这就是「高强度战斗对应显著且连贯的重度破损」的来源，
而不是靠一个开方曲线硬拉。

`drive = Eh + 0.35·E·I·T`：穿刺能量 + 裂口处的循环载荷。
`need = 2.2 · max(0.20, coherence) · max(0.20, fatigueRelief)`：相邻部位已经破洞时
`coherence` 下降（应力被邻居分担），但持续高强度会压低 `fatigueRelief`（疲劳软化），
所以**破洞既会「成片」出现，又不会在第一次重击就炸开**。

### 3.2 受击部位：主部位 + 缝合线扩散

一次命中只作用于一个逻辑部位，但会按 `adjacencyWeight = 0.18` 向相邻部位传导。
测试 `hit-location primary dominance` / `stress diffusion to adjacent panels` 双向锁死：
主部位永远显著重于邻居，但连续打同一处会让周边一起进入撕裂态（连贯性）。

### 3.3 四级状态与升级门槛

| 级 | 名称 | 判定 | 视觉 |
|---|---|---|---|
| S0 | 完好 | 初始 | 原样 |
| S1 | 轻微磨损 | `wearMean ≥ 0.10` | 污渍、粗糙度上升、轻微褪色 |
| S2 | 边缘撕裂 | `tearMean ≥ 0.16` **且** ≥2 个部位 `tear ≥ 0.20` | 破边贴花 + 边缘抖动 + 可选毛边条 |
| S3 | 局部破洞 | 破洞数 ≥ 2 **且** `holeMean ≥ 0.045` | 内衬 + 局部缺口 |

**升级三重保护**：`滞回 0.80`（阈值回落 20% 才允许降级） + `驻留 0.25s`（候选状态需稳定） +
`淡入 0.35s`（新旧层级交叉过渡）。单次爆发打不进下一级，测试里专门有一条
`single burst does not instantly promote`。

`heavyGrade`（0–3）是 S3 之上的**重度表现分档**，独立于阶段：
破洞数 3/5/8 或 `I ≥ 0.55 且 tearMean ≥ 0.55` 逐档抬升，用于放大毛边与缺口半径（×1.36）。
这样「打了很久但只裂不破」和「被连击打穿」在观感上是两件事。

---

## 4. 表现层映射

| 通道 | 输出 | 上限 | 说明 |
|---|---|---|---|
| wear | 材质参数 `dirt / roughnessBoost / desaturation` | 0.85 / 0.55 / 0.30 | 只改参数，不换材质 |
| wear>0.30 | 焦痕贴花（叠乘 0.6） | — | 高温类打击的主要观感 |
| tear | 破边贴花 + `edgeJitter` | 0.95 / 0.40 | UV 边缘抖动，避免整齐的假边 |
| tear + 有变体 | `BIND_ADDITIVE_MESH` 毛边条 | — | **叠加**在原网格上，不替换 |
| hole（S3 层起绘制） | 内衬贴花 → 缺口贴花（两层，按序） | 1.0 | 半径 0.045–0.115，最小 UV 间距 0.16 防重叠 |

每个部位**第一条指令永远是 `SET_MATERIAL_PARAM`（role=`base_keep`）**——这是「衣服还在」的
运行时证据，也方便 hook 侧核对。

指令缓冲上限 512，满则丢弃后续指令（宁可少装饰，不可溢出或降级基础绑定）。

---

## 5. 动态服装兼容（关键设计）

游戏资源按 **角色码 + 变体路径/ID** 索引，不按某件具体衣服硬编码。系统据此做绑定：

```json
{ "costumes": [ {
    "characterCode": "5nrt", "variantId": "default", "damageAssets": "dedicated",
    "panels": {
      "torso_front": { "mesh": "char/5nrt/body_top", "material": "char/5nrt/body_top_mat",
                        "frayVariant": "bw/5nrt/torso_fray", "holeMask": "bw/5nrt/torso_hole" }
    }
} ] }
```

- **已注册**：用 modder 提供的专用毛边网格 / 缺口遮罩。
- **未注册**（任意外部服装 MOD，未做适配）：`Resolve()` **永不失败**，生成
  `auto:whole_garment` 通用绑定，破损以材质磨损 + 通用贴花呈现，`fallbackUsed=true`
  供 UI/日志提示。不崩、不穿模、不黑模。
- **缺单个资源**：只降级该部位的那一层（无 `frayVariant` → 只出贴花；无 `holeMask` → 用通用遮罩），
  其余层级照常。
- 换装 / 切角色：显式 `Reset()`——新衣服是完好的衣服，战损不跨服装继承（测试覆盖）。

---

## 6. 接进现有工作流

1. **资源**：通用贴花（`bw/generic/*`）与毛边 NUD 打进一个 BattleWear CPK，由 JAPI 的
   CPK Loader 加载；**不覆盖任何原版文件**，与替换型 MOD 无冲突。
2. **绑定**：`costume_bindings.json` 可与 NSC Mod Manager 的 costume 模组一起分发，
   一个 modder 一个条目，互不干扰。
3. **插件**：`BattleWear.dll` 由 JAPI / ModdingAPI 加载，把每帧回调转发给 `BW_Tick`：

```cpp
void OnFrame(double dt) {
    if (void* h = BW_Create("BattleWear/battlewear.config.json",
                            "BattleWear/costume_bindings.json", &myAdapter))
        BW_Tick(h, dt);          // myAdapter 实现 IGameAdapter
}
```

4. **命中来源**：`prm/prm_load` 的角色状态、hitbox→骨骼映射到逻辑部位、伤害值归一化到
   `energy ∈ [0,25]`。这些都在 `IGameAdapter::PollHit()` 里由接入方实现，
   核心逻辑不含任何游戏地址/偏移，升级游戏版本不需要重编译核心。

---

## 7. 构建与验证

```bat
build.bat
:: 或
cmake -B build -S . -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
build\bin\Release\bw_tests.exe
```

不调用 `vcvars64.bat`（本机 `reg.exe` 被策略拦截），VS 生成器可直接解析工具链。

**离线验证（无需编译器，当前已跑通）**：

```bash
python tools/verify_flow.py     # 全流程 + 边界条件
python tools/cpp_audit.py       # C++ 静态审计
```

---

## 8. 实测全流程（可复现）

`out/flow_trace.json`，种子 `0x5EED1234`，10 个逻辑部位：

| 时刻 | 事件 | wear | tear | 破洞 | 覆盖率 |
|---|---|---|---|---|---|
| 0.00s | S0 完好 | 0.000 | 0.000 | 0 | 100% |
| 1.15s | **S1 轻微磨损** 提交（稳定后 1.65s） | 0.136 | 0.001 | 0 | 100% |
| 2.85s | 首个破洞**力学成核**（仍在 S1 区间，尚未上屏） | 0.505 | 0.148 | 1 | 99.5% |
| 3.20s | **S2 边缘撕裂**（3 处破边 + 3 个已成核的洞） | 0.552 | 0.223 | 3 | 98.6% |
| 4.15s | **S3 局部破洞**（8 处，开始绘制内衬+缺口） | 0.676 | 0.381 | 8 | 93.2% |
| 19.65s | 持续高强度，grade 3 | 0.913 | 0.900 | 36 | **83.6%** |

> 「成核」与「上屏」是两件事：破洞的物理条件在 2.85s 就已满足（裂口 + 穿刺能量 + 疲劳软化），
> 但缺口贴花与内衬从 S3 才开始绘制。这样画面上看到的是 磨损 → 撕裂 → 破洞 的严格递进，
> 而状态内部不会因为「等表现层」而失真。进 S3 时不是从零突然冒出洞，而是把已经存在的
> 3 个缺口一次揭示出来并继续扩大——观感上更像「布料撑不住了」，不像特效开关。

关键性质：
- 磨损先于撕裂、撕裂先于破洞——**通道有依赖，不是三个独立进度条**。
- 首洞 2.85s → 8 洞 4.15s（1.3s 内连锁成片），随后 15 秒只从 8 增到 36，
  因为面积预算与单部位上限在收口——这是**饱和**，不是卡住。
- 覆盖率 83.6% 始终 > 80% 下限；总破损 16.4% < 18% 上限；单部位 22% 上限未被击穿。
- 停手 12 秒：强度从 1.00 衰减到 ~0（τ=1.2s），36 个洞、覆盖率和所有损伤通道**一格未退**。

验证规模：**24464 项检查 / 0 失败**，逐帧校验 8068 帧，轨迹 1300 帧，28 项边界用例全通过。
静态审计 8 项全通过（且做过反向注入测试：删一个定义、塞一个 `RemoveMesh`、
写一处裸 `p.wear =` 都能被抓出）。

---

## 9. 边界情况全表（`out/edge_cases.json`）

| # | 情况 | 处理 |
|---|---|---|
| 1 | 能量为 0 / 负 / NaN / Inf / 超上限 | `Finite` + `Clamp`，NaN 归一为 0，超大截到 25 |
| 2 | dt 为 0 / 负 / NaN / Inf / 超大 | 夹到 `[0, dtMax]`，负值不推进时钟 |
| 3 | 时间戳乱序 / 未来跳跃 | 不许倒流（夹到已有时钟），跳跃按 `dtMax` 切片推进，最多 256 片 |
| 4 | 部位/打击类型索引越界 | 部位夹到合法范围，未知类型回落 index 0 |
| 5 | 单次爆发 | 驻留 + 滞回挡住瞬时跳级 |
| 6 | 阶段回退 | 滞回 0.80 + 单调损伤，实际永不降级 |
| 7–8 | 打击位置主导 / 扩散 | 主部位 ≫ 邻居，且邻居能进入撕裂 |
| 9–10 | 同种子 / 不同种子 | 同种子逐位一致；不同种子破洞位置不同（可复现的随机） |
| 11–12 | 30fps vs 60fps | 积分按真实时间解析求值，阶段与数值一致 |
| 13–17 | 3000 次满能量连击 | 覆盖率仍 ≥80%、面积上限守住、无脱落级、帧有效、指令不溢出 |
| 18–20 | 专用毛边/遮罩 | 叠加绑定，基础网格一个不少 |
| 21 | 渲染操作白名单 | 非白名单操作被 `Emit` 静默丢弃 |
| 22 | 未知服装 MOD | 通用绑定照常出帧，`fallbackUsed=true` |
| 23 | 词汇表 | 帧指令里不存在 hide/detach/unload/alpha=0 |
| 24 | 未磨损就撕裂 | 不允许（除锐器应力集中） |
| 25 | 无裂口就破洞 | 不允许（`tear ≥ 0.32` 前置） |
| 26 | 每帧内衬 | lining 数 == cutout 数 |
| 27–28 | 每帧覆盖率 / 指令数 | ≥0.80 / ≤512 |

另有两个**长尾**场景单列验证：12 秒完全停手（强度归零、损伤不回退）、
换装到未注册变体（回到 S0 且绑定成功）。

---

## 10. 调参

全部在 `config/battlewear.config.json`，改完不用重编译（`LoadJson` 逐键覆盖，缺键保留默认）：

| 想要的效果 | 改哪个 |
|---|---|
| 整体更快/更慢进入下一级 | `wearGain` / `tearGain` / `holeGain` 同比缩放 |
| 连击更「危险」 | 提高 `fatigueGain`、`intensityTauSeconds` |
| 破洞更少更克制 | 降低 `maxHolesPerPanel`、`maxTotalLossRatio` |
| 撕裂更连贯（成片） | 提高 `adjacencyWeight`、降低 `holeSiteCoherenceGain` |
| 减少贴花数量 | 贴花触发门槛目前是编译期常量（`dirt>0.01`、`wear>0.30`、`fray>0.01`、`holeCutout>0`），需要改 `visual_mapper.cpp`；如果常调，可以把它们提进 config |
| 更明显的重度表现 | 提高 `heavyGrade.grade3_intensity` 阈值以下的分档密度 |

---

## 11. 交付状态（诚实说明）

| 项 | 状态 |
|---|---|
| 逻辑层、表现层、绑定层、不变式层 | 已实现（C++17，约 1100 行 + 552 行测试） |
| 全流程与 28 项边界验证 | **已实际运行通过**（Python 参考实现，24464 检查 0 失败） |
| C++ 静态审计 | **已通过**（8/8，含反向注入测试） |
| C++ 编译 / `bw_tests.exe` | **未执行**——本机无 MSVC（`cl.exe` 不存在、`vcvars` 调 `reg.exe` 被策略拦、pip 装便携 Zig 拉不下来）。代码结构已审计，但**首次编译可能需要修少量编译器层面的问题**（例如 `/W4` 下的窄化警告） |
| 游戏内实测 | 未做（需要 `NSUNSC.exe` 运行时 + JAPI 加载 + 一次真实对局） |

下一步最有价值的一步是把 `bw_tests.exe` 在有 MSVC 的机器上编出来跑一遍：它和
`verify_flow.py` 是同一套断言，两边都过就说明参考实现与生产实现没有分叉。
