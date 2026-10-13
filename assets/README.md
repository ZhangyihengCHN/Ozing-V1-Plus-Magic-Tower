# 本地资源

本目录只跟踪这份说明。原厂固件和资源不随源码分发。

可从本地原始 `V1PU.bin` / `V1PRU.bin` 提取：

```sh
python prepare-assets.py --firmware-dir /path/to/your/firmware
```

也可以导入已提取的文件：

```sh
python prepare-assets.py --from-dir /path/to/your/assets
```

需要 `fw.bin`、`bmp00.bin` 至 `bmp13.bin`、`map0280.bin`、`dlg0281.bin`、`font96.bin`、`font100.bin`、`font_ascii12.bin`、`font_cjk12.bin`。`extraction.json` 给出本版支持的长度和 SHA-256；脚本先校验全部内容，再写入本目录，不修改输入文件。

提取使用 V1PU 的资源表和 V1PRU 的原始资源边界。RID 0x176 内的 14 个位图按原表偏移和 BMP 长度提取；字库分别为 RID 0x100、0x101、0x106、0x107，地图和原文分别为 RID 0x280、0x281。不同固件版本若不匹配会明确报错，不生成混合资源。
