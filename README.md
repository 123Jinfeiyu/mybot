# bot4cj

基于仓颉和 Linux I2C 接口的硬件机器人库

## 项目介绍

`bot4cj` 是一个专为硬件机器人设计的库，基于仓颉编程语言和 Linux I2C 接口，提供了便捷的硬件控制能力。

## 功能特性

- 提供 I2C 总线通信接口
- 支持 PCA9685 舵机驱动芯片控制
- 简洁易用的 API 接口
- 基于仓颉语言的现代化编程体验

## 项目结构

```
bot4cj/
├── src/
│   ├── I2C.c           # I2C 接口实现
│   ├── PCA9685.c       # PCA9685 舵机驱动实现
│   ├── bot4cj.h        # 库头文件
│   └── main.cj         # 示例主文件
├── .gitignore
├── LICENSE
├── Makefile
├── README.md
└── cjpm.toml          # 仓颉包管理文件
```

## 安装方法

### 前置依赖

- Linux 系统
- I2C 设备支持
- 仓颉编译器
- GCC 编译器

### 构建步骤

1. 克隆项目到本地

2. 进入项目目录
   ```bash
   cd bot4cj
   ```

3. 编译C代码
   ```bash
   make
   ```

4. 编译cj示例代码
    ```bash
    cjc -L . -l bot src/main.cj
    ```
   
## 许可证

本项目采用 MIT 许可证，详见 [LICENSE](LICENSE) 文件。

## 贡献

欢迎提交 Issue 和 Pull Request 来改进这个项目。

## 联系方式

如有问题或建议，请通过 GitHub Issues 与我们联系。
