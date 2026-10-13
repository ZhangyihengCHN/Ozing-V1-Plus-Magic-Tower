# 参与开发

请先按 [开发与部署](docs/DEVELOPMENT.md) 准备本地资源并构建。修改 Web 逻辑编辑 `game-source.js`，修改加密编辑 `save-crypto.mjs`，不要直接编辑生成的 `game.js`。原版行为相关修改应保留真实 ARM 程序逻辑，并加入能覆盖问题的回归测试。

提交前运行 `npm run check:source`；涉及游戏逻辑时先重新构建并运行 `npm test`。测试脚本位于 `tests/`，`tools/test.mjs` 已按测试快照生成顺序排列，运行命令保持不变。

不要提交原厂资源、生成的运行文件、存档、`save-key.json`、服务器部署凭据或个人路径。新增公开文件时更新 `tools/check-source.mjs` 的允许范围。请勿在 Issue 中上传自己的存档或密钥；报告问题时提供浏览器版本、操作步骤和不含个人内容的截图。
