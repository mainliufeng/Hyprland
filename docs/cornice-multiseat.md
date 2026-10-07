# Cornice: 多 seat、共享窗口、独立工作区视图

本 fork 在同一个 Hyprland 进程里支持多个输入 seat。人使用默认的 `Hyprland`
seat，agent 使用命名 seat；seat 数量没有写死为两个。

每个 agent 有自己的鼠标、键盘焦点、按键/修饰键状态、输入法 relay、剪贴板、
抓取/拖放状态和当前工作区。所有 seat 共享窗口与工作区集合，可以选择同一个 ws，
也可以分别选择 ws1、ws10、ws11。一个输出可供多个 seat 使用。

物理输出仍显示人的当前工作区。`seat workspace` 只改变指定 agent 的视图，
不会调用物理输出的 `changeWorkspace`。隐藏工作区接收真实 Wayland 输入，
有 30 Hz 的 frame callback/FIFO 更新调度，按需渲染为截图；没有创建私有虚拟输出。

特性分支 `codex/cornice-agent-desktop` 已增加结构化截图、只读帧导出与受管理输入暂停。
接口与边界见 [cornice 桌面对接](cornice-desktop-api.md)。

Fork: <https://github.com/mainliufeng/Hyprland>。
上游基础：`5a78b5e927345860a27e2893bf894f97ee620c48`。

## 使用

这些命令必须发给**本 fork 的实例**；安装的上游 Hyprland 没有这些接口。
首次运行请使用独立开发会话。仓库测试会创建私有嵌套会话，不替换正在运行的桌面。
`HYPRLAND_INSTANCE_SIGNATURE` 和 `XDG_RUNTIME_DIR` 应指向目标实例。

先创建需要的 seats，再启动要共享操作的应用：

```sh
output=$(hyprctl -j monitors | jq -r '.[] | select(.focused) | .name')
hyprctl seat create agent1 "$output"
hyprctl seat create agent2 "$output"
hyprctl seat create agent3 "$output"
hyprctl seat workspace agent1 10
hyprctl seat workspace agent2 11
hyprctl seat workspace agent3 name:research
hyprctl -j seat list
```

`seat list` 列出额外 seats，返回 `name`、`output`、`display`、`cursor`、
`workspace` 和焦点窗口标题。默认 seat 不在这个额外 seats 列表里；它的状态仍通过
`activeworkspace`、`activewindow`、`cursorpos` 查询。

通过指定 seat 的额外 Wayland socket 启动应用：

```sh
agent_display=$(hyprctl -j seat list | jq -r '.[] | select(.name=="agent1") | .display')
env -u WAYLAND_SOCKET -u DISPLAY WAYLAND_DISPLAY="$agent_display" \
    dbus-run-session -- kitty -o linux_display_server=wayland
```

socket 决定新窗口的默认工作区、缺省虚拟指针的 seat，以及无显式 seat 的旧协议默认值。
**socket 不决定窗口所有权**：所有连接均能看到所有活跃 `wl_seat` 和输出；
人启动的窗口可以接收 agent 的输入，agent 启动的窗口也可以被人操作。
浏览器等应用应使用独立 profile，防止进程/DBus 单例把窗口交给旧进程。

截图与焦点操作：

```sh
# agent 仍在后台 ws10；人的屏幕和鼠标不动。
hyprctl seat capture agent1 /tmp/agent1.png
hyprctl seat capture Hyprland /tmp/human.png

# 只允许聚焦该 seat 当前 ws 中可接受输入的窗口。
hyprctl seat focus agent1 'title:^Agent terminal$'
hyprctl seat focus agent1 'address:0xWINDOW_ADDRESS'

# 同一工作区和同一窗口可以被两个 seat 选择。
hyprctl seat workspace agent1 1
hyprctl seat focus agent1 'title:^Shared editor$'

hyprctl seat remove agent1
```

截图保存 PNG，尺寸来自工作区所在输出，坐标与输入使用的工作区布局一致。
截图包含该 ws 的窗口、弹窗、该 seat 的鼠标和输入法弹窗；输出级 shell 图层
（bar、壁纸、launcher）仍由人的 shell 管理，不放入 agent 的窗口视图。
`grim -o OUTPUT` 捕获的是物理输出当前显示内容，不能代替 agent 的 `seat capture`。
截图路径必须绝对；可以含空格。锁屏时输入、切换、聚焦和截图均被拒绝。

## 输入控制

控制程序通过 Wayland 虚拟输入协议注入事件，**显式按名称选择 wl_seat**：

- `zwp_virtual_keyboard_manager_v1.create_virtual_keyboard(seat)`；
- `zwlr_virtual_pointer_manager_v1.create_virtual_pointer_with_output(seat, output)`。

可直接参考 `hyprtester/multiseat/input.c`。测试脚本会编译一个可运行的协议控制程序，
其标准输入支持 `motion X Y`、`relative DX DY`、`button CODE STATE`、
`key CODE STATE`、`mods MASK`、`type TEXT`；`type` 示例只处理 ASCII，真实键盘协议不限于 ASCII。
实际应用中的 IME/text-input 能力取决于客户端是否为对应 seat 创建协议对象。

虚拟指针的 `seat=NULL` 使用连接所属 seat，主连接则使用默认 seat。
`uinput`/`ydotool` 沿物理设备路径进入人的 seat，不能用于后台 agent。
现有全局快捷键与 `hyprctl dispatch` 仍操作人的桌面；agent 应使用上述 seat API。

剪贴板工具也要显式选择 seat：

```sh
env WAYLAND_DISPLAY="$agent_display" wl-copy --seat agent1 'agent clipboard'
env WAYLAND_DISPLAY="$agent_display" wl-paste --seat agent1
```

## 已知兼容边界

- 已验证真实原生 Wayland GTK 3 应用：后台输入、共享窗口、抓取、拖放和截图。
  其他工具包及复杂应用需要各自验证，不能从协议支持推断全部应用兼容。
- **GTK 3.24.52 的热新增 seat 有限制**：实测运行中的应用收到新 seat 公告，却未绑定它。
  [其 Wayland backend 源码](https://github.com/GNOME/gtk/blob/3.24.52/gdk/wayland/gdkdisplay-wayland.c)
  把 seat 初始化排入 closure，而 closure 处理在显示连接初始化路径运行。
  因此先创建 seats，再启动要共同操作的 GTK 3 应用；添加新 seat 后需要重启相关应用。
  这不影响现有 seat 切 ws，也不是工作区或窗口独占限制。
- GTK 3 的 Wayland 输入法模块
  [选择 display 的默认 seat](https://github.com/GNOME/gtk/blob/3.24.52/modules/input/imwayland.c)，
  不能据此保证同一 GTK 应用为每个 seat 提供独立 IME context。
  合成器的 relay、IME 键盘抓取及 text-input-v3 路由按显式 seat 分离；客户端也必须支持。
- GTK 客户端发起窗口移动/缩放时，也应使用事件所属 device/seat 的 API，
  例如 `gdk_window_begin_move_drag_for_device`；默认 device 的便捷 API 不能代表另一个 seat。
- 额外 seats 当前使用虚拟键盘/指针。物理设备重新分配、额外触摸/数位板、
  XWayland 多输入焦点、输出热拔插组合尚不作为本版本保证。
- 多 seat 不是安全沙箱。文件、进程、应用/文档数据、窗口布局和 ws 集合共享。
  同一个应用的文本插入位置、选区、拖动和文档修改仍可能相互影响。
  Prompt 可以要求 agent 留在 ws10，但本版本不设置工作区 ACL。
- `codex/cornice-agent-desktop` 的帧接口供 cornice 只读跟随/浏览；这些读取不切换任何
  seat 的实际 ws。人的物理输入接管尚未实现，暂停不等于接管。
- Chrome 的 Wayland backend 只使用一个 seat。agent socket 优先公布自己的 seat，
  绑定后再公布其他共享 seats；Chrome 可接受其目标 seat 的输入，但不能承诺另一
  seat 也能输入同一个 Chrome 窗口。GTK 3 的共享窗口多 seat 输入已实测。

## 生命周期

删除 seat 会关闭其监听 socket、移除 seat global、结束拖动/抓取、清理输入焦点并禁用输入。
**不会隐藏或关闭共享窗口**。旧协议资源、释放 wl_seat 后仍存在的子资源和旧连接保持有效；
控制器在它们释放后才能回收。再次创建同名 seat 会得到新 socket；旧输入不会迁移过去。

## 构建与验证

测试环境：Clang 22、Lua 5.5、aquamarine 0.15.1、hyprutils 0.14.2、系统 Python/GTK3。

```sh
git submodule update --init --recursive
cmake -S . -B build-multiseat -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS_DEBUG='-O0 -g0' \
  -DPython3_EXECUTABLE=/usr/bin/python3
cmake --build build-multiseat -j4
ulimit -c 0
seat_test_coverage=$(mktemp -d /tmp/cornice-unit-coverage.XXXXXX)
GCOV_PREFIX="$seat_test_coverage" ./build-multiseat/hyprland_gtests
make -B -C hyprtester/plugin CXX=clang++ LUA_INCLUDES=/usr/include/lua5.5
MULTISEAT_REGRESSION=1 ./hyprtester/multiseat/run.sh
```

集成套件需要 Mutter、dbus-daemon、PyGObject/GTK3、GTK 开发头文件、编译器、
wayland-scanner、Wayland 协议、xkbcommon、wl-clipboard、hyprctl。
套件只读取宿主设备名称；嵌套实例禁用这些物理设备，并使用独立 runtime、DBus 和
`AQ_DRM_DEVICES=/dev/null`。所有测试输入只发给私有实例。
它输出实际截图、应用/合成器日志和 `results.json`，见
[本版本验证记录](verification/cornice-shared-workspaces.md)。

上游贡献须遵守 [AI 使用政策](https://github.com/hyprwm/.github/blob/main/policies/AI_USAGE.md)
和 [issue 规范](https://wiki.hypr.land/contributing-and-debugging/issue-guidelines/)。
本次仅交付到用户自己的 fork，没有向上游提交 PR、issue 或 discussion。
