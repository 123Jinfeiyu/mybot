# bot4cj

基于仓颉和 Linux I2C 接口的硬件机器人库

## 项目介绍

`bot4cj` 是一个专为硬件机器人设计的库，基于仓颉编程语言和 Linux I2C 接口，提供了便捷的硬件控制能力，支持多种传感器和执行器。

## 功能特性

- 提供 I2C 总线通信接口
- 支持 PCA9685 舵机驱动芯片控制
- 支持 MPU6050 六轴传感器数据获取
- 简洁易用的 API 接口
- 基于仓颉语言的现代化编程体验
- 内置 HTTP 服务器示例，便于远程控制

## 项目结构

```
bot4cj/
├── src/
│   ├── I2C.c           # I2C 接口实现
│   ├── PCA9685.c       # PCA9685 舵机驱动实现
│   ├── MPU6050.c       # MPU6050 六轴传感器实现
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
    cjpm run
    ```
   
## API 参考

### I2C 接口

- `I2C_init(path: CString): Int16` - 初始化 I2C 设备
- `I2C_free(fd: Int16): Unit` - 释放 I2C 设备资源
- `I2C_setAddr(fd: Int16, addr: UInt8): Unit` - 设置 I2C 设备地址
- `I2C_writeReg(fd: Int16, reg: UInt8, val: UInt8): Unit` - 向寄存器写入数据
- `I2C_readReg(fd: Int16, reg: UInt8): UInt8` - 从寄存器读取数据

### PCA9685 接口

- `PCA9685_init(fd: Int16, addr: UInt8): Unit` - 初始化 PCA9685 芯片
- `PCA9685_setPWM(fd: Int16, channel: UInt8, on: UInt16, off: UInt16): Unit` - 设置 PWM 信号
- `PCA9685_setAngle(fd: Int16, channel: UInt8, angle: Float32): Unit` - 设置舵机角度

### MPU6050 接口

- 支持获取加速度计和陀螺仪数据

## 许可证

本项目采用 MIT 许可证，详见 [LICENSE](LICENSE) 文件。

## 贡献

欢迎提交 Issue 和 Pull Request 来改进这个项目。

## 联系方式

如有问题或建议，请通过 GitHub Issues 与我们联系。
