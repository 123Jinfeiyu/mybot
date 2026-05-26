// test_geometry_cnn.cj - 几何图形分类测试（Adam 优化器版本）
package bot4cj
import bot4cj.math.*
import std.math.*
import std.random.*
import std.collection.*

/**
 * 计算圆形像素值
 */
func calcCirclePixel(x: Int64, y: Int64, center: Float64, radius: Float64, offsetX: Float64, offsetY: Float64): FP32 {
    let dx = Float64(x) - center + offsetX
    let dy = Float64(y) - center + offsetY
    let dist = sqrt(dx * dx + dy * dy)
    
    if (dist <= radius) {
        let intensity = 1.0 - (dist / radius) * 0.3
        FP32(Float32(intensity))
    } else if (dist <= radius + 1.5) {
        let edgeDist = dist - radius
        let intensity = max(0.0, 1.0 - edgeDist / 1.5) * 0.7
        FP32(Float32(intensity))
    } else {
        FP32(0.0)
    }
}

/**
 * 计算正方形像素值
 */
func calcSquarePixel(x: Int64, y: Int64, center: Float64, radius: Float64, offsetX: Float64, offsetY: Float64): FP32 {
    let halfSize = radius * 0.8
    let left = center - halfSize + offsetX
    let right = center + halfSize + offsetX
    let top = center - halfSize + offsetY
    let bottom = center + halfSize + offsetY
    
    let fx = Float64(x)
    let fy = Float64(y)
    
    if (fx >= left && fx <= right && fy >= top && fy <= bottom) {
        let distToEdge = min(
            min(fx - left, right - fx),
            min(fy - top, bottom - fy)
        )
        let intensity = min(1.0, 0.7 + distToEdge / halfSize * 0.3)
        FP32(Float32(intensity))
    } else {
        let distX = max(0.0, max(left - fx, fx - right))
        let distY = max(0.0, max(top - fy, fy - bottom))
        let edgeDist = sqrt(distX * distX + distY * distY)
        if (edgeDist <= 1.5) {
            let intensity = max(0.0, 1.0 - edgeDist / 1.5) * 0.6
            FP32(Float32(intensity))
        } else {
            FP32(0.0)
        }
    }
}

/**
 * 计算三角形像素值
 */
func calcTrianglePixel(x: Int64, y: Int64, center: Float64, radius: Float64, offsetX: Float64, offsetY: Float64): FP32 {
    let height = radius * 2.0
    let baseWidth = height * 1.2
    let topY = center - height * 0.5 + offsetY
    let bottomY = center + height * 0.5 + offsetY
    
    let fx = Float64(x)
    let fy = Float64(y)
    
    if (fy >= topY && fy <= bottomY) {
        let t = (fy - topY) / (bottomY - topY)
        let halfWidthAtY = baseWidth * (1.0 - t) * 0.5
        let centerX = center + offsetX
        
        if (fx >= centerX - halfWidthAtY && fx <= centerX + halfWidthAtY) {
            let distToEdge = min(
                fx - (centerX - halfWidthAtY),
                (centerX + halfWidthAtY) - fx
            )
            let intensity = min(1.0, 0.7 + distToEdge / halfWidthAtY * 0.3)
            FP32(Float32(intensity))
        } else {
            let distToLeft = (centerX - halfWidthAtY) - fx
            let distToRight = fx - (centerX + halfWidthAtY)
            let edgeDist = max(distToLeft, distToRight)
            if (edgeDist <= 1.5) {
                let intensity = max(0.0, 1.0 - edgeDist / 1.5) * 0.6
                FP32(Float32(intensity))
            } else {
                FP32(0.0)
            }
        }
    } else {
        FP32(0.0)
    }
}

/**
 * 生成几何图形数据
 * 图形类型：0=圆形, 1=正方形, 2=三角形
 */
func generateGeometryData(numSamples: Int64): (Array<Matrix1>, Array<Int64>) {
    var inputs = Array(numSamples, { _ => Matrix1(1, 1) })
    var labels = Array(numSamples, { _ => 0 })
    let rng = Random()
    
    let imgSize = 16
    
    for (i in 0..numSamples) {
        let shapeType = Int64(rng.nextFloat64() * 3.0)
        labels[i] = shapeType
        
        var matrix = Matrix1(imgSize * imgSize, 1)
        let center = Float64(imgSize) / 2.0
        let radius = Float64(imgSize) / 3.0 + rng.nextFloat64() * 2.0 - 1.0
        
        let offsetX = (rng.nextFloat64() - 0.5) * 2.0
        let offsetY = (rng.nextFloat64() - 0.5) * 2.0
        
        for (y in 0..imgSize) {
            for (x in 0..imgSize) {
                let idx = y * imgSize + x
                
                var pixelValue = match (shapeType) {
                    case 0 => calcCirclePixel(x, y, center, radius, offsetX, offsetY)
                    case 1 => calcSquarePixel(x, y, center, radius, offsetX, offsetY)
                    case _ => calcTrianglePixel(x, y, center, radius, offsetX, offsetY)
                }
                
                let noise = FP32(Float32(rng.nextFloat64() - 0.5) * 0.05)
                pixelValue = pixelValue + noise
                
                if (pixelValue.getValue() > 1.0) {
                    pixelValue = FP32(1.0)
                } else if (pixelValue.getValue() < 0.0) {
                    pixelValue = FP32(0.0)
                }
                
                matrix.set(idx, 0, pixelValue)
            }
        }
        
        inputs[i] = matrix
    }
    
    (inputs, labels)
}

/**
 * 可视化一个样本
 */
func showSample(matrix: Matrix1, imgSize: Int64, label: Int64): Unit {
    let shapeNames = ["圆形", "正方形", "三角形"]
    println("标签: ${shapeNames[label]}")
    
    for (y in 0..imgSize) {
        var line = ""
        for (x in 0..imgSize) {
            let idx = y * imgSize + x
            let val = matrix.get(idx, 0).getOrThrow().getValue()
            if (val > 0.8) {
                line = line + "██"
            } else if (val > 0.6) {
                line = line + "▓▓"
            } else if (val > 0.4) {
                line = line + "▒▒"
            } else if (val > 0.2) {
                line = line + "░░"
            } else {
                line = line + "  "
            }
        }
        println(line)
    }
    println("")
}

/**
 * Softmax 辅助函数
 */
func computeSoftmax(input: Matrix1): Matrix1 {
    let cols = input.get_col()
    var output = Matrix1(1, cols)
    
    var maxVal = Float32(-1.0e30)
    for (c in 0..cols) {
        let raw = input.get(0, c).getOrThrow().getValue()
        if (raw > maxVal) {
            maxVal = raw
        }
    }
    
    var expSum = Float32(0.0)
    for (c in 0..cols) {
        let raw = input.get(0, c).getOrThrow().getValue()
        let ev = Float32(exp(raw - maxVal))
        output.set(0, c, FP32(ev))
        expSum = expSum + ev
    }
    
    for (c in 0..cols) {
        let v = output.get(0, c).getOrThrow().getValue()
        output.set(0, c, FP32(v / expSum))
    }
    
    output
}

/**
 * 获取预测类别
 */
func predictClass(probs: Matrix1): Int64 {
    let cols = probs.get_col()
    var maxProb = Float32(-1.0)
    var predClass = 0
    
    for (c in 0..cols) {
        let p = probs.get(0, c).getOrThrow().getValue()
        if (p > maxProb) {
            maxProb = p
            predClass = Int64(c)
        }
    }
    
    predClass
}

/**
 * 打印分隔线
 */
func printLine(): Unit {
    println("============================================================")
}

/**
 * 主测试函数
 */
func testGeometryCNN(): Unit {
    printLine()
    println("CNN 几何图形分类测试 (Adam 优化器版本)")
    printLine()
    println("")
    
    let imgSize = 16
    let nTrainSamples = 200
    let nTestSamples = 60
    let nEpochs = 50
    let batchSize = 10
    let nClasses = 3
    
    println("配置信息:")
    println("  图像大小: ${imgSize}x${imgSize}")
    println("  训练样本数: ${nTrainSamples}")
    println("  测试样本数: ${nTestSamples}")
    println("  训练轮数: ${nEpochs}")
    println("  Batch 大小: ${batchSize}")
    println("  类别数: ${nClasses} (圆形, 正方形, 三角形)")
    println("  优化器: Adam")
    println("")
    
    println("网络结构:")
    println("  Input(16x16x1)")
    println("  -> Conv1(5x5, 1->8) -> Pool1(2x2)")
    println("  -> Conv2(3x3, 8->16) -> Pool2(2x2)")
    println("  -> Flatten -> FC(64->32) -> FC(32->3)")
    println("  -> Softmax")
    println("")
    
    let lr = FP32(0.01)
    
    // 构建网络
    var conv1 = ConvLayer(1, 8, (5, 5), 1, 0, lr, Af.Leaky_ReLU(FP32(0.01)), Option.None)
    var pool1 = PoolingLayer(8, (2, 2), 2, 0, "max", Af.ReLU, lr, lr)
    var conv2 = ConvLayer(8, 16, (3, 3), 1, 0, lr, Af.Leaky_ReLU(FP32(0.01)), Option.None)
    var pool2 = PoolingLayer(16, (2, 2), 2, 0, "max", Af.ReLU, lr, lr)
    var flatten = FlattenLayer(16, 2, 2)
    var fc1 = Nn1(64, 32, Float32(0.3), Af.ReLU)
    var fc2 = Nn1(32, 3, Float32(0.3), Af.Softmax)
    
    // 创建 Adam 优化器
    var adamConv1 = Adam(FP32(0.001))    // 卷积层1
    var adamConv2 = Adam(FP32(0.001))    // 卷积层2
    var adamFC1 = Adam(FP32(0.0005))     // 全连接层1
    var adamFC2 = Adam(FP32(0.0005))     // 全连接层2
    
    println("网络创建完成 (Adam优化器: Conv1, Conv2, FC1, FC2)")
    println("")
    
    // 生成数据
    println("生成训练数据...")
    let (trainInputs, trainLabels) = generateGeometryData(nTrainSamples)
    println("生成测试数据...")
    let (testInputs, testLabels) = generateGeometryData(nTestSamples)
    println("数据生成完成")
    println("")
    
    // 显示几个样本
    println("样本预览:")
    for (i in 0..3) {
        if (i < nTrainSamples) {
            println("样本 ${i + 1}:")
            showSample(trainInputs[i], imgSize, trainLabels[i])
        }
    }
    
    println("开始训练...")
    println("")
    
    var bestAccuracy = Float32(0.0)
    
    // 计算卷积输出尺寸
    let conv1OutH = imgSize - 4  // 16 - 5 + 1 = 12
    let conv1OutW = imgSize - 4
    let pool1OutH = conv1OutH / 2  // 6
    let pool1OutW = conv1OutW / 2
    let conv2OutH = pool1OutH - 2  // 6 - 3 + 1 = 4
    let conv2OutW = pool1OutW - 2
    let pool2OutH = conv2OutH / 2  // 2
    let pool2OutW = conv2OutW / 2
    let flatSize = 16 * pool2OutH * pool2OutW  // 64
    
    for (epoch in 0..nEpochs) {
        var epochLoss = Float32(0.0)
        var epochCorrect = 0
        
        // 训练
        for (i in 0..nTrainSamples) {
            let x = trainInputs[i]
            let label = trainLabels[i]
            
            // 前向传播
            let c1Out = conv1.forward(x, (1, imgSize, imgSize, 1))
            let p1Out = pool1.forward(c1Out, (1, conv1OutH, conv1OutW, 8))
            let c2Out = conv2.forward(p1Out, (1, pool1OutH, pool1OutW, 8))
            let p2Out = pool2.forward(c2Out, (1, conv2OutH, conv2OutW, 16))
            
            let flatOut = flatten.forward(p2Out, (1, pool2OutH, pool2OutW, 16))
            
            // 转换为全连接层输入
            var fc1Input = Matrix1(1, flatSize)
            for (j in 0..flatSize) {
                fc1Input.set(0, j, flatOut.get(j, 0).getOrThrow())
            }
            
            let fc1Out = fc1.forward(fc1Input)
            let fc2Out = fc2.forward(fc1Out)
            
            // Softmax
            let probs = computeSoftmax(fc2Out)
            
            // 计算损失和准确率
            let probVal = probs.get(0, label).getOrThrow().getValue()
            let loss = -log(max(probVal, Float32(1e-10)))
            epochLoss = epochLoss + loss
            
            let predClass = predictClass(probs)
            if (predClass == label) {
                epochCorrect += 1
            }
            
            // 反向传播
            var delta = Matrix1(1, nClasses)
            for (c in 0..nClasses) {
                let prob = probs.get(0, c).getOrThrow()
                if (Int64(c) == label) {
                    delta.set(0, c, prob - FP32(1.0))
                } else {
                    delta.set(0, c, prob)
                }
            }
            
            let fc2Delta = fc2.backward(delta)
            let fc1Delta = fc1.backward(fc2Delta)
            
            // 转换回特征图格式
            var flatDelta = Matrix1(flatSize, 1)
            for (j in 0..flatSize) {
                flatDelta.set(j, 0, fc1Delta.get(0, j).getOrThrow())
            }
            
            let p2Delta = flatten.backward(flatDelta)
            let c2Delta = pool2.backward(p2Delta)
            let p1Delta = conv2.backward(c2Delta)
            let c1Delta = pool1.backward(p1Delta)
            let _ = conv1.backward(c1Delta)
            
            // 批次更新 - 使用 Adam
            if ((i + 1) % batchSize == 0) {
                adamConv1.updateConvLayer(conv1)    // Conv1 - Adam
                adamConv2.updateConvLayer(conv2)    // Conv2 - Adam
                adamFC1.updateFCLayer(fc1)          // FC1 - Adam
                adamFC2.updateFCLayer(fc2)          // FC2 - Adam
                
                // 池化层仍使用 SGD
                pool1.update(Float32(1.0))
                pool2.update(Float32(1.0))
            }
        }
        
        // 最终更新 - 使用 Adam
        adamConv1.updateConvLayer(conv1)
        adamConv2.updateConvLayer(conv2)
        adamFC1.updateFCLayer(fc1)
        adamFC2.updateFCLayer(fc2)
        pool1.update(Float32(1.0))
        pool2.update(Float32(1.0))
        
        let avgLoss = epochLoss / Float32(nTrainSamples)
        let accuracy = Float32(epochCorrect) / Float32(nTrainSamples) * Float32(100.0)
        
        if (accuracy > bestAccuracy) {
            bestAccuracy = accuracy
        }
        
        // 输出训练信息
        if (epoch % 1 == 0 || epoch == nEpochs - 1) {
            println("Epoch ${epoch + 1}/${nEpochs} - Loss: ${avgLoss}, Accuracy: ${accuracy}%, Adam Step: ${adamConv1.getStep()}")
        }
    }
    
    println("")
    println("训练完成! 最佳训练准确率: ${bestAccuracy}%")
    println("总更新步数: ${adamConv1.getStep()}")
    println("")
    
    // 测试
    println("开始测试...")
    println("")
    
    var testCorrect = 0
    var confusion = Array(nClasses, { _ => Array(nClasses, { _ => 0 }) })
    var testLoss = Float32(0.0)
    
    for (i in 0..nTestSamples) {
        let x = testInputs[i]
        let label = testLabels[i]
        
        // 前向传播
        let c1Out = conv1.forward(x, (1, imgSize, imgSize, 1))
        let p1Out = pool1.forward(c1Out, (1, conv1OutH, conv1OutW, 8))
        let c2Out = conv2.forward(p1Out, (1, pool1OutH, pool1OutW, 8))
        let p2Out = pool2.forward(c2Out, (1, conv2OutH, conv2OutW, 16))
        
        let flatOut = flatten.forward(p2Out, (1, pool2OutH, pool2OutW, 16))
        
        var fc1Input = Matrix1(1, flatSize)
        for (j in 0..flatSize) {
            fc1Input.set(0, j, flatOut.get(j, 0).getOrThrow())
        }
        
        let fc1Out = fc1.forward(fc1Input)
        let fc2Out = fc2.forward(fc1Out)
        
        let probs = computeSoftmax(fc2Out)
        
        // 计算损失
        let probVal = probs.get(0, label).getOrThrow().getValue()
        testLoss = testLoss + Float32(-log(max(probVal, Float32(1e-10))))
        
        // 预测
        let predClass = predictClass(probs)
        
        confusion[label][predClass] += 1
        if (predClass == label) {
            testCorrect += 1
        }
        
        // 显示前几个错误分类的样本
        if (predClass != label && testCorrect < 10) {
            println("错误分类样本 ${i + 1}:")
            showSample(x, imgSize, label)
            let shapeNames = ["圆形", "正方形", "三角形"]
            println("  真实标签: ${shapeNames[label]}, 预测标签: ${shapeNames[predClass]}")
            println("")
        }
    }
    
    let testAcc = Float32(testCorrect) / Float32(nTestSamples) * Float32(100.0)
    let avgTestLoss = testLoss / Float32(nTestSamples)
    
    println("测试结果:")
    println("  准确率: ${testAcc}% (${testCorrect}/${nTestSamples})")
    println("  平均损失: ${avgTestLoss}")
    println("")
    
    let classNames = ["圆形  ", "正方形", "三角形"]
    println("各类别准确率:")
    for (c in 0..nClasses) {
        var total = 0
        for (p in 0..nClasses) {
            total += confusion[c][p]
        }
        if (total > 0) {
            let acc = Float32(confusion[c][c]) / Float32(total) * Float32(100.0)
            println("  ${classNames[c]}: ${confusion[c][c]}/${total} (${acc}%)")
        }
    }
    println("")
    
    println("混淆矩阵:")
    println("              预测圆形  预测正方形  预测三角形")
    for (t in 0..nClasses) {
        var line = "真实${classNames[t]}:"
        for (p in 0..nClasses) {
            line = line + "      ${confusion[t][p]}      "
        }
        println(line)
    }
    println("")
    
    printLine()
    println("几何图形分类测试完成!")
    printLine()
}