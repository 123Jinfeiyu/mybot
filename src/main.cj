package bot4cj
import std.io.*;
import std.convert.*
import std.collection.*
import stdx.net.http.*;
foreign func I2C_init(path: CString): Int16
foreign func I2C_free(fd: Int16): Unit
foreign func PCA9685_init(fd: Int16, addr: UInt8): Unit
foreign func PCA9685_setAngle(fd: Int16, channel: UInt8, angle: Float32): Unit
const BOT_ADDR = '0.0.0.0'
const BOT_PORT = 5201u16
const I2C_PATH = "/dev/i2c-1"
class PCA9685 {
    public init(path: String) {
        unsafe{
            var cstr = LibC.mallocCString(path)
            fd = I2C_init(cstr)
            if (fd <= 0) { /* 打开设备失败 */
                throw Exception("Can't open i2c device")
            }
            LibC.free(cstr)
            PCA9685_init(fd, 0x40)
        }
    }
    public func turnAngle(channel: UInt8, angle: Float32) {
        unsafe {
            PCA9685_setAngle(fd, channel, angle)
        }
    }
    ~init() {
        unsafe {
            I2C_free(fd)
        }
    }
    private var fd: Int16
}

class Actor {
    public init(filepath: String) {
        pca = PCA9685(filepath)
    }
    public func parse(ss: StringReader<InputStream>) {
        var count: Int64 = 0;
        while (true) {  /* FIXME(花间岛): while(let Some(f) <- ss.read()) */
            try {       /*      (qing): 链式调用可读性反而降低 */
                let a = ss.read()
                let aa = match(a) {
                    case Some(v) => v
                    case None => throw ReadOutOfStream(count)
                }
                count += 1
                this.next(aa)
            }
            catch (exp: ReadOutOfStream) {
                break
            }
        }
    }

    private var pca: Option<PCA9685> = None
    private var nl: Bool = true   /* new line */
    private var sl: Bool = false  /* special */
    private var cm: Bool = false  /* comment */
    private var tmp = ArrayList<Rune>()
    private var grp = ArrayList<Float32>()
    private var bot = ArrayList<ArrayList<Float32>>()

    private func next(c: Rune) {
        /* ---- comment ---- */
        if (!cm && c == r'#') {
            cm = true
        }
        else if (cm && c == r'\n') {
            nl = true
            sl = false
            cm = false
        }
        else if (cm && c != r'\n') { /* FIXME: EOF */
            ;
        }
        else if (nl && c == r' ') {
            ;
        }
        else if (nl && c == r'\n') {
            ;
        }
        /* ---- new line ---- */
        else if (nl && c == r'!') {
            nl = false
            sl = true
        }
        else if (nl && c != r'!') {
            nl = false
            sl = false
            tmp.add(c)
        }
        /* ---- ---- */
        else if (!nl && c == r' ') {
            let f = fromTmp()
            grp.add(f)
        }
        else if (!nl && c == r';') {
            let f = fromTmp()
            grp.add(f)
            bot.add(grp)
            grp.clear()
            nl = true
            sl = false
            cm = false
        }
        else if (!nl) {
             tmp.add(c)
        }
    }
    private func fromTmp(): Float32 {
        let str = String(tmp)
        tmp.clear()
        let f = Float32.parse(str)
        return f
    }
}
private class ReadOutOfStream <: Exception {
    public ReadOutOfStream(public let length: Int64) {
        super('we got length: ${length}')
    }
}
main(_: Array<String>): Int64 {

    let pca = PCA9685("/dev/i2c-1")
    pca.turnAngle(1, 0.0)
    return 0;

    let serv = ServerBuilder()
        .addr(BOT_ADDR)
        .port(BOT_PORT)
        .build()

    serv.distributor.register('/api/v2/bot_run', { context =>
        if (context.request.method == 'POST') {
            let ss = StringReader(context.request.body)
            let actor = Actor(I2C_PATH)
            actor.parse(ss)
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
