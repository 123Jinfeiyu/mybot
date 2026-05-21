package bot4cj.th
foreign func pthread_mutex_init(mutex: CPointer<UInt8>, attr: CPointer<UInt64>): Int32
foreign func pthread_mutex_destroy(mutex: CPointer<UInt8>): Int32
foreign func pthread_mutex_lock(mutex: CPointer<UInt8>): Int32
foreign func pthread_mutex_trylock(mutex: CPointer<UInt8>): Int32
foreign func pthread_mutex_unlock(mutex: CPointer<UInt8>): Int32

/* -------- 互斥锁类型的字节数 -------*/
const SIZE_PTHREAD_MUTEX_TYPE_BYTE = 48

/* -------- 为一个新的互斥锁申请内存 -------- */
@C
public func _mutex_create(): CPointer<UInt8> {
	return unsafe { LibC.malloc<UInt8>(count: SIZE_PTHREAD_MUTEX_TYPE_BYTE) }
}

/* -------- 释放一段互斥锁内存 -------- */
@C
public func _mutex_free(p: CPointer<UInt8>) {
	unsafe { LibC.free(p) }
}

/* -------- 初始化一个互斥锁，需要内存空间 --------
 * FIXME: 异常处理
 * TODO: 自动申请内存 */
@C
public func _mutex_init(mutex: CPointer<UInt8>) {
	let attr = CPointer<UInt64>()
	unsafe { pthread_mutex_init(mutex, attr) }
}

/* -------- 销毁一个互斥锁，不释放内存空间 -------- */
@C
public func _mutex_destroy(mutex: CPointer<UInt8>) {
	unsafe { pthread_mutex_destroy(mutex) }
}

/* -------- 互斥锁上锁 -------- */
@C
public func _mutex_lock(mutex: CPointer<UInt8>) {
	unsafe { pthread_mutex_lock(mutex) }
}

/* -------- 尝试上锁 -------- */
@C
public func _mutex_trylock(mutex: CPointer<UInt8>) {
	unsafe { pthread_mutex_trylock(mutex) }
}

/* -------- 互斥锁解锁 --------*/
@C
public func _mutex_unlock(mutex: CPointer<UInt8>) {
	unsafe { pthread_mutex_unlock(mutex) }
}
