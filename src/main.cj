package bot4cj
import std.io.*
import std.env.*
import std.math.*
import std.convert.*
import std.collection.*
const I2C_PATH = "/dev/i2c-1"

/* 机器人类 
 * TODO: 增加缓慢到达方法
 * TODO: 增加机器人构造偏差 
 * TODO: 增加脚本起始状态限制、最终状态记录
 * FIXME: print()以原貌打印
 * FIXME: parse()兼容空的特殊行 */

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
                count = this.next(aa, count)
            }
            catch (exp: BufOverTheLine) { /* 临时占位 */
                throw exp
            }
            catch (exp: IllegalArgumentException) {
                throw UnknowExcept(count)
            }
        }
    }

    /* 打印整个动作脚本
     * */
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
        let max = bot[0] /* 第一行是上限 */
        let min = bot[1] /* 第二行是下限 */
        let i2 = i2c.getOrThrow()
        let pca9685 = PCA9685(i2)
        //let mp = MPU6050(i2)
        //let data = mp.readSensor()
        //data.print()
        for (i in 2..bot.size) { /* 从第2行开始遍历 */
            let a = bot[i]       /* 选中一行 */
            for (j in 0..a.size-1) {  /* 遍历关节（最后一列是时延） */
                let ma = abs(max[j])  /* 取得该关节最大值 */
                let mi = abs(min[j])  /* 取得该关节最小值 */
                let rf = max[j] < 0.0f32  /* rotate flag 逆转标志 */
                var t = a[j]              /* 选中单个 */
                t = if (ma < t) { ma } else { t }  /* 对上限进行切片 */
                t = if (mi < t) { t }  else { mi } /* 对下限进行切片 */
                t = if (rf) { 180.0f32 - t } else { t } /* 是否逆转 */
                let jj = UInt8(j) /* FIXME: 应该从一开始就用UInt8进行存储 */
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
    private func next(c: Rune, count: Int64): Int64 {
        var cc: Int64 = count
        /* ---- comment ---- */

        /* 注释行开始标志 */
        if (!cm && c == r'#') {
            cm = true
        }

        /* 注释行结束标识 */
        else if (cm && c == r'\n') {
            nl = true
            sl = false
            cm = false
            cc += 1
        }

        /* 注释内容
         * FIXME: EOF */
        else if (cm && c != r'\n') {
            ;
        }
        /* ---------------- new line ---------------- */

        /* 新行且为空字符 */
        else if (nl && (c == r' ' || c == r'\t')) { 
            ;
        }

        /* 新行且为空行 */
        else if (nl && c == r'\n') {
            cc += 1
        }

        /* 新行且为特殊行 */
        else if (nl && c == r'!') { 
            nl = false
            sl = true
        }

        /* 新行且不为特殊行、制表符和空格：录入 */
        else if (nl && c != r'!') {
            nl = false
            sl = false
            tmp.add(c) /* 到缓冲区 */
            if (tmp.size > TMP_LEN) { /* 缓冲区过长 */
                throw BufOverTheLine(count)
            }
        }
        /* ---------------- 非首行 ---------------- */
        /* 空格与制表符：两个列之间的分隔字符 */
        else if (!nl && (c == r' ' || c = r'\t')) {
            if (!tmp.isEmpty()) { /* 结算当前 */
                let f = fromTmp()
                grp.add(f)
            }
        }

        /* 分号：一行的结束标志 */
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

        /* 非空格、非制表符、非分号 （FIXME: 且是数字） */
        else if (!nl) {
            tmp.add(c) /* 到缓冲区 */
            if (tmp.size > TMP_LEN) { /* 缓冲区过长 */
                throw BufOverTheLine(count)
            }
        }

        cc
    }

    /* 从文字缓冲区解析、清理并输出数值 */
    private func fromTmp(): Float32 {
        let str = String(tmp)
        let f = Float32.parse(str)
        tmp.clear()
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
