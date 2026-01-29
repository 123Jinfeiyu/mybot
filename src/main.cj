package bot4cj
import std.io.*;
import stdx.net.http.*;
foreign func I2C_init(path: CString): Int16
foreign func I2C_free(fd: Int16): Unit
foreign func PCA9685_init(fd: Int16, addr: UInt8): Unit
foreign func PCA9685_setAngle(fd: Int16, channel: UInt8, angle: Float32): Unit
const BOT_ADDR = '0.0.0.0'
const BOT_PORT = 5201u16
class ScriptParser {
    public static func parse(ss: StringReader<InputStream>) {
        var count: Int64 = 0;
        while (true) {
            try {
                let a = ss.read()
                let aa = match(a) {
                    case Some(v) => v
                    case None => throw ReadOutOfStream(count)
                }
                count += 1
            }
            catch (exp: ReadOutOfStream) {
                break
            }
        }
    }
}
private class ReadOutOfStream <: Exception {
    public ReadOutOfStream(public let length: Int64) {
        super('we got length: ${length}')
    }
}
main(_: Array<String>): Int64 {
    unsafe{
        var path = LibC.mallocCString("/dev/i2c-1")
        var fd: Int16 = I2C_init(path)
        if (fd <= 0) { /* 打开设备失败 */
            return 1
        }
        LibC.free(path)
        PCA9685_init(fd, 0x40)
        PCA9685_setAngle(fd, 0, 0.0)
        I2C_free(fd)
    }
    let serv = ServerBuilder()
        .addr(BOT_ADDR)
        .port(BOT_PORT)
        .build()
    serv.distributor.register('/api/v2/bot_run', { context =>
        if (context.request.method == 'POST') {
            let ss = StringReader(context.request.body)
            let act = ScriptParser.parse(ss)
            /* ---- 响应 ---- */
            let headers = HttpHeaders()
            headers.add('Content-Type', 'text/html; charset=utf-8;')
            context.responseBuilder.setHeaders(headers)
            context.responseBuilder.body('OK')
        }
    })
    println('Bot at ${BOT_ADDR}:${BOT_PORT}')
    serv.serve()
    return 0
}
