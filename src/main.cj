package bot4cj
import std.io.*
import std.fs.*
import std.env.*
import std.time.*
import std.argopt.*
import std.convert.*
import std.collection.*
import stdx.encoding.json.*
import bot4cj.hd.*
import bot4cj.math.*
import ctype4cj.*

/* ---- 神经网络输出路径 ---- */
var NNS_OUT: Option<String> = None	
main(args: Array<String>): Int64 {
	
	/* ---- 解析命令行参数 ---- */
	let specs = [
		ArgumentSpec.Long("help", ArgumentMode.NoValue),	/* ---- 拉起帮助菜单           ---- */
		ArgumentSpec.Short(r'i', ArgumentMode.RequiredValue),	/* ---- 神经网络模型    - 输入 ---- */
		ArgumentSpec.Short(r'o', ArgumentMode.RequiredValue),	/* ---- 神经网络模型    - 输出 ---- */
		ArgumentSpec.Long("test", ArgumentMode.RequiredValue)	/* ---- 运行一个测试分支 - 需要输入测试类型 ---- */
	]

	/* ---- 执行解析 ---- */
	let parsed = parseArguments(args, specs)
	let options = parsed.options

	//---------------------------
	/* 读取设置项 */

	/*---------------------------*/
	/*---------------------------*/
	/* 初始化神经网络 （init nn）
	 * TODO: 可以去掉内部初始化参数，使用矩阵式模板进行初始化 */


	//var nns: ArrayList<Nn> = 
	//if (options.keys().contains("i")) {
	//	let filename = options['i']
	//	load_nns(filename)  }
	//else {
	//	let nn_in = ArrayList<Nn>()
	//	nn_in.add(Nn(41, 80, ActivationFuncs.Leaky_ReLU, 0.2f32))
	//	nn_in.add(Nn(80, 80, ActivationFuncs.Leaky_ReLU, 0.2f32))
	//	nn_in.add(Nn(80, 40, ActivationFuncs.Leaky_ReLU, 0.2f32))
	//	nn_in.add(Nn(40, 17, ActivationFuncs.Sigmoid, 0.2f32)) // 16个舵机加1个时延
	//	nn_in  }

	/* ---- 使用-o选项进行输出路径的选择 ---- */
	if (options.contains("o")) {
		NNS_OUT = Option<String>.Some(options['o'])
	}

	/*---------------------------*/
	/*---------------------------*/
	/* 分支选择 */


	/* -------- 拉起帮助手册
	 * -------- */
	if  (options.contains("help")) {
		print_help()
		return 0
	}

	/* -------- 启动一个测试分支
	 * -------- */
	else if (options.contains("test")) {
		run_test(options["test"])
		return 0
	}

	/* -------- 缺省行为：从标准输入获取动作脚本
	 * --------
	 * -------- TODO(20260519-qing): 进行脚本引入（相对路径也可，从**环境变量**的路径中搜寻也可）  */
	else {
		let consoleReader = getStdIn()
		let robot = RobotStruct(consoleReader)
		try {
			robot.active()
			robot.run()
		}
		finally {
			robot.deactive()
		}

		/* ---- NOTE(20260523-qing): 储存微调网络模型
		 * ---- */
//		let consoleReader = getStdIn()
//		run_action_drl(consoleReader, nns)
//		if (options.contains("o")) {
//			save_nns(nns, options['o'])  }
	}


	return 0
}

/* -------- 打印帮助手册
 * -------- */
func print_help()
{
	println("这是一个机器人动作脚本解析器。")
	println("使用管道输入动作脚本，示例：")
	println("cat std.mt | ./bot")
	println()
	println("--i2c=/path/to/device\t选择i2c设备")
	println()
	println("-i  选择输入的模型的路径")
	println("-o  设置输出的模型的路径")
}

/* -------- 集成多种常规测试、临时测试
 * -------- */
func run_test(key: String) {

	/* ----------- 神经网络与门测试
	 * -----------
	 * ----------- INFO: 我们使用随机生成的动态数据集，这样会导致一些后果 */
	if (key == "nn") {
		let nn_t = ArrayList<Nn1>()
		nn_t.add(Nn1(2, 4, 0.05f32, Af.Leaky_ReLU(FP32(0.01))))
		nn_t.add(Nn1(4, 1, 0.05f32, Af.Sigmoid))
		let batch = 1
		let num =2000
		let prt_num = 100
		for (i in 0..(num*batch)) {
			let m = Matrix1(1, 2)
			let t = Matrix1(1, 1)
			prepare_andDoor(m, t)
			let l = train1(nn_t, m, t, 1.0f32) // 这个1.0是学习率的折扣，而非学习率本身
			if (i % prt_num == prt_num-1) {
				print("${i}\t:")
				print("${l}")
				println()
			}
		}
	}


	else if (key == "cnn") {
		testCNN()
	}

	else if (key == "cnn1") {
		testGeometryCNN()
	}


	/* -------- 测试解析矩阵，并且输出
	 * -------- */
	else if (key == "matrix_o") {
		let file = File("ss/test.mt", OpenMode.Read)
		let robot = RobotStruct(file)
		file.close()
		let outf = File("ss/output.mt", OpenMode.Write)
		robot.save(outf)
		outf.close()
	}

	/* -------- 解析测试输出的矩阵
	 * -------- */
	else if (key == "matrix_2") {
		let file = File("ss/output.mt", OpenMode.Read)
		let robot = RobotStruct(file)
		file.close()
		robot.active()
		robot.run()
		robot.deactive()
		let outf = File("ss/output_1.mt", OpenMode.Write)
		robot.save(outf)
		outf.close()
	}

}
