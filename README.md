# HearthStone-Skipper

炉石传说酒馆战棋 macOS 一键拔线工具。战斗开始后断开当前对局连接，让客户端自动重连并跳过战斗动画。

> 本项目修改自 [z2z63/hearthstone_skipper](https://github.com/z2z63/hearthstone_skipper)。在原项目基础上增加了无需 Clash 的 macOS 原生 PF 后端、对局连接精确识别、结构化日志与重连稳定性修复。

## 功能

- `macOS 原生 PF（无需 Clash）`：不安装、不运行 Clash 也可以拔线。
- `Clash TCP/IP / UNIX 套接字`：保留原有 Clash/Mihomo 使用方式。
- 只选择炉石当前对局连接，避开 Battle.net 登录连接。
- 日志记录每次点击、连接选择、helper 结果和耗时，方便定位偶发问题。

## 下载与安装

1. 打开本仓库的 [Releases](https://github.com/an-stu/hearthstone_skipper/releases)，下载最新的 `macos-arm64.zip`。当前测试包仅支持 Apple Silicon（M1/M2/M3/M4 等）。
2. 完全退出旧版 Skipper。解压 ZIP，将 `skipper.app` 拖入 `/Applications`；若系统询问是否替换，选择替换。
3. 第一次打开时，如 macOS 阻止未公证应用，进入“系统设置 → 隐私与安全”，确认打开 Skipper。
4. Skipper 启动后会出现在系统菜单栏，并自动打开设置窗口。

升级时只保留 `/Applications/skipper.app` 即可，不要同时运行下载目录或构建目录中的副本。

## 教程一：完全原生模式（推荐，无需 Clash）

### 使用前设置

1. 退出 Clash，或至少关闭 Clash/VPN 的 TUN/增强模式。原生模式检测到 `198.18.0.0/15` 等合成 TUN 地址时会拒绝执行，避免误报成功。
2. 打开 Skipper 设置，在“拔线后端”选择 `macOS 原生 PF（无需 Clash）`。
3. 点击“检测”。显示成功表示安装包内的原生 helper 存在且可执行；这一步不会申请管理员权限。
4. 可选：启用“在游戏窗口右上角显示一键拔线按钮”。首次启用悬浮按钮时，按系统提示授予辅助功能权限。也可以不启用，直接使用菜单栏按钮。

### 实战拔线

1. 正常进入酒馆战棋并等待战斗开始。
2. 确认已经切入战斗画面后，点击悬浮的“一键拔线”，或点击菜单栏 Skipper 图标 → “一键拔线”。不要在招募阶段提前点击。
3. 本次 Skipper 启动后的第一次原生拔线会弹出 macOS 管理员授权，请输入管理员密码。授权启动的受限 helper 会复用到 Skipper 退出，因此同一次启动内后续拔线通常不再询问密码。
4. 游戏应短暂显示“正在重新连接”，随后回到当前对局。一次操作尚未完成时的重复点击会被忽略，不需要连续点击。

退出或重新启动 Skipper 后，下一次原生拔线需要重新授权。若 helper 意外退出，也可能再次弹出密码框。当前测试版不会安装永久常驻的 root 服务。

## 教程二：Clash/Mihomo 模式

1. 开启 Clash/Mihomo 的 TUN 模式，确保炉石流量由其核心接管。
2. 在 Clash 配置中启用 external controller，并添加：

   ```yaml
   find-process-mode: always
   ```

3. 在 Skipper 设置中选择 `TCP/IP` 或 `UNIX套接字`。
4. TCP/IP 模式填写 `external_controller` 和 `secret`；UNIX 模式选择控制器套接字文件。Skipper 会尝试自动推断常见配置，无法推断时再手动填写。
5. 点击“检测”，成功后按上面的实战步骤使用“一键拔线”。Clash 模式不需要管理员密码。

## 日志与问题反馈

测试时请记下异常发生的准确时间（例如 `09:45`），然后从菜单栏 Skipper 图标选择“打开日志”。日志文件位于：

```text
~/Library/Application Support/z2z63/skipper/log.txt
```

反馈问题时请提供：

- Skipper 版本、macOS 版本和 Mac 芯片；
- 使用原生模式还是 Clash 模式；
- 点击时间，以及当时处于招募、战斗还是观战；
- 游戏表现（无反应、一直重连、游戏退出等）；
- 对应时间前后约一分钟的日志。日志会轮转，不要等多次重启后再保存。

常见情况：

- **点击没有反应**：一次操作正在执行时重复点击会被忽略；打开日志查找 `duplicate click while busy`。
- **原生模式提示关闭 TUN**：关闭 Clash/VPN 的 TUN 后重新进入对局再试。
- **一直显示正在重新连接**：先等待游戏自身完成重连，不要连续拔线；保存日志和异常时间后再重启游戏。
- **每次都要求密码**：确认没有退出/重启 Skipper，并检查是否同时运行了多个 `skipper.app`。同一进程内 helper 正常存活时只需首次授权。
- **游戏直接退出**：立即保存 Skipper 日志和炉石日志，并注明退出时间；Skipper 不会主动结束游戏进程。

## 界面截图

<div style="display: flex; justify-content: space-around; flex-wrap: wrap">
  <img src="docs/img.png" alt="Skipper 设置" width="420">
  <img src="docs/img_1.png" alt="Clash 配置" width="420">
  <img src="docs/img_2.png" alt="Skipper 使用示例" width="420">
  <img src="docs/img_3.png" alt="菜单栏图标" width="160">
  <img src="docs/img_4.png" alt="悬浮按钮" width="420">
</div>

## 原理

### 拔线是什么

每句对战中，所有玩家每三分钟同时进入战斗，战斗结束后、直到下次对战的时间被称为回合内。然而每次战斗的过程和结果在进入战斗时已经由服务器确定，客户端只是播放动画。

拔线指通过特殊手段跳过战斗动画，提前结束战斗进入回合，获得更多操作时间

### 拔线的原理

在战斗时或战斗即将开始时，断开客户端与服务器的连接，使客户端自动尝试重连，重连完成后即可跳过战斗动画，提前进入回合

### 本项目的独特之处/契机

macOS 已移除 ALTQ，但 PF 的过滤与连接状态控制仍然存在。真正的限制是：macOS 应用防火墙没有提供按应用终止活跃出站连接的公开接口；PF 需要 root 权限、不按进程识别连接，而且仅加载新规则不会自动清除已有连接状态。若要安全集成，还需要签名的特权 helper 或 Network Extension。

然而，这些软件要么支持的功能不足以动态添加规则、终止一个活跃的网络连接，要么是商业付费专有的

本项目使用了一个独特的思路，通过external controller与 clash 核心通信，在 clash 核心内终止炉石传说客户端连接，因此不需要
root 权限，不修改网络配置，系统影响小

macOS 原生后端通过炉石日志与 `libproc` 双重确认真实对局连接，再由受限辅助程序在独立 PF anchor 中短暂加载精确四元组规则并清除对应状态；完成或失败后都会撤销规则。首次授权启动的辅助程序只接受严格校验的拔线指令，并在 Skipper 退出时结束，因此同一 Skipper 进程内不会因系统授权缓存超时而反复询问密码。它不依赖 Clash，但首次使用需要 macOS 管理员授权。详细设计和边界见 [非 Clash 原生拔线方案](docs/native-disconnect-design.md)。

同时，这个思路也适用于 windows 端的炉石传说客户端。虽然 windows 端已有广泛使用的 HDT炉石团子
插件，但需要管理员权限，而且由开源转为闭源，严重违反开源精神

## 要求与限制

1. 原生 PF 后端仅支持 macOS，需要管理员授权；当前发布包仅构建 Apple Silicon 版本。
2. 原生 PF 与 Clash/VPN TUN 不能同时使用。
3. Clash 后端要求兼容 Clash API 的核心能够接管炉石传说客户端流量。
4. 当前原生版本仍为实战测试版。正式分发仍需 Developer ID 签名、公证及基于 `SMAppService` 的 helper。

详细设计和安全边界见 [非 Clash 原生拔线方案](docs/native-disconnect-design.md)。
