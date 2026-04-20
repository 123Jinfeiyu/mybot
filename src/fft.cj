package bot4cj
foreign func start_fft(M: CPointer<Unit>): CPointer<Unit>
foreign func gen_wave(): CPointer<Unit>

func genWave(): Matrix {
    unsafe {
        return Matrix(gen_wave())
    }
}
func start_fft(M: Matrix): Matrix {
    unsafe {
        return Matrix(start_fft(M.getData()))
    }
}
