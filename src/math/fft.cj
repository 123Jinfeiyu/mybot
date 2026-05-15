package bot4cj.math
foreign func start_fft(M: CPointer<Unit>): CPointer<Unit>
foreign func gen_wave(): CPointer<Unit>

public func genWave(): Matrix {
    unsafe {
        return Matrix(gen_wave())
    }
}
public func start_fft(M: Matrix): Matrix {
    unsafe {
        return Matrix(start_fft(M.getData()))
    }
}
