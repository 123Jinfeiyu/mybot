// Adam.cj - Adam 优化器实现
package bot4cj.math

import std.math.*
import std.collection.*

public class Adam {
    let learningRate: FP32
    let beta1: FP32
    let beta2: FP32
    let epsilon: FP32
    
    var m: HashMap<String, Matrix1>
    var v: HashMap<String, Matrix1>
    var t: Int64
    
    // 完整参数构造函数
    public init(
        lr: FP32,
        beta1: FP32,
        beta2: FP32,
        epsilon: FP32
    ) {
        learningRate = lr
        this.beta1 = beta1
        this.beta2 = beta2
        this.epsilon = epsilon
        m = HashMap<String, Matrix1>()
        v = HashMap<String, Matrix1>()
        t = 0
    }
    
    // 默认参数构造函数
    public init() {
        learningRate = FP32(0.001)
        beta1 = FP32(0.9)
        beta2 = FP32(0.999)
        epsilon = FP32(0.00000001)
        m = HashMap<String, Matrix1>()
        v = HashMap<String, Matrix1>()
        t = 0
    }
    
    // 仅自定义学习率的构造函数
    public init(lr: FP32) {
        learningRate = lr
        beta1 = FP32(0.9)
        beta2 = FP32(0.999)
        epsilon = FP32(0.00000001)
        m = HashMap<String, Matrix1>()
        v = HashMap<String, Matrix1>()
        t = 0
    }
    
    /**
     * 计算幂运算的辅助函数
     */
    private func power(base: FP32, exp: Int64): FP32 {
        var result = FP32(1.0)
        for (i in 0..exp) {
            result = result * base
        }
        return result
    }
    
    /**
     * 计算平方根的辅助函数 - 使用牛顿迭代法
     */
    private func sqrt(val: FP32): FP32 {
        let x = val.getValue()
        if (x <= 0.0) {
            return FP32(0.0)
        }
        var guess = x
        for (i in 0..10) {
            guess = (guess + x / guess) / 2.0
        }
        return FP32(guess)
    }
    
    /**
     * 更新卷积层参数
     */
    public func updateConvLayer(layer: ConvLayer): Unit {
        t += 1
        
        // 更新卷积核
        var kGrad = match (layer.kernelsGrad) {
            case Some(val) => val
            case _ => throw Exception("No kernel gradients")
        }
        
        for (oc in 0..layer.nOutputChannels) {
            let key = "kernel_" + oc.toString()
            
            // 初始化动量
            let hasM = m.contains(key)
            if (!hasM) {
                let rows = layer.kernels[oc].get_row()
                let cols = layer.kernels[oc].get_col()
                m.add(key, Matrix1(rows, cols))
                v.add(key, Matrix1(rows, cols))
            }
            
            let mMat = m.get(key).getOrThrow()
            let vMat = v.get(key).getOrThrow()
            let grad = kGrad[oc]
            
            let rows = layer.kernels[oc].get_row()
            let cols = layer.kernels[oc].get_col()
            
            for (r in 0..rows) {
                for (c in 0..cols) {
                    let g = grad.get(r, c).getOrThrow()
                    
                    // 更新动量
                    let oldM = mMat.get(r, c).getOrThrow()
                    let newM = beta1 * oldM + (FP32(1.0) - beta1) * g
                    mMat.set(r, c, newM)
                    
                    let oldV = vMat.get(r, c).getOrThrow()
                    let newV = beta2 * oldV + (FP32(1.0) - beta2) * g * g
                    vMat.set(r, c, newV)
                    
                    // 偏差修正
                    let one = FP32(1.0)
                    let beta1Pow = power(beta1, t)
                    let beta2Pow = power(beta2, t)
                    
                    let mHat = newM / (one - beta1Pow)
                    let vHat = newV / (one - beta2Pow)
                    
                    // 更新参数
                    let oldParam = layer.kernels[oc].get(r, c).getOrThrow()
                    let update = learningRate * mHat / (sqrt(vHat) + epsilon)
                    layer.kernels[oc].set(r, c, oldParam - update)
                }
            }
        }
        
        // 更新偏置
        var bGrad = match (layer.biasGrad) {
            case Some(val) => val
            case _ => throw Exception("No bias gradient")
        }
        
        let biasKey = "bias"
        let hasBiasM = m.contains(biasKey)
        if (!hasBiasM) {
            let rows = layer.biases.get_row()
            let cols = layer.biases.get_col()
            m.add(biasKey, Matrix1(rows, cols))
            v.add(biasKey, Matrix1(rows, cols))
        }
        
        let mBias = m.get(biasKey).getOrThrow()
        let vBias = v.get(biasKey).getOrThrow()
        
        for (c in 0..layer.nOutputChannels) {
            let g = bGrad.get(0, c).getOrThrow()
            
            let oldM = mBias.get(0, c).getOrThrow()
            let newM = beta1 * oldM + (FP32(1.0) - beta1) * g
            mBias.set(0, c, newM)
            
            let oldV = vBias.get(0, c).getOrThrow()
            let newV = beta2 * oldV + (FP32(1.0) - beta2) * g * g
            vBias.set(0, c, newV)
            
            // 偏差修正
            let one = FP32(1.0)
            let beta1Pow = power(beta1, t)
            let beta2Pow = power(beta2, t)
            
            let mHat = newM / (one - beta1Pow)
            let vHat = newV / (one - beta2Pow)
            
            let oldBias = layer.biases.get(0, c).getOrThrow()
            let update = learningRate * mHat / (sqrt(vHat) + epsilon)
            layer.biases.set(0, c, oldBias - update)
        }
    }
    
    /**
     * 重置优化器状态
     */
    public func reset(): Unit {
        m.clear()
        v.clear()
        t = 0
    }
    
    /**
     * 获取当前迭代次数
     */
    public func getStep(): Int64 {
        t
    }
}