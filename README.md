# bot4cj

基于仓颉和 Linux I2C 接口的硬件机器人库

## 项目介绍

`bot4cj` 是一个专为硬件机器人设计的库，基于仓颉编程语言和 Linux I2C 接口，提供了便捷的硬件控制能力，支持多种传感器和执行器。

编译方法：
```bash
make
cjpm build
```

## 解析器

程序内置了动作脚本解析器，使用如下方法使用：
```bash
# 蜘蛛机器人脚本
cat ss/step.bot | cjpm run
```