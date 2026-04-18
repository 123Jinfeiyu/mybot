package bot4cj
import std.io.*
import std.env.*
import std.math.*
import std.time.*
import std.random.*
import std.convert.*
import std.collection.*
import stdx.net.*
const I2C_PATH = "/dev/i2c-1"

/* 机器人类 
 * TODO: 增加缓慢到达方法
 * TODO: 增加机器人构造偏差 
 * TODO: 机器人构造脚本
 * TODO: 增加脚本起始状态限制、最终状态记录
 * TODO: 增加动态调节变量
 * TODO：增加check()方法，检查脚本列数是否对应
 * TODO：解析pi为单位的角度
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

        //check()
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

    /* 开始演绎 --- 按照解析出来的顺序进行旋转
     * FIXME: 考虑max和min符号不同的情况 */
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
        /* ---------------- 非首字 ---------------- */
        /* 空格与制表符：两个列之间的分隔字符 */
        else if (!nl && (c == r' ' || c == r'\t')) {
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

main(args: Array<String>): Int64 {

    println(args[0])
    if (args.size == 2 && args[1] == "--serve") {
        
        let serv = ServerBuilder()
        .addr("127.0.0.1")
        .port(5119)
        .build()

        /* curl -X POST /api/v2/bot_run -d ${cat filepath} */
        serv.distributor.register('/api/v2/bot_run', { context =>
            if (context.request.method == 'POST') {
                let ss = StringReader(context.request.body)
                let actor = Actor(I2C_PATH)
                actor.parse(ss)
                //println("-------------")
                //actor.print()
                //actor.play()
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

    let consoleReader = getStdIn()
    let actor = Actor(I2C_PATH)
    actor.parse(consoleReader)
    //actor.print()
    actor.play()


    /* test */
    /*let m = Matrix.initWithXavierUniform(10, 20)
    let p = m[1, 2]
    let s1 = Matrix(20, 12)
    let s2 = Matrix(10, 20)
    let s3 = s1 + 2.0f32
    let r1 = m + 12.0f32
    let r2 = r1 + r1;
    let r3 = r2 - 5.0f32;
    let r4 = m + 2.0f32
    let r5 = r3 - r4
    let r6 = r5 * 3.0f32
    let r7 = r6 / 2.0f32
    let dr1 = r7 * s3
    let pdr1 = dr1[1, 2]
    print(m == s1)
    print("\t")
    print(m == s2)
    println()
    print(m.hasDotShapeWith(s1))
    print("\t")
    print(m.hasDotShapeWith(s2))
    println()
    print(dr1.getRow())
    print(',\t')
    print(dr1.getCol())
    println()
    unsafe {
        if (!p.isNull()) {
            println(p.read())
            println(pdr1.read())
        }
    }*/


    /* test nn */
    /*let batch = 1
    let num = 10000
    let prt_num = 1
    let f = ActivationFuncs.Leaky_ReLU
    let nns = ArrayList<Nn>()
    nns.add(Nn(2, 4, f, 0.1f32))
    nns.add(Nn(4, 2, f, 0.1f32))
    let random = Random(UInt64(DateTime.now().nanosecond))
    let wsgrads = ArrayList<ArrayList<Matrix>>(nns.size, {_: Int64 => ArrayList<Matrix>()})
    let bsgrads = ArrayList<ArrayList<Matrix>>(nns.size, {_: Int64 => ArrayList<Matrix>()})
    for (i in 0..(num*batch)) {
        let m = Matrix(1, 2)
	let a = random.nextInt8(2)
	let b = random.nextInt8(2)
	unsafe {
            m[0, 0].write(Float32(a))
            m[0, 1].write(Float32(b))
        }
 
        let outs_dof = ArrayList<Matrix> ()
        let outs_dotf = ArrayList<Matrix> ()

        for (idx in 0..nns.size) {
            let x = if (idx == 0) { m } else { outs_dof[idx-1] }
            let res = nns[idx].forward(x)
            let out_dof = f.fdo(res)
            let out_dotf = f.fdot(out_dof)
            outs_dof.add(out_dof)
            outs_dotf.add(out_dotf)
        }

        var errs = Matrix(1, 2)
	unsafe {
            let t0 = if (a  == b) { 1.0f32 } else { 0.0f32 }    
            let t1 = if (a  != b) { 1.0f32 } else { 0.0f32 }    
            let r0 = outs_dof[nns.size-1][0, 0].read()
            let r1 = outs_dof[nns.size-1][0, 1].read()
            let loss0 =  -t0 * log(r0) - (1.0f32-t0) * log(1.0f32-r0);
            let loss1 =  -t1 * log(r1) - (1.0f32-t1) * log(1.0f32-r1);
            let err0 = r0 - t0
            let err1 = r1 - t1
            errs[0, 0].write(err0)
            errs[0, 1].write(err1)
            if (i % prt_num == prt_num-1) {
                print("${i},\t${t0},\t${t1}\t:\t${r0},\t${r1}\t& \t ${err0},\t${err1}")
                print("\t: ${0.5 * (loss0*loss0 + loss1*loss1)}")
                println()
            }
        }

        for (idx in (nns.size-1)..=0:-1) {
            let up = if (idx == 0) { m} else { outs_dof[idx-1] }
            let bias_grad = errs
            //let delta = nns[idx].bak_bsgrad(errs, outs_dotf[idx])
            let weights_grad = nns[idx].bak_wsgrad(up, bias_grad)
            if (idx > 0) {
                errs = nns[idx].bak_uperrs(bias_grad, outs_dotf[idx-1])
            }
            wsgrads[idx].add(weights_grad)
            bsgrads[idx].add(bias_grad)
            if (wsgrads[idx].size >= batch) {
                for (j in 0..wsgrads[idx].size) {
                    nns[idx].update(wsgrads[idx][j], bsgrads[idx][j], 1.0f32/Float32(batch))
                }
                wsgrads[idx] = ArrayList<Matrix>()
                bsgrads[idx] = ArrayList<Matrix>()
            }
        }
    }*/

    return 0
}
