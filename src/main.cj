package bot4cj
import std.io.*
import std.time.*
import std.math.*
import std.convert.*
import std.collection.*
import stdx.net.http.*
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

/* 机器人类 */
class Actor {

    public init(filepath: String) {
        pca9685 = PCA9685(filepath)
    }
    
    public func parse(ss: StringReader<InputStream>) {
        var count: Int64 = 0;
        while (true) {  /* FIXME(花间岛): while(let Some(f) <- ss.read()) */
            try {       /*      (qing): 链式调用可读性反而降低 */
                let a = ss.read()
                let aa = match(a) {
                    case Some(v) => v
                    case None => break
                }
                count += 1
                print(aa)
                this.next(aa, count)
            }
            catch (exp: BufOverTheLine) { /* 临时占位 */
                throw exp
            }
            catch (exp: IllegalArgumentException) {
                throw UnknowExcept(count)
            }
        }
    }

    public func print() {
        for (array in bot) {
            for (item in array) {
                print(item)
                print(" ")
            }
            print("\n")
        }
    }

    public func play() {
        let max = bot[0]
        let min = bot[1]
        let pca = pca9685.getOrThrow()
        for (i in 2..bot.size) {
            let a = bot[i]
            for (j in 0..a.size-1) {
                let ma = abs(bot[0][j])
                let mi = abs(bot[1][j])
                let rf: Bool = bot[0][j] < 0.0f32  /* rotate flag */
                var t = a[j]
                t = if (ma < t) { ma } else { t }
                t = if (mi < t) { t }  else { mi }
                t = if (rf) { 180.0f32 - t } else { t }
                let jj = UInt8(j)
                pca.turnAngle(jj, t)
            }
            let aa = a.last.getOrThrow()
            sleep(Duration.millisecond * Int64(aa))
        }
    }

    /*-----------------*/
    private var pca9685: Option<PCA9685> = None
    private var nl: Bool = true   /* new line */
    private var sl: Bool = false  /* special */
    private var cm: Bool = false  /* comment */
    private var tmp = ArrayList<Rune>()
    private var grp = ArrayList<Float32>()
    private var bot = ArrayList<ArrayList<Float32>>()
    private static const TMP_LEN = 16
    private func next(c: Rune, count: Int64) {
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
            if (tmp.size > TMP_LEN) {
                throw BufOverTheLine(count)
            }
        }
        /* ---- ---- */
        else if (!nl && c == r' ') {
            if (!tmp.isEmpty()) {
                let f = fromTmp()
                grp.add(f)
            }
        }
        else if (!nl && c == r';') {
            if (!tmp.isEmpty()) {
                let f = fromTmp()
                grp.add(f)
            }
            if (!grp.isEmpty()) {
                bot.add(grp.clone())
                grp.clear()
            }
            nl = true
            sl = false
            cm = false
        }
        else if (!nl) {
            tmp.add(c)
            if (tmp.size > TMP_LEN) {
                throw BufOverTheLine(count)
            }
        }
    }
    private func fromTmp(): Float32 {
        let str = String(tmp)
        tmp.clear()
        let f = Float32.parse(str)
        return f
    }
}

open private class ParseExcept <: Exception {
    public ParseExcept(msg: String, length: Int64) {
        super('${msg}. we got length: ${length}')
    }
}

private class BufOverTheLine <: ParseExcept {
    public BufOverTheLine(count: Int64) {
        super('Buffer length over the line', count)
    }
}

private class UnknowExcept <: ParseExcept {
    public UnknowExcept(count: Int64) {
        super("I don't understand what happened", count)
    }
}

main(_: Array<String>): Int64 {

    let serv = ServerBuilder()
        .addr(BOT_ADDR)
        .port(BOT_PORT)
        .build()

    serv.distributor.register('/api/v2/bot_run', { context =>
        if (context.request.method == 'POST') {
            let ss = StringReader(context.request.body)
            let actor = Actor(I2C_PATH)
            actor.parse(ss)
            println("-------------")
            actor.print()
            actor.play()
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
