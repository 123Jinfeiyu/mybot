// cnn.cj - 修正 forward 中的偏置加法
package bot4cj.math

import std.math.*
import std.random.*
import std.collection.*

public class ConvLayer {

    let nInputChannels: Int64
    let nOutputChannels: Int64
    let kernelHeight: Int64
    let kernelWidth: Int64
    let stride: Int64
    let padding: Int64
    let learningRate: FP32
    let f: Af

    var connectingMatrix: Matrix1
    var kernels: Array<Matrix1>
    var biases: Matrix1

    var prevInput: Option<Matrix1> = Option.None
    var prevInputShape: Option<(Int64, Int64, Int64, Int64)> = Option.None
    var prevCombination: Option<Matrix1> = Option.None
    var prevOutput: Option<Matrix1> = Option.None
    var prevOutputShape: Option<(Int64, Int64, Int64, Int64)> = Option.None

    var kernelsGrad: Option<Array<Matrix1>> = Option.None
    var biasGrad: Option<Matrix1> = Option.None

    public init(
        inCh: Int64,
        outCh: Int64,
        kernelSize: (Int64, Int64),
        stride: Int64,
        padding: Int64,
        lr: FP32,
        activation: Af,
        connMatrix: ?Matrix1
    ) {
        nInputChannels = inCh
        nOutputChannels = outCh
        kernelHeight = kernelSize[0]
        kernelWidth = kernelSize[1]
        this.stride = stride
        this.padding = padding
        learningRate = lr
        this.f = activation

        // 初始化 connectingMatrix
        connectingMatrix = match (connMatrix) {
            case m: Matrix1 => m
            case _ => Matrix1(nInputChannels, nOutputChannels)
        }
        let isNone = match (connMatrix) {
            case _: Matrix1 => false
            case _ => true
        }
        if (isNone) {
            for (r in 0..nInputChannels) {
                for (c in 0..nOutputChannels) {
                    connectingMatrix.set(r, c, FP32(1.0))
                }
            }
        }

        // 初始化 kernels
        kernels = Array(nOutputChannels, { _ => Matrix1(1, 1) })
        for (oc in 0..nOutputChannels) {
            var nConnections = 0
            for (ic in 0..nInputChannels) {
                let val = connectingMatrix.get(ic, oc).getOrThrow()
                if (val.getValue() > 0.0) {
                    nConnections += 1
                }
            }
            let kernelLen = nConnections * kernelHeight * kernelWidth
            let kernel = Matrix1(1, kernelLen)
            
            Ini.xavierUniformInit(kernel, 1, kernelLen)
            
            kernels[oc] = kernel
        }

        // 初始化 biases
        biases = Matrix1(1, nOutputChannels)
        Ini.xavierUniformInit(biases, 1, nOutputChannels)
    }

    // ==================== 前向传播 ====================
    public func forward(x: Matrix1, inputShape: (Int64, Int64, Int64, Int64)): Matrix1 {
        prevInput = Option.Some(x)
        prevInputShape = Option.Some(inputShape)

        let batch = inputShape[0]
        let inH = inputShape[1]
        let inW = inputShape[2]

        let outH = (inH + 2 * padding - kernelHeight) / stride + 1
        let outW = (inW + 2 * padding - kernelWidth) / stride + 1
        let outSize = batch * outH * outW

        var combination = Matrix1(outSize, nOutputChannels)
        var output = Matrix1(outSize, nOutputChannels)

        for (b in 0..batch) {
            for (oh in 0..outH) {
                for (ow in 0..outW) {
                    let outRow = b * outH * outW + oh * outW + ow

                    for (oc in 0..nOutputChannels) {
                        var sum = FP32(0.0)
                        var kernelIdx = 0

                        for (ic in 0..nInputChannels) {
                            let connVal = connectingMatrix.get(ic, oc).getOrThrow()
                            if (connVal.getValue() > 0.0) {
                                for (kh in 0..kernelHeight) {
                                    for (kw in 0..kernelWidth) {
                                        let ih = oh * stride + kh - padding
                                        let iw = ow * stride + kw - padding

                                        if (ih >= 0 && ih < inH && iw >= 0 && iw < inW) {
                                            let inRow = b * inH * inW + ih * inW + iw
                                            let xVal = x.get(inRow, ic).getOrThrow()
                                            let kVal = kernels[oc].get(0, kernelIdx).getOrThrow()
                                            sum = sum + xVal * kVal
                                        }
                                        kernelIdx += 1
                                    }
                                }
                            }
                        }

                        combination.set(outRow, oc, sum)
                    }
                }
            }
        }

        // 手动广播偏置加法（因为 Matrix1 的 + 要求形状相同）
        for (r in 0..outSize) {
            for (c in 0..nOutputChannels) {
                let val = combination.get(r, c).getOrThrow()
                let biasVal = biases.get(0, c).getOrThrow()
                combination.set(r, c, val + biasVal)
            }
        }

        prevCombination = Option.Some(combination)

        output = f.forward(combination)
        prevOutput = Option.Some(output)
        prevOutputShape = Option.Some((batch, outH, outW, nOutputChannels))

        return output
    }

    // ==================== 反向传播 ====================
    public func backward(errs: Matrix1): Matrix1 {
        var x = Matrix1(1, 1)
        match (prevInput) {
            case Some(T) => x = T
            case _ => throw Exception("No input data")
        }
        
        var shape = (0, 0, 0, 0)
        match (prevInputShape) {
            case Some(T) => shape = T
            case _ => throw Exception("No input shape")
        }
        
        var combination = Matrix1(1, 1)
        match (prevCombination) {
            case Some(val) => combination = val
            case _ => throw Exception("No combination")
        }
        
        var outShape = (0, 0, 0, 0)
        match (prevOutputShape) {
            case Some(val) => outShape = val
            case _ => throw Exception("No output shape")
        }
        
        let batch = shape[0]
        let inH = shape[1]
        let inW = shape[2]
        let outH = outShape[1]
        let outW = outShape[2]
        
        let delta = errs * f.backward(combination)
        
        calcKernelGradient(x, delta, batch, inH, inW, outH, outW)
        calcBiasGradient(delta)
        
        let errsUp = calcUpsampledError(delta, batch, inH, inW, outH, outW)
        
        return errsUp
    }

    /**
     * 计算卷积核梯度
     */
    private func calcKernelGradient(
        x: Matrix1,
        delta: Matrix1,
        batch: Int64,
        inH: Int64,
        inW: Int64,
        outH: Int64,
        outW: Int64
    ): Unit {
        var grads = Array(nOutputChannels, { _ => Matrix1(1, 1) })
        
        for (oc in 0..nOutputChannels) {
            let kernelCols = kernels[oc].get_col()
            var grad = Matrix1(1, kernelCols)
            
            for (b in 0..batch) {
                for (oh in 0..outH) {
                    for (ow in 0..outW) {
                        let deltaRow = b * outH * outW + oh * outW + ow
                        let deltaVal = delta.get(deltaRow, oc).getOrThrow()
                        
                        var kernelIdx = 0
                        for (ic in 0..nInputChannels) {
                            let connVal = connectingMatrix.get(ic, oc).getOrThrow()
                            if (connVal.getValue() > 0.0) {
                                for (kh in 0..kernelHeight) {
                                    for (kw in 0..kernelWidth) {
                                        let ih = oh * stride + kh - padding
                                        let iw = ow * stride + kw - padding
                                        
                                        if (ih >= 0 && ih < inH && iw >= 0 && iw < inW) {
                                            let inRow = b * inH * inW + ih * inW + iw
                                            let xVal = x.get(inRow, ic).getOrThrow()
                                            let oldGrad = grad.get(0, kernelIdx).getOrThrow()
                                            grad.set(0, kernelIdx, oldGrad + xVal * deltaVal)
                                        }
                                        kernelIdx += 1
                                    }
                                }
                            }
                        }
                    }
                }
            }
            
            grads[oc] = grad
        }
        
        kernelsGrad = Option.Some(grads)
    }

    /**
     * 计算偏置梯度
     */
    private func calcBiasGradient(delta: Matrix1): Unit {
        let rows = delta.get_row()
        var grad = Matrix1(1, nOutputChannels)
        
        for (oc in 0..nOutputChannels) {
            var sum = FP32(0.0)
            for (r in 0..rows) {
                sum = sum + delta.get(r, oc).getOrThrow()
            }
            grad.set(0, oc, sum)
        }
        
        biasGrad = Option.Some(grad)
    }

    /**
     * 计算传递到上一层的误差
     */
    private func calcUpsampledError(
        delta: Matrix1,
        batch: Int64,
        inH: Int64,
        inW: Int64,
        outH: Int64,
        outW: Int64
    ): Matrix1 {
        let inSize = batch * inH * inW
        var errsUp = Matrix1(inSize, nInputChannels)
        
        for (b in 0..batch) {
            for (oh in 0..outH) {
                for (ow in 0..outW) {
                    let deltaRow = b * outH * outW + oh * outW + ow
                    
                    for (oc in 0..nOutputChannels) {
                        let deltaVal = delta.get(deltaRow, oc).getOrThrow()
                        
                        var kernelIdx = 0
                        for (ic in 0..nInputChannels) {
                            let connVal = connectingMatrix.get(ic, oc).getOrThrow()
                            if (connVal.getValue() > 0.0) {
                                for (kh in 0..kernelHeight) {
                                    for (kw in 0..kernelWidth) {
                                        let ih = oh * stride + kh - padding
                                        let iw = ow * stride + kw - padding
                                        
                                        if (ih >= 0 && ih < inH && iw >= 0 && iw < inW) {
                                            let inRow = b * inH * inW + ih * inW + iw
                                            let kVal = kernels[oc].get(0, kernelIdx).getOrThrow()
                                            let oldErr = errsUp.get(inRow, ic).getOrThrow()
                                            errsUp.set(inRow, ic, oldErr + deltaVal * kVal)
                                        }
                                        kernelIdx += 1
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        
        return errsUp
    }

    // ==================== 参数更新 ====================
    public func update(discount: Float32): Unit {
        var kGrad = Array(0, { _ => Matrix1(1, 1) })
        match (kernelsGrad) {
            case Some(val) => kGrad = val
            case _ => throw Exception("No kernel gradients")
        }
        
        var bGrad = Matrix1(1, 1)
        match (biasGrad) {
            case Some(val) => bGrad = val
            case _ => throw Exception("No bias gradient")
        }
        
        for (oc in 0..nOutputChannels) {
            let rows = kernels[oc].get_row()
            let cols = kernels[oc].get_col()
            for (r in 0..rows) {
                for (c in 0..cols) {
                    let oldVal = kernels[oc].get(r, c).getOrThrow()
                    let gradVal = kGrad[oc].get(r, c).getOrThrow()
                    kernels[oc].set(r, c, oldVal - gradVal * learningRate * FP32(discount))
                }
            }
        }
        
        for (oc in 0..nOutputChannels) {
            let oldBias = biases.get(0, oc).getOrThrow()
            let gradBias = bGrad.get(0, oc).getOrThrow()
            biases.set(0, oc, oldBias - gradBias * learningRate * FP32(discount))
        }
    }

    // ==================== 形状访问器 ====================
    public func getInShape(): (Int64, Int64) {
        (nInputChannels, kernelHeight * kernelWidth)
    }

    public func getOutShape(): Int64 {
        nOutputChannels
    }

    public func getLR(): FP32 {
        learningRate
    }

    public func getFType(): String {
        f.get_type()
    }

    public func calculateOutputSize(inputHeight: Int64, inputWidth: Int64): (Int64, Int64) {
        let outH = (inputHeight + 2 * padding - kernelHeight) / stride + 1
        let outW = (inputWidth + 2 * padding - kernelWidth) / stride + 1
        (outH, outW)
    }
}
