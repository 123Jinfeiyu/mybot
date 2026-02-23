package bot4cj

/* MPU6050传感器数据 */
@C
struct MPU6050_sensorData {
    var accel_x: Int16 = 0
    var accel_y: Int16 = 0
    var accel_z: Int16 = 0
    var gyro_x:  Int16 = 0
    var gyro_y:  Int16 = 0
    var gyro_z:  Int16 = 0
    public func print() {  /* 格式化打印 */
        println("[accel x, y, z] ${accel_x}, ${accel_y}, ${accel_z}")
        println("[gyro  x, y, z] ${gyro_x},  ${gyro_y},  ${gyro_z}")
        println("--------------------")
    }
}

/* MPU6050陀螺仪、加速度计
 *   输入并持有一个I2C引用 */
class MPU6050 {
    public init(i2c: I2C) {
        this.i2c = i2c
        i2c.setAddr(ADDR_0);
        i2c.writeReg(PWR_MGMT_1, PWR_NOSLEEP);        /* 退出睡眠 */
        i2c.writeReg(GYRO_CONF_REG, GYRO_FS_500DPS);  /* ±500°/s */
        i2c.writeReg(ACCEL_CONF_REG, ACCEL_FS_8g);    /*  ±8g */
        sleep(Duration.millisecond * 100);            /* 等待稳定 */
    }
    
    /*-------- FIXME: 使用枚举类*/
    private static const ADDR_0:           UInt8 = 0x68  /* 默认地址 */
    private static const PWR_MGMT_1:       UInt8 = 0x6B  /* 电源管理寄存器1 */
    private static const PWR_NOSLEEP:      UInt8 = 0x00  /* 电源管理 - 非睡眠模式 */
    private static const GYRO_CONF_REG:    UInt8 = 0x1B  /* 陀螺仪配置寄存器 */
    private static const GYRO_FS_500DPS:   UInt8 = 0x08  /* 陀螺仪量程 - 正负500度/秒 */
    private static const GYRO_REG_X_HIGH:  UInt8 = 0x43  /* x - 陀螺仪横轴 高字节 */
    private static const GYRO_REG_X_LOW:   UInt8 = 0x44
    private static const GYRO_REG_Y_HIGH:  UInt8 = 0x45  /* y */
    private static const GYRO_REG_Y_LOW:   UInt8 = 0x46
    private static const GYRO_REG_Z_HIGH:  UInt8 = 0x47  /* z */
    private static const GYRO_REG_Z_LOW:   UInt8 = 0x48
    private static const ACCEL_CONF_REG:   UInt8 = 0x1C  /* 加速度计配置寄存器 */
    private static const ACCEL_FS_8g:      UInt8 = 0x10  /* 加速度计量程 正负8g */
    private static const ACCEL_REG_X_HIGH: UInt8 = 0x3B
    private static const ACCEL_REG_X_LOW:  UInt8 = 0x3C
    private static const ACCEL_REG_Y_HIGH: UInt8 = 0x3D
    private static const ACCEL_REG_Y_LOW:  UInt8 = 0x3E
    private static const ACCEL_REG_Z_HIGH: UInt8 = 0x3F
    private static const ACCEL_REG_Z_LOW:  UInt8 = 0x40
    /*----------------*/
    private var i2c: Option<I2C> = None

    /* 读取 16 位传感器数据（带符号）
     * FIXME: 是否能够正确处理符号位 */
    private func read16BitReg(high: UInt8, low: UInt8): Int16 {
        let i2 = i2c.getOrThrow()
        let hh: Int16 = Int16(i2.readRegFlag(high))
        let ll: Int16 = Int16(i2.readRegFlag(low))
        return (hh << 8) | ll
    }

    /* 获取原始MPU6050传感器数据 */
    public func readSensor(): MPU6050_sensorData {
        var data = MPU6050_sensorData()
        data.accel_x = read16BitReg(ACCEL_REG_X_HIGH, ACCEL_REG_X_LOW);
        data.accel_y = read16BitReg(ACCEL_REG_Y_HIGH, ACCEL_REG_Y_LOW);
        data.accel_z = read16BitReg(ACCEL_REG_Z_HIGH, ACCEL_REG_Z_LOW);
        data.gyro_x  = read16BitReg(GYRO_REG_X_HIGH, GYRO_REG_X_LOW);
        data.gyro_y  = read16BitReg(GYRO_REG_Y_HIGH, GYRO_REG_Y_LOW);
        data.gyro_z  = read16BitReg(GYRO_REG_Z_HIGH, GYRO_REG_Z_LOW);
        return data
    }
}

