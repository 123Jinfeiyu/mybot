// flatten.cj - 移除调试代码
package bot4cj.math

import std.math.*
import std.collection.*

public class FlattenLayer {

    let nInputChannels: Int64
    let inputHeight: Int64
    let inputWidth: Int64
    let outputSize: Int64

    var prevInput: Option<Matrix1>
    var prevInputShape: Option<(Int64, Int64, Int64, Int64)>
    var prevOutput: Option<Matrix1>
    var prevOutputShape: Option<(Int64, Int64)>

    public init(inputChannels: Int64, inputHeight: Int64, inputWidth: Int64) {
        nInputChannels = inputChannels
        this.inputHeight = inputHeight
        this.inputWidth = inputWidth
        outputSize = inputChannels * inputHeight * inputWidth
        
        prevInput = None
        prevInputShape = None
        prevOutput = None
        prevOutputShape = None
    }

    public init(inputShape: (Int64, Int64, Int64)) {
        nInputChannels = inputShape[0]
        inputHeight = inputShape[1]
        inputWidth = inputShape[2]
        outputSize = nInputChannels * inputHeight * inputWidth
        
        prevInput = None
        prevInputShape = None
        prevOutput = None
        prevOutputShape = None
    }

    public func forward(x: Matrix1, inputShape: (Int64, Int64, Int64, Int64)): Matrix1 {
        prevInput = x
        prevInputShape = inputShape

        let batch = inputShape[0]
        let inH = inputShape[1]
        let inW = inputShape[2]
        let inCh = inputShape[3]

        let totalRows = batch * outputSize
        var output = Matrix1(totalRows, 1)

        for (b in 0..batch) {
            for (h in 0..inH) {
                for (w in 0..inW) {
                    for (ch in 0..inCh) {
                        let inRow = b * inH * inW + h * inW + w
                        let outRow = b * outputSize + (ch * inH * inW + h * inW + w)
                        if (outRow < totalRows) {
                            let val = x.get(inRow, ch).getOrThrow()
                            output.set(outRow, 0, val)
                        }
                    }
                }
            }
        }

        prevOutput = output
        prevOutputShape = (totalRows, 1)

        return output
    }

    public func backward(errs: Matrix1): Matrix1 {
        var shape = (0, 0, 0, 0)
        match (prevInputShape) {
            case Some(T) => shape = T
            case _ => throw Exception("FlattenLayer.backward: No input shape. Call forward() first.")
        }
        
        let batch = shape[0]
        let inH = shape[1]
        let inW = shape[2]
        let inCh = shape[3]

        let inSize = batch * inH * inW
        var errsUp = Matrix1(inSize, nInputChannels)

        let errsCols = errs.get_col()

        for (b in 0..batch) {
            for (h in 0..inH) {
                for (w in 0..inW) {
                    let inRow = b * inH * inW + h * inW + w

                    for (ch in 0..inCh) {
                        let outRow = b * outputSize + (ch * inH * inW + h * inW + w)
                        
                        if (outRow < errs.get_row()) {
                            if (errsCols == 1) {
                                let val = errs.get(outRow, 0).getOrThrow()
                                errsUp.set(inRow, ch, val)
                            } else {
                                var sum = FP32(0.0)
                                for (col in 0..errsCols) {
                                    sum = sum + errs.get(outRow, col).getOrThrow()
                                }
                                errsUp.set(inRow, ch, sum)
                            }
                        }
                    }
                }
            }
        }

        return errsUp
    }

    public func update(discount: Float32): Unit {
        // 无参数
    }

    public func getInShape(): (Int64, Int64, Int64) {
        (nInputChannels, inputHeight, inputWidth)
    }

    public func getOutShape(): Int64 {
        outputSize
    }

    public func getOutputSize(): Int64 {
        outputSize
    }
}
