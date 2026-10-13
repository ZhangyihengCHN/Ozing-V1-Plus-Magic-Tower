# Ozing V1+ Magic Tower · 魔塔 50 竖屏 Web 版

**在线游玩：[https://mt.zzzyyyhhh.cn/](https://mt.zzzyyyhhh.cn/)**

原生 HTML / CSS / JavaScript + WebAssembly，无前端框架、CDN 或游戏后端。Web 界面运行原版 ARM 程序，保留魔塔的地图、战斗、商店、剧情和内部窗口；手机采用竖屏布局，电脑采用游戏与操作区并排布局。

**这是源码仓库。原厂固件、贴图、字库和包含它们的运行包不随仓库分发，需要本地准备资源后构建。** 自行编写的源码采用 [MIT](LICENSE)，第三方内容见 [许可说明](THIRD_PARTY_NOTICES.md)。

## 功能

- 手机画面、快捷道具栏与四向摇杆随可见高度适配；长按移动，滑动切换方向，松开或失焦停止。
- 原版装备、商店、怪物手册、记事本、攻击动画与上下楼道具；保留原版条件和消耗次数。
- 三个手动存档槽及独立自动进度，展示原版图标、生命、攻击、防御、金币、楼层、钥匙和时间。
- 存档选择窗口位于游戏区域，可用触屏、摇杆或键盘选择；加密 JSON 导入与导出。
- 十格快捷道具栏；电脑显示快捷键，手机只显示道具图标和剩余次数。
- 加载动画展示下载、解压、启动和读档过程；资源内嵌并压缩，支持本地文件打开。

## 构建与运行

需要 Node.js 22+、Python 3.10+，以及支持 wasm32 且包含 `wasm-ld` 的 LLVM Clang。Node / npm 只用于开发构建，游戏运行不需要安装依赖。

```sh
git clone https://github.com/ZhangyihengCHN/Ozing-V1-Plus-Magic-Tower.git
cd Ozing-V1-Plus-Magic-Tower
npm ci
python prepare-assets.py --firmware-dir /path/to/your/firmware
python build.py
npm start
```

`--firmware-dir` 目录内应有本地 `V1PU.bin` 和 `V1PRU.bin`。提取前后校验本版支持的 SHA-256，不下载固件，不修改输入文件。也支持 `python prepare-assets.py --from-dir /path/to/your/extracted-assets`，详见 [资源准备](assets/README.md)。

Clang 不在 PATH 时：

```sh
python build.py --clang /path/to/clang
```

也可通过 `CLANG` 环境变量指定。Windows 参数可以是 `clang.exe` 的完整路径；Android NDK 内的 Clang 也可使用。若链接时报找不到 `wasm-ld`，请安装 LLVM 的 WebAssembly 链接器并放在 Clang 同目录或 PATH。

构建后访问 `http://localhost:8765`；同一局域网手机使用电脑的局域网地址。也可以直接打开构建后的 `index.html`，无需联网。`npm start` 只公开四个运行文件，不提供源码、密钥或存档目录。

仅修改网页逻辑或样式时，已构建过资源即可运行 `npm run build:web`。修改 `native/` 后需重新运行 `python build.py`。

## 部署

构建结果的运行文件只有：`index.html`、`style.css`、`game.js`、`game-data.js`。可放在普通静态服务器；相对资源路径也支持子目录。

```sh
python package.py
```

将 `dist/magic-tower-cloudflare-pages.zip` 上传到 [Cloudflare Pages Direct Upload](https://developers.cloudflare.com/pages/get-started/direct-upload/)。ZIP 根目录就是网站文件，无需在线构建。请上传这个包，不要把整个源码目录当静态站点公开。资源 URL 带内容哈希，更新包时缓存可重新校验。

Docker：

```sh
docker compose up -d --build
```

访问 `http://localhost:8080` 或 `/magic-tower/`；可设置 `MAGIC_TOWER_PORT` 改端口。容器使用非 root 用户、只读文件系统和临时目录，提供 gzip 与 ETag。`python package.py --docker` 可生成独立 Docker 构建包。

构建包包含本地原厂资源的衍生产物；其分发权限不由本仓库 MIT 许可授予。

## 操作

| 按键 | 操作 |
| --- | --- |
| 方向键 / WASD | 移动；菜单、原版窗口和存档选项导航 |
| Enter / 空格 / Z | 确认、继续对话 |
| Esc / X | 菜单、返回 |
| B | 怪物手册开关，需获得道具 |
| N | 记事本，需获得道具 |
| F5 / F9 | 保存 / 读取；刷新使用浏览器刷新按钮 |
| PageUp / PageDown | 楼层飞行器上一层 / 下一层，受原版条件限制 |
| 1–9 / 0 | 使用十格快捷栏对应道具，支持小键盘 |

手机使用屏幕按钮和摇杆；游戏内部窗口仍可直接点击。监狱醒来不赠送测试道具、钥匙或金币；对称飞行按原版最多 3 次。

## 存档与加密

存档保存在浏览器本地，不上传服务器。清理网站数据、换浏览器或换域名会改变可见存档；迁移前从菜单导出加密 JSON，再在目标网页导入。

自动存档、三个手动槽和导出文件使用 XChaCha20-Poly1305，数据与时间一同加密；篡改密文、nonce 或认证码会拒绝加载。原版二进制数据仍校验楼层、位置与属性范围。旧浏览器本地明文存档会一次性迁移；直接导入旧 JSON / BIN 已关闭。

首次运行 `npm run build:web` 自动生成本地 `save-key.json`，后续更新必须保留它，删除后重新生成会让旧加密存档无法解密。密钥不进入 Git，也不单独放进部署包；离线客户端中必须包含它。独立构建默认使用不同密钥，因此现有在线版的加密存档不能直接在独立构建中互通；只有你自己维护的、使用相同密钥的部署之间才可互通。

**离线加密增加篡改难度，不提供服务器级反作弊。** 熟悉客户端实现的人仍可修改运行数据或重新加密，本项目不把客户端数据作为可信排行榜依据。参见 [安全说明](SECURITY.md)。

## 源码结构

| 文件 / 目录 | 用途 |
| --- | --- |
| `index.html` / `style.css` | 自适应外层界面 |
| `game-source.js` | 输入、帧循环、加载、菜单、快捷道具与存档 |
| `save-crypto.mjs` / `build-web.mjs` | 存档认证加密与 Web 打包 |
| `native/` | ARM 模拟核心、原版应用配置和 Web 桥 |
| `prepare-assets.py` / `extraction.json` | 本地资源提取及校验 |
| `build.py` | Clang 编译并内嵌压缩 WASM |
| `package.py` / `Dockerfile` | 静态与容器部署包 |
| `tests/` / `tools/test.mjs` | 集中存放回归测试及运行入口 |

`game.js`、`game-data.js`、`game.wasm`、`save-crypto.bundle.js` 是生成文件。源码直接维护已适配的 `native/`，不依赖另一个模拟器仓库，也不需要从父目录复制补丁。

## 验证

```sh
npm run check:source
npm run build:web
npm run test:crypto
# 本地资源与完整 WASM 已构建后：
npm test
```

完整回归按依赖顺序运行 18 套测试，涵盖新游戏、监狱剧情、地下室、道具限制、飞行、商店、手册、记事本、攻击显示、长按输入、三槽存档、认证加密、旧存档迁移与失败回退、加载解压及静态服务器范围。测试生成的快照与报告只写入被忽略的 `verification/`，不使用个人存档，也不提交这些资源派生文件。

测试脚本统一放在 `tests/`，运行命令仍为 `npm test` 和 `npm run test:crypto`。单独检查可使用 `node tests/test-core.cjs`；依赖其他测试快照的用例请通过完整测试入口运行。

`docs/github-actions.yml` 是可选 GitHub Actions 模板：只检查源码公开范围、Web 构建、认证加密与 Python 语法，不获取原厂资源。模板未自动启用；需要时可在 GitHub 网页将其内容保存为 `.github/workflows/source-checks.yml`，或使用具备工作流写入权限的凭据提交。未宣称已手工通关全部 50 层。
