package bot4cj.th

foreign func pthread_mutex_init(mutex: CPointer<UInt8>, attr: CPointer<UInt64>): Int32
foreign func pthread_mutex_destroy(mutex: CPointer<UInt8>): Int32
foreign func pthread_mutex_lock(mutex: CPointer<UInt8>): Int32
foreign func pthread_mutex_trylock(mutex: CPointer<UInt8>): Int32
foreign func pthread_mutex_unlock(mutex: CPointer<UInt8>): Int32
@C
public func _mutex_create(): CPointer<UInt8> {
	return unsafe { LibC.malloc<UInt8>(count: 48) }
}

@C
public func _mutex_free(p: CPointer<UInt8>) {
	unsafe { LibC.free(p) }
}

/* FIXME: 异常处理 */
@C
public func _mutex_init(mutex: CPointer<UInt8>) {
	let attr = CPointer<UInt64>()
	unsafe { pthread_mutex_init(mutex, attr) }
}

@C
public func _mutex_destroy(mutex: CPointer<UInt8>) {
	unsafe { pthread_mutex_destroy(mutex) }
}

@C
public func _mutex_lock(mutex: CPointer<UInt8>) {
	unsafe { pthread_mutex_lock(mutex) }
}

@C
public func _mutex_trylock(mutex: CPointer<UInt8>) {
	unsafe { pthread_mutex_trylock(mutex) }
}

@C
public func _mutex_unlock(mutex: CPointer<UInt8>) {
	unsafe { pthread_mutex_unlock(mutex) }
}
