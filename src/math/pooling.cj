// pooling_layer.cj
package bot4cj.math

import std.math.*
import std.collection.*

/**
 * 池化层类
 * 设计风格与 Nn1 和 ConvLayer 保持一致
 */
public class PoolingLayer {

    // ==================== 层配置参数 ====================
    let nChannels: Int64
    let poolHeight: Int64
    let poolWidth: Int64
    let stride: Int64
    let padding: Int64
    let poolType: String
    let f: Af
    let learningRateWeight: FP32
    let learningRateBias: FP32

    // ==================== 可学习参数 ====================
    var weights: Matrix1
    var biases: Matrix1

    // ==================== 运行时缓存 ====================
    var prevInput: Option<Matrix1> = Option.None
    var prevInputShape: Option<(Int64, Int64, Int64, Int64)> = Option.None
    var prevCombination: Option<Matrix1> = Option.None
    var prevOutput: Option<Matrix1> = Option.None
    var prevOutputShape: Option<(Int64, Int64, Int64, Int64)> = Option.None
    var prevMaxIndices: Option<Matrix1> = Option.None

    var weightsGrad: Option<Matrix1> = Option.None
    var biasGrad: Option<Matrix1> = Option.None

    // ==================== 构造函数 ====================
    public init(
        nChannels: Int64,
        poolSize: (Int64, Int64),
        stride: Int64,
        padding: Int64,
        poolType: String,
        activation: Af,
        lrWeight: FP32,
        lrBias: FP32
    ) {
        this.nChannels = nChannels
        poolHeight = poolSize[0]
        poolWidth = poolSize[1]
        this.stride = stride
        this.padding = padding
        this.poolType = poolType
        this.f = activation
        learningRateWeight = lrWeight
        learningRateBias = lrBias

        // 初始化权值
        weights = Matrix1(1, nChannels)
        if (poolType == "average") {
            let avgWeight = FP32(1.0) / FP32(Float32(poolHeight * poolWidth))
            for (c in 0..nChannels) {
                weights.set(0, c, avgWeight)
            }
        } else {
            for (c in 0..nChannels) {
                weights.set(0, c, FP32(1.0))
            }
        }

        // 初始化偏置为 0
        biases = Matrix1(1, nChannels)
        for (c in 0..nChannels) {
            biases.set(0, c, FP32(0.0))
        }
    }

    public init(
        nChannels: Int64,
        poolSize: (Int64, Int64),
        poolType: String,
        activation: Af
    ) {
        this.nChannels = nChannels
        poolHeight = poolSize[0]
        poolWidth = poolSize[1]
        stride = poolHeight
        padding = 0
        this.poolType = poolType
        this.f = activation
        learningRateWeight = FP32(0.1)
        learningRateBias = FP32(0.1)

        weights = Matrix1(1, nChannels)
        if (poolType == "average") {
            let avgWeight = FP32(1.0) / FP32(Float32(poolHeight * poolWidth))
            for (c in 0..nChannels) {
                weights.set(0, c, avgWeight)
            }
        } else {
            for (c in 0..nChannels) {
                weights.set(0, c, FP32(1.0))
            }
        }

        biases = Matrix1(1, nChannels)
        for (c in 0..nChannels) {
            biases.set(0, c, FP32(0.0))
        }
    }

    // ==================== 前向传播 ====================
    public func forward(x: Matrix1, inputShape: (Int64, Int64, Int64, Int64)): Matrix1 {
        prevInput = Option.Some(x)
        prevInputShape = Option.Some(inputShape)

        let batch = inputShape[0]
        let inH = inputShape[1]
        let inW = inputShape[2]

        let outH = (inH + 2 * padding - poolHeight) / stride + 1
        let outW = (inW + 2 * padding - poolWidth) / stride + 1
        let outSize = batch * outH * outW

        var combination = Matrix1(outSize, nChannels)
        var output = Matrix1(outSize, nChannels)
        var maxIndices = Matrix1(outSize, nChannels)

        for (b in 0..batch) {
            for (oh in 0..outH) {
                for (ow in 0..outW) {
                    let outRow = b * outH * outW + oh * outW + ow

                    for (ch in 0..nChannels) {
                        var pooledVal = FP32(0.0)
                        var maxIdx = 0

                        if (poolType == "max") {
                            var maxVal = FP32(-1.0e30)
                            var kernelIdx = 0
                            for (kh in 0..poolHeight) {
                                for (kw in 0..poolWidth) {
                                    let ih = oh * stride + kh - padding
                                    let iw = ow * stride + kw - padding

                                    if (ih >= 0 && ih < inH && iw >= 0 && iw < inW) {
                                        let inRow = b * inH * inW + ih * inW + iw
                                        let val = x.get(inRow, ch).getOrThrow()
                                        if (val.getValue() > maxVal.getValue()) {
                                            maxVal = val
                                            maxIdx = kernelIdx
                                        }
                                    }
                                    kernelIdx += 1
                                }
                            }
                            pooledVal = maxVal
                        } else {
                            var sum = FP32(0.0)
                            var count = 0
                            for (kh in 0..poolHeight) {
                                for (kw in 0..poolWidth) {
                                    let ih = oh * stride + kh - padding
                                    let iw = ow * stride + kw - padding

                                    if (ih >= 0 && ih < inH && iw >= 0 && iw < inW) {
                                        let inRow = b * inH * inW + ih * inW + iw
                                        sum = sum + x.get(inRow, ch).getOrThrow()
                                        count += 1
                                    }
                                }
                            }
                            if (count > 0) {
                                pooledVal = sum / FP32(Float32(count))
                            }
                        }

                        combination.set(outRow, ch, pooledVal)
                        maxIndices.set(outRow, ch, FP32(Float32(maxIdx)))
                    }
                }
            }
        }

        // 逐通道应用权值和偏置
        for (r in 0..outSize) {
            for (ch in 0..nChannels) {
                let val = combination.get(r, ch).getOrThrow()
                let w = weights.get(0, ch).getOrThrow()
                let b = biases.get(0, ch).getOrThrow()
                combination.set(r, ch, val * w + b)
            }
        }

        prevCombination = Option.Some(combination)

        output = f.forward(combination)
        prevOutput = Option.Some(output)
        prevOutputShape = Option.Some((batch, outH, outW, nChannels))
        prevMaxIndices = Option.Some(maxIndices)

        return output
    }

    // ==================== 反向传播 ====================
    public func backward(errs: Matrix1): Matrix1 {
        var x = Matrix1(1, 1)
        match (prevInput) {
            case Some(val) => x = val
            case _ => throw Exception("No input data")
        }
        
        var shape = (0, 0, 0, 0)
        match (prevInputShape) {
            case Some(val) => shape = val
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

        var maxIndices = Matrix1(1, 1)
        let hasMaxIndices = match (prevMaxIndices) {
            case Some(_) => true
            case _ => false
        }
        if (hasMaxIndices) {
            maxIndices = match (prevMaxIndices) {
                case Some(val) => val
                case _ => Matrix1(1, 1)
            }
        }
        
        let batch = shape[0]
        let inH = shape[1]
        let inW = shape[2]
        let outH = outShape[1]
        let outW = outShape[2]
        
        let delta = errs * f.backward(combination)

        calcWeightBiasGradient(delta, combination)

        let errsUp = calcUpsampledError(delta, maxIndices, batch, inH, inW, outH, outW)
        
        return errsUp
    }

    /**
     * 计算权值和偏置梯度
     */
    private func calcWeightBiasGradient(delta: Matrix1, combination: Matrix1): Unit {
        let outSize = delta.get_row()
        
        var wGrad = Matrix1(1, nChannels)
        var bGrad = Matrix1(1, nChannels)

        for (ch in 0..nChannels) {
            var wSum = FP32(0.0)
            var bSum = FP32(0.0)
            for (r in 0..outSize) {
                let d = delta.get(r, ch).getOrThrow()
                bSum = bSum + d
                let comb = combination.get(r, ch).getOrThrow()
                wSum = wSum + d * comb
            }
            wGrad.set(0, ch, wSum)
            bGrad.set(0, ch, bSum)
        }

        weightsGrad = Option.Some(wGrad)
        biasGrad = Option.Some(bGrad)
    }

    /**
     * 上采样误差（池化层的反向传播核心）
     */
    private func calcUpsampledError(
        delta: Matrix1,
        maxIndices: Matrix1,
        batch: Int64,
        inH: Int64,
        inW: Int64,
        outH: Int64,
        outW: Int64
    ): Matrix1 {
        let inSize = batch * inH * inW
        var errsUp = Matrix1(inSize, nChannels)

        for (b in 0..batch) {
            for (oh in 0..outH) {
                for (ow in 0..outW) {
                    let deltaRow = b * outH * outW + oh * outW + ow

                    for (ch in 0..nChannels) {
                        let deltaVal = delta.get(deltaRow, ch).getOrThrow()
                        let w = weights.get(0, ch).getOrThrow()

                        if (poolType == "max") {
                            let maxIdx = Int64(maxIndices.get(deltaRow, ch).getOrThrow().getValue())
                            
                            var kernelIdx = 0
                            var found = false
                            for (kh in 0..poolHeight) {
                                for (kw in 0..poolWidth) {
                                    if (kernelIdx == maxIdx && !found) {
                                        let ih = oh * stride + kh - padding
                                        let iw = ow * stride + kw - padding

                                        if (ih >= 0 && ih < inH && iw >= 0 && iw < inW) {
                                            let inRow = b * inH * inW + ih * inW + iw
                                            errsUp.set(inRow, ch, deltaVal * w)
                                            found = true
                                        }
                                    }
                                    kernelIdx += 1
                                }
                            }
                        } else {
                            var count = 0
                            for (kh in 0..poolHeight) {
                                for (kw in 0..poolWidth) {
                                    let ih = oh * stride + kh - padding
                                    let iw = ow * stride + kw - padding
                                    if (ih >= 0 && ih < inH && iw >= 0 && iw < inW) {
                                        count += 1
                                    }
                                }
                            }
                            
                            if (count > 0) {
                                let avgDelta = deltaVal * w / FP32(Float32(count))
                                for (kh in 0..poolHeight) {
                                    for (kw in 0..poolWidth) {
                                        let ih = oh * stride + kh - padding
                                        let iw = ow * stride + kw - padding
                                        if (ih >= 0 && ih < inH && iw >= 0 && iw < inW) {
                                            let inRow = b * inH * inW + ih * inW + iw
                                            let oldVal = errsUp.get(inRow, ch).getOrThrow()
                                            errsUp.set(inRow, ch, oldVal + avgDelta)
                                        }
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
        var wGrad = Matrix1(1, 1)
        match (weightsGrad) {
            case Some(val) => wGrad = val
            case _ => throw Exception("No weight gradients")
        }
        
        var bGrad = Matrix1(1, 1)
        match (biasGrad) {
            case Some(val) => bGrad = val
            case _ => throw Exception("No bias gradients")
        }
        
        for (ch in 0..nChannels) {
            let oldW = weights.get(0, ch).getOrThrow()
            let gW = wGrad.get(0, ch).getOrThrow()
            weights.set(0, ch, oldW + gW * learningRateWeight * FP32(discount))
        }
        
        for (ch in 0..nChannels) {
            let oldB = biases.get(0, ch).getOrThrow()
            let gB = bGrad.get(0, ch).getOrThrow()
            biases.set(0, ch, oldB + gB * learningRateBias * FP32(discount))
        }
    }

    // ==================== 形状访问器 ====================
    public func getInShape(): (Int64, Int64) {
        (nChannels, poolHeight * poolWidth)
    }

    public func getOutShape(): Int64 {
        nChannels
    }

    public func getPoolType(): String {
        poolType
    }

    public func getLR(): FP32 {
        learningRateWeight
    }

    public func getFType(): String {
        f.get_type()
    }

    public func calculateOutputSize(inputHeight: Int64, inputWidth: Int64): (Int64, Int64) {
        let outH = (inputHeight + 2 * padding - poolHeight) / stride + 1
        let outW = (inputWidth + 2 * padding - poolWidth) / stride + 1
        (outH, outW)
    }
}
