package bot4cj
import std.io.*
import std.env.*
import std.math.*
import std.convert.*
import std.collection.*
const I2C_PATH = "/dev/i2c-1"

/* 机器人类 */
class Actor {

    public init(filepath: String) {
        i2c = I2C(filepath)
    }
    
    /*
     * 解析器 - 循环不变式
     *
     *
     * 初始化：ss是一个动作脚本的输入流
     *
     * 保持：ss是动作脚本剩余部分的输入流
     *
     * 终止：当输入流为空时，循环终止
     */
    public func parse(inputStream: InputStream) {
        let ss = StringReader(inputStream)
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

    /* 开始演绎 ---
     * 按照解析出来的顺序进行旋转 */
    public func play() {
        let max = bot[0]
        let min = bot[1]
        let i2 = i2c.getOrThrow()
        let pca9685 = PCA9685(i2)
        let mp = MPU6050(i2)
        let data = mp.readSensor()
        data.print()
        for (i in 2..bot.size) {
            let a = bot[i] /* 选中一行 */
            for (j in 0..a.size-1) {
                let ma = abs(max[j])
                let mi = abs(min[j])
                let rf = max[j] < 0.0f32  /* rotate flag */
                var t = a[j]              /* 选中单个 */
                t = if (ma < t) { ma } else { t }
                t = if (mi < t) { t }  else { mi }
                t = if (rf) { 180.0f32 - t } else { t }
                let jj = UInt8(j)
                pca9685.setAngle(jj, t)
            }
            let aa = a.last.getOrThrow()
            sleep(Duration.millisecond * Int64(aa))
        }
    }

    /*-----------------*/
    private var i2c: Option<I2C> = None
    private var nl: Bool = true   /* new line */
    private var sl: Bool = false  /* special */
    private var cm: Bool = false  /* comment */
    private var tmp = ArrayList<Rune>()    /* 文字缓冲区 */
    private var grp = ArrayList<Float32>() /* 单行 */
    private var bot = ArrayList<ArrayList<Float32>>()
    private static const TMP_LEN = 16      /* 文字缓冲区不超过 */
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
            tmp.add(c) /* 到缓冲区 */
            if (tmp.size > TMP_LEN) { /* 缓冲区过长 */
                throw BufOverTheLine(count)
            }
        }
        /* ---- ---- */
        else if (!nl && c == r' ') {
            if (!tmp.isEmpty()) { /* 结算当前 */
                let f = fromTmp()
                grp.add(f)
            }
        }
        else if (!nl && c == r';') {
            if (!tmp.isEmpty()) { /* 结算当前 */
                let f = fromTmp()
                grp.add(f)
            }
            if (!grp.isEmpty()) { /* 结算一行 */
                bot.add(grp.clone())
                grp.clear()
            }
            nl = true
            sl = false
            cm = false
        }
        else if (!nl) {
            tmp.add(c) /* 到缓冲区 */
            if (tmp.size > TMP_LEN) { /* 缓冲区过长 */
                throw BufOverTheLine(count)
            }
        }
    }

    /* 从文字缓冲区输出并解析 */
    private func fromTmp(): Float32 {
        let str = String(tmp)
        tmp.clear()
        let f = Float32.parse(str)
        return f
    }
}

/* 解析异常 */
open private class ParseExcept <: Exception {
    public ParseExcept(msg: String, length: Int64) {
        super('${msg}. we got length: ${length}')
    }
}

/* 缓冲区过长 */
private class BufOverTheLine <: ParseExcept {
    public BufOverTheLine(count: Int64) {
        super('Buffer length over the line', count)
    }
}

/* 未知异常 */
private class UnknowExcept <: ParseExcept {
    public UnknowExcept(count: Int64) {
        super("I don't understand what happened", count)
    }
}

main(_: Array<String>): Int64 {

    let consoleReader = getStdIn()
    let actor = Actor(I2C_PATH)
    actor.parse(consoleReader)
    actor.print()
    actor.play()
    return 0
}
