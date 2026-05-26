// test_cnn.cj - 使用 Adam 优化器的版本（所有层）
package bot4cj
import bot4cj.math.*
import std.math.*
import std.random.*
import std.collection.*

func generateSolidColorData(numSamples: Int64): (Array<Matrix1>, Array<Int64>) {
    var inputs = Array(numSamples, { _ => Matrix1(1, 1) })
    var labels = Array(numSamples, { _ => 0 })
    let rng = Random()

    for (i in 0..numSamples) {
        let colorClass = Int64(rng.nextFloat64() * 3.0)
        labels[i] = colorClass

        let h = 8
        let w = 8
        var matrix = Matrix1(h * w, 1)

        let colorValue = match (colorClass) {
            case 0 => FP32(1.0)
            case 1 => FP32(0.5)
            case _ => FP32(0.0)
        }

        for (r in 0..h * w) {
            let noise = FP32(Float32(rng.nextFloat64() - 0.5) * 0.1)
            var val = colorValue + noise
            if (val.getValue() > 1.0) {
                val = FP32(1.0)
            } else if (val.getValue() < 0.0) {
                val = FP32(0.0)
            }
            matrix.set(r, 0, val)
        }

        inputs[i] = matrix
    }

    (inputs, labels)
}

func printSeparator(): Unit {
    println("==================================================")
}

func testCNN(): Unit {
    printSeparator()
    println("CNN 纯色矩阵识别测试 (所有层使用 Adam 优化器)")
    printSeparator()
    println()

    let nTrainSamples = 100
    let nTestSamples = 60
    let nEpochs = 50
    let batchSize = 10

    println("训练样本数: ${nTrainSamples}")
    println("测试样本数: ${nTestSamples}")
    println("训练轮数: ${nEpochs}")
    println("Batch 大小: ${batchSize}")
    println()

    println("创建网络结构...")
    println("Input(8x8x1) -> Conv(3x3,1->4) -> Pool(2x2) -> Conv(3x3,4->8) -> Flatten -> FC(8,3)")
    println()

    let lr = FP32(0.005)

    var conv1 = ConvLayer(1, 4, (3, 3), 1, 0, lr, Af.Leaky_ReLU(FP32(0.01)), Option.None)
    var pool1 = PoolingLayer(4, (2, 2), 2, 0, "max", Af.ReLU, lr, lr)
    var conv2 = ConvLayer(4, 8, (3, 3), 1, 0, lr, Af.Leaky_ReLU(FP32(0.01)), Option.None)
    var flatten = FlattenLayer(8, 1, 1)
    var fc = Nn1(8, 3, Float32(0.2), Af.Softmax)

    // 创建 Adam 优化器（为每个可训练层创建独立的优化器）
    var adamConv1 = Adam(FP32(0.001))   // 卷积层1
    var adamConv2 = Adam(FP32(0.001))   // 卷积层2
    var adamFC = Adam(FP32(0.0005))     // 全连接层（使用更小的学习率）

    println("网络创建完成 (Conv1, Conv2, FC 使用 Adam 优化器)")
    println()

    println("生成数据...")
    let (trainInputs, trainLabels) = generateSolidColorData(nTrainSamples)
    let (testInputs, testLabels) = generateSolidColorData(nTestSamples)
    println("训练集: ${nTrainSamples} 样本")
    println("测试集: ${nTestSamples} 样本")
    println()

    println("开始训练...")
    println()

    for (epoch in 0..nEpochs) {
        var epochLoss = Float32(0.0)
        var epochCorrect = 0

        for (i in 0..nTrainSamples) {
            let x = trainInputs[i]
            let label = trainLabels[i]

            // ===== 前向传播 =====
            let c1Out = conv1.forward(x, (1, 8, 8, 1))
            let p1Out = pool1.forward(c1Out, (1, 6, 6, 4))
            let c2Out = conv2.forward(p1Out, (1, 3, 3, 4))
            let flatOut = flatten.forward(c2Out, (1, 1, 1, 8))
            
            // 调试：确认 flatten.forward 执行成功
            if (i == 0 && epoch == 0) {
                println("DEBUG: flatten.forward done, flatOut rows: ${flatOut.get_row()}, cols: ${flatOut.get_col()}")
            }
            
            var fcInput = Matrix1(1, 8)
            for (j in 0..8) {
                fcInput.set(0, j, flatOut.get(j, 0).getOrThrow())
            }
            let fcOut = fc.forward(fcInput)

            // Softmax
            var softmax = Matrix1(1, 3)
            var maxVal = Float32(-1.0e30)
            for (c in 0..3) {
                let raw = fcOut.get(0, c).getOrThrow().getValue()
                if (raw > maxVal) {
                    maxVal = raw
                }
            }
            var expSum = Float32(0.0)
            for (c in 0..3) {
                let raw = fcOut.get(0, c).getOrThrow().getValue()
                let ev = Float32(exp(raw - maxVal))
                softmax.set(0, c, FP32(ev))
                expSum = expSum + ev
            }
            for (c in 0..3) {
                let v = softmax.get(0, c).getOrThrow().getValue()
                softmax.set(0, c, FP32(v / expSum))
            }

            let probVal = softmax.get(0, label).getOrThrow().getValue()
            let loss = -log(max(probVal, Float32(1e-10)))
            epochLoss = epochLoss + loss

            var maxProb = Float32(-1.0)
            var predClass = 0
            for (c in 0..3) {
                let p = softmax.get(0, c).getOrThrow().getValue()
                if (p > maxProb) {
                    maxProb = p
                    predClass = Int64(c)
                }
            }
            if (predClass == label) {
                epochCorrect += 1
            }

            // ===== 反向传播 =====
            var delta = Matrix1(1, 3)
            for (c in 0..3) {
                let prob = softmax.get(0, c).getOrThrow()
                if (Int64(c) == label) {
                    delta.set(0, c, prob - FP32(1.0))
                } else {
                    delta.set(0, c, prob)
                }
            }

            let fcDelta = fc.backward(delta)
            
            // 调试：确认 fcDelta 形状
            if (i == 0 && epoch == 0) {
                println("DEBUG: fc.backward done, fcDelta rows: ${fcDelta.get_row()}, cols: ${fcDelta.get_col()}")
            }
            
            var flatDelta = Matrix1(8, 1)
            for (j in 0..8) {
                flatDelta.set(j, 0, fcDelta.get(0, j).getOrThrow())
            }
            
            // 调试：确认 flatDelta 形状
            if (i == 0 && epoch == 0) {
                println("DEBUG: flatDelta rows: ${flatDelta.get_row()}, cols: ${flatDelta.get_col()}")
            }
            
            let c2Delta = flatten.backward(flatDelta)
            let p1Delta = conv2.backward(c2Delta)
            let c1Delta = pool1.backward(p1Delta)
            let _ = conv1.backward(c1Delta)

            // ===== 参数更新 =====
            if ((i + 1) % batchSize == 0) {
                // 所有可训练层使用 Adam 更新
                adamConv1.updateConvLayer(conv1)   // 卷积层1 - Adam
                adamConv2.updateConvLayer(conv2)   // 卷积层2 - Adam
                adamFC.updateFCLayer(fc)           // 全连接层 - Adam
                
                // 池化层使用 SGD 更新（如果需要的话）
                pool1.update(Float32(1.0))
            }
        }

        // Epoch 结束时的更新
        adamConv1.updateConvLayer(conv1)
        adamConv2.updateConvLayer(conv2)
        adamFC.updateFCLayer(fc)
        pool1.update(Float32(1.0))

        let avgLoss = epochLoss / Float32(nTrainSamples)
        let accuracy = Float32(epochCorrect) / Float32(nTrainSamples) * Float32(100.0)

        println("Epoch ${epoch + 1}/${nEpochs} - Loss: ${avgLoss}, Accuracy: ${accuracy}%")
        
        // 打印 Adam 的步数信息
        if (epoch == 0) {
            println("  Adam Step: ${adamConv1.getStep()}")
        }
    }

    println("")
    println("开始测试...")
    println("")

    var testCorrect = 0
    var confusion = Array(3, { _ => Array(3, { _ => 0 }) })

    for (i in 0..nTestSamples) {
        let x = testInputs[i]
        let label = testLabels[i]

        let c1Out = conv1.forward(x, (1, 8, 8, 1))
        let p1Out = pool1.forward(c1Out, (1, 6, 6, 4))
        let c2Out = conv2.forward(p1Out, (1, 3, 3, 4))
        let flatOut = flatten.forward(c2Out, (1, 1, 1, 8))
        
        var fcInput = Matrix1(1, 8)
        for (j in 0..8) {
            fcInput.set(0, j, flatOut.get(j, 0).getOrThrow())
        }
        let fcOut = fc.forward(fcInput)

        var softmax = Matrix1(1, 3)
        var maxVal = Float32(-1.0e30)
        for (c in 0..3) {
            let raw = fcOut.get(0, c).getOrThrow().getValue()
            if (raw > maxVal) {
                maxVal = raw
            }
        }
        var expSum = Float32(0.0)
        for (c in 0..3) {
            let raw = fcOut.get(0, c).getOrThrow().getValue()
            let ev = Float32(exp(raw - maxVal))
            softmax.set(0, c, FP32(ev))
            expSum = expSum + ev
        }
        for (c in 0..3) {
            let v = softmax.get(0, c).getOrThrow().getValue()
            softmax.set(0, c, FP32(v / expSum))
        }

        var maxProb = Float32(-1.0)
        var predClass = 0
        for (c in 0..3) {
            let p = softmax.get(0, c).getOrThrow().getValue()
            if (p > maxProb) {
                maxProb = p
                predClass = Int64(c)
            }
        }

        confusion[label][predClass] += 1
        if (predClass == label) {
            testCorrect += 1
        }
    }

    let testAcc = Float32(testCorrect) / Float32(nTestSamples) * Float32(100.0)
    println("测试准确率: ${testAcc}% (${testCorrect}/${nTestSamples})")
    print("")

    let classNames = ["红色(高)", "绿色(中)", "蓝色(低)"]
    println("各类别准确率:")
    for (c in 0..3) {
        var total = 0
        for (p in 0..3) {
            total += confusion[c][p]
        }
        if (total > 0) {
            let acc = Float32(confusion[c][c]) / Float32(total) * Float32(100.0)
            print("  ${classNames[c]}: ${confusion[c][c]}/${total} (${acc}%)")
        }
    }
    println("")

    println("混淆矩阵:")
    println("          预测红  预测绿  预测蓝")
    for (t in 0..3) {
        var line = "真实${classNames[t]}:"
        for (p in 0..3) {
            line = line + "    ${confusion[t][p]}   "
        }
        print(line)
    }
    println("")

    printSeparator()
    println("测试完成!")
    printSeparator()
}