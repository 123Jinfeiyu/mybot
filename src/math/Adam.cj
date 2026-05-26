// Adam.cj - Adam 优化器实现（添加全连接层支持）
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
     * 更新矩阵参数（通用方法）
     * @param param 要更新的参数矩阵
     * @param grad 梯度矩阵
     * @param key 参数在HashMap中的键名
     */
    private func updateMatrix(
        param: Matrix1,
        grad: Matrix1,
        key: String
    ): Unit {
        // 初始化动量
        let hasState = m.contains(key)
        if (!hasState) {
            let rows = param.get_row()
            let cols = param.get_col()
            m.add(key, Matrix1(rows, cols))
            v.add(key, Matrix1(rows, cols))
        }
        
        let mMat = m.get(key).getOrThrow()
        let vMat = v.get(key).getOrThrow()
        
        let rows = param.get_row()
        let cols = param.get_col()
        
        for (r in 0..rows) {
            for (c in 0..cols) {
                let g = grad.get(r, c).getOrThrow()
                
                // 更新一阶矩
                let oldM = mMat.get(r, c).getOrThrow()
                let newM = beta1 * oldM + (FP32(1.0) - beta1) * g
                mMat.set(r, c, newM)
                
                // 更新二阶矩
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
                let oldParam = param.get(r, c).getOrThrow()
                let update = learningRate * mHat / (sqrt(vHat) + epsilon)
                param.set(r, c, oldParam - update)
            }
        }
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
            let key = "conv_kernel_" + oc.toString()
            updateMatrix(layer.kernels[oc], kGrad[oc], key)
        }
        
        // 更新偏置
        var bGrad = match (layer.biasGrad) {
            case Some(val) => val
            case _ => throw Exception("No bias gradient")
        }
        
        let biasKey = "conv_bias"
        updateMatrix(layer.biases, bGrad, biasKey)
    }
    
    /**
     * 更新全连接层参数（Nn1）
     */
    public func updateFCLayer(layer: Nn1): Unit {
        t += 1
        
        // 更新权重
        var wGrad = match (layer.weights_grad) {
            case Some(val) => val
            case _ => throw Exception("No weights gradient")
        }
        
        let weightKey = "fc_weights"
        updateMatrix(layer.weights, wGrad, weightKey)
        
        // 更新偏置
        var bGrad = match (layer.bias_grad) {
            case Some(val) => val
            case _ => throw Exception("No bias gradient")
        }
        
        let biasKey = "fc_bias"
        updateMatrix(layer.bias, bGrad, biasKey)
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