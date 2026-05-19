package bot4cj.th

foreign func pthread_create(thread: CPointer<UInt64>,  attr: CPointer<Unit>,
                   start_routine: CFunc<(CPointer<Unit>) -> CPointer<Unit>>,
                   arg: CPointer<Unit>): Int32
foreign func pthread_join(thread: CPointer<UInt64>, retval: CPointer<CPointer<Unit>>): Int32


public func Pth_init(_main: CFunc<(CPointer<Unit>) -> CPointer<Unit>>, args: CPointer<Unit>): UInt64 {
	var pt: UInt64 = 0u64
	let ret = unsafe {
		pthread_create(inout pt,  CPointer<Unit>(), _main, args) }
	return pt
}
