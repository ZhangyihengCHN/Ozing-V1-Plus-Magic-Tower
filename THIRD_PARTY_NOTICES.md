# 第三方内容

本仓库自行编写的 Web 界面、模拟核心、桥接代码、构建与测试工具采用 MIT，见 `LICENSE`。

好记星 V1+ 固件中的原版魔塔 ARM 程序、地图、贴图、字库和原文不属于上述许可。本仓库不提供这些二进制文件，也不提供内嵌它们的 `game.wasm` / `game-data.js`。构建时请在本地准备自己有权使用的资源；项目名称仅用于标识兼容的设备和游戏。

构建依赖：

| 组件 | 版本 | 许可 | 用途 |
| --- | --- | --- | --- |
| [@noble/ciphers](https://github.com/paulmillr/noble-ciphers) | 2.4.0 | MIT | 离线存档认证加密；许可证内嵌在生成的 game.js 中 |
| [esbuild](https://github.com/evanw/esbuild) | 0.28.2 | MIT | 开发时打包加密模块 |

两者仅在 `npm ci` / 构建时下载；游戏运行不访问 npm 或 CDN。Docker 使用官方 nginx 镜像；其第三方许可证由镜像分发方提供。LLVM / Clang 是本地构建工具，不随仓库分发。
