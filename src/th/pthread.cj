package bot4cj.th

foreign func pthread_create(thread: CPointer<UInt64>,  attr: CPointer<Unit>,
                   start_routine: CFunc<(CPointer<Unit>) -> CPointer<Unit>>,
                   arg: CPointer<Unit>): Int32
foreign func pthread_join(thread: UInt64, retval: CPointer<CPointer<Unit>>): Int32
foreign func pthread_exit(retval: CPointer<Unit>): Unit
foreign func pthread_cancel(thread: UInt64): Int32


/* -------- pthread线程类 --------
   FIXME(20260520-qing): 目前仅支持ARM64  */
public class Pth {
	private var pthread: UInt64 = 0
	public init(_main: CFunc<(CPointer<Unit>) -> CPointer<Unit>>,
			 args: CPointer<Unit>) {
		var pt = pthread
		let ret = unsafe {
			pthread_create(inout pt,  CPointer<Unit>(), _main, args) }
		pthread = pt
	}

	/* -------- 等待线程退出，获取返回值 --------*/
	public func join(retval: CPointer<CPointer<Unit>>): Unit {
		unsafe { pthread_join(pthread, retval)  }
	}

	/* -------- 取消线程 --------
	   NOTE(20260520-qing): 取消线程之后应该join()  */
	public func close(): Unit {
		let ret = unsafe { pthread_cancel(pthread) }
	}
}
