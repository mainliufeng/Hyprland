# cornice 桌面工具与只读观察接口

实现分支 `codex/cornice-agent-desktop`，cornice 对接分支 `codex/agent-desktop`。
本轮仅在私有嵌套实例测通，不合 main、不发版、不替换现有桌面。
647/647 单元与多 seat/单 seat 集成回归通过；完整 cornice 安装后对接证据在
`/tmp/ad-fpovr9l2`，连续观察实际绘制 14.94 fps，像素时钟到观察结果最大 138 ms。

## 协议

下列调用均指向显式选择的 fork 实例，结果使用 `hyprctl -j` 读取。
已有 `seat create/list/workspace/focus/capture/remove` 保留兼容。

| 命令 | 结果或行为 |
| --- | --- |
| `seat capabilities` | `protocol:1`、Lua 方言与逐项特性位 |
| `seat state NAME` | 生命周期、控制代次、暂停/可用状态、真实 ws、焦点、光标及输出几何 |
| `seat windows NAME` | 该 seat 当前 ws 的窗口，含稳定 `id`；标题/裸地址不是身份 |
| `seat control NAME LIFE GENERATION pause` | 递增代次，释放 held keys/buttons/modifiers、抓取、IME/DnD 状态，阻断写入 |
| `seat control NAME LIFE GENERATION resume` | 仅可用场景允许恢复，递增代次；旧设备和旧连接不重新获权 |
| `seat act NAME LIFE GENERATION workspace WS` | 校验身份、代次与写入状态后，只切指定 seat 的实际 ws |
| `seat act NAME LIFE GENERATION focus WINDOW_ID` | 只聚焦指定 seat 当前 ws 中的有效窗口身份 |
| `seat snapshot NAME LIFE current|EXISTING_WS png|argb ABS_PATH` | 同一个 compositor 请求内渲染并返回对应帧元数据 |

`LIFE` 取 `seatId`，不是显示名称；`GENERATION` 取最新 `generation`。
窗口身份由弱引用与递增序号维护，销毁及地址复用不会继续使用旧身份。
`seatId` 绑定本次 seat socket，调用方还须固定 compositor 实例；重建同名 seat 或
合成器重启后不能复用凭证。错误调用返回错误文本，不返回假的成功 JSON。

结构化状态/截图包含 `seatId`、`generation`、`paused`、`available`、`output`、
`display`、`workspace`、`viewWorkspace`、`windowId`、`window`、`cursor`、
`cursorVisible`、`position`、`logicalSize`、`pixelSize`、`scale`、`transform`。
截图另有 `frameId`、单调时钟 `timestampNs` 和 `format`；图像写入完成后才确认。

`workspace` 是 agent 的真实 ws，`viewWorkspace` 是这次读取的 ws。读取其他已有 ws
不会创建/激活 ws，也不改变任何 seat 的 focus/cursor。该画面不绘制另一个 ws 的
agent 光标或输入法/DnD 弹窗。窗口与普通 popup 仍属于实际工作区场景。

ARGB 路径必须是已经创建的绝对路径、同 UID、权限无 group/other 位的普通文件；
拒绝符号链接。cornice 为每个持续观察连接维护一个私有原始缓冲，复制完一帧之后才
请求下一帧。持续观察不使用 PNG 压缩/解压；按需截图才编码 PNG。
只读请求还为隐藏 ws 提供必要 frame/FIFO 回调，因此没有 seat 选中它时动画也能推进。

## 输入生命周期

cornice 创建后显式暂停、确认新帧再 resume，随后通过对应 socket 创建长期存在的
虚拟 keyboard/pointer。资源和源连接记录授予时的代次。暂停或锁屏后：

- 原设备的迟到事件被忽略；旧源连接不能重新创建虚拟设备来绕过撤销。
- 控制 API 的旧代次被拒绝；恢复需要新连接、新设备、新凭证和新截图。
- 锁屏发出 `sessionlock>>locked`，阻断导出；cornice 清除画面及原始缓冲。
- 已受管理的 seat 失去最后一个当前代次的键盘或指针时自动暂停，包括服务 SIGKILL。
- 解锁不会自动恢复输入。`seatcontrol>>NAME,GENERATION,paused|active` 提供控制变化事件。

这约束受管理的桌面输入，不能隔离同 UID 对特权 IPC、文件、终端与网络的访问。
现有未进入 managed control 的低层 seat 用法保持兼容。

## 客户端兼容

agent socket 首先公布它自己的 seat；客户端绑定后，再向 registry 公布其他共享 seats。
这是对只绑定第一个 seat 的应用的兼容安排，不建立窗口归属，不隐藏共享输出/窗口。
GTK 3 仍能绑定全部 seats 并操作同一个共享窗口；启动时需预先创建全部 seats。

实际 Google Chrome 的后台点击与中文输入已测试。它的
[WaylandSeat 实现](https://chromium.googlesource.com/chromium/src/+/05dd7b00a7a392a3aa1897f889755e70f481ea35/ui/ozone/platform/wayland/host/wayland_seat.cc)
只保留首个绑定的 seat。因此其他 seat 不能保证向该 Chrome 窗口输入；观察它的画面
不受此限制，今后的接管必须路由进原 seat。

## 验证与未交付部分

使用 cornice 的 `test/agent-desktop-verify.sh` 测试真实工具、应用及安装后观察器；
本库 `MULTISEAT_REGRESSION=1 ./hyprtester/multiseat/run.sh` 覆盖共享窗口、并行输入、
IME、clipboard、popup、拖放、约束、锁屏、退役和原有单 seat 回归。

此分支未提供物理输入接管路由，也未集成具体模型执行器或部署正式桌面会话。
没有对应能力位；cornice 不开放尚不存在的接管按钮或任务状态。
