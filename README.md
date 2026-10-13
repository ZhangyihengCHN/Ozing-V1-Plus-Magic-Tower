<div align="center">

<h1>魔塔 50</h1>
<p><strong>好记星 V1+ 学习机原版 · 竖屏 Web 版</strong></p>

<p>
  <a href="https://mt.zzzyyyhhh.cn/"><strong>▶ 在线游玩</strong></a>
  · <a href="docs/DEVELOPMENT.md">开发与部署</a>
  · <a href="LICENSE">MIT 开源</a>
</p>

</div>

---

将 **好记星 V1+ 学习机内置的原版 50 层魔塔**带到浏览器。通过 WebAssembly 运行原版 ARM 游戏程序，保留原版地图、剧情、战斗、商店与道具规则。

网页使用原生 HTML、CSS 和 JavaScript，无前端框架。手机竖屏游玩，电脑支持鼠标与键盘。

## 特色

| 原版体验 | 网页适配 |
| --- | --- |
| 原版画面、剧情与攻击动画 | 手机竖屏、电脑左右布局 |
| 原版商店、手册、记事本与道具 | 四向摇杆与十格快捷道具栏 |
| 原版属性与道具消耗规则 | 三个手动存档槽与自动进度 |
| 保留原版游戏流程 | 离线运行、加密存档导入与导出 |

## 操作

**手机：** 使用触屏、摇杆和确认 / 菜单按钮。

**电脑：** 方向键 / WASD 移动，Enter / 空格 / Z 确认，Esc / X 返回；B 打开手册，N 打开记事本，1–9 / 0 使用快捷道具。更多快捷键见 [操作说明](docs/DEVELOPMENT.md#操作)。

存档保存在当前浏览器中；清理网站数据或更换地址前，请先导出备份。

## 本地运行

源码仓库不包含原厂固件、贴图和字库，需要准备自己的本地资源。环境要求：Node.js 22+、Python 3.10+、支持 wasm32 的 Clang / wasm-ld。

```sh
npm ci
python prepare-assets.py --firmware-dir /path/to/your/firmware
python build.py
npm start
```

打开 `http://localhost:8765`。构建后也可直接打开 `index.html`，或打包部署到 Cloudflare Pages / Docker，详见 [开发与部署](docs/DEVELOPMENT.md)。

## 文档与许可

[资源准备](assets/README.md) · [参与开发](CONTRIBUTING.md) · [安全说明](SECURITY.md) · [第三方许可](THIRD_PARTY_NOTICES.md)

自行编写的源码采用 [MIT](LICENSE)。原厂游戏程序与资源不包含在该许可中；离线存档加密用于增加篡改难度，不提供服务器级反作弊。
