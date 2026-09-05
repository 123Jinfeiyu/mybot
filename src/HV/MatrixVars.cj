/*
 *  硬件变量基类（自 r.cj 迁出）
 *  接收3个lambda表达式，在回调函数中定义行为（）
 *  通过JsonValue这种通用的数据结构进行传值
 *
 *  INFO*(20260517-qing): 因为析构器（终结器）的调用时间不确定，需要手动构造RAII机制
 *  TODO(20260519-qing): 考虑将@Action矩阵对象置为硬件变量的一种
 *  TODO*(20260529-qing): 增加微调器机制，通过不同的微调器对@Action进行微调
 */

package bot4cj.HV
import std.convert.*
import stdx.encoding.json.*
import mt2.*

public open class MatrixVars_Hardware {

	/*  ---------------- 主初始化器 ----------------
	 *   输入三个回调函数，用json参数能够为多种硬件提供同一接口
	 *   在@Action矩阵中的每一列都是一个输入参数
	 *  --------------------------------------------  */
	public init(
	things!: (JsonValue) -> JsonValue,	/* 运行硬件或传感器 */
	rise!: Option<() -> Unit>,		/* 可选的硬件构造函数 */
	free!: Option<() -> Unit>) {		/* 可选的硬件析构函数 */

		this.things = things
		this.rise   = rise
		this.free   = free
	}


	/*
	 *  --------------------------------
	 *  事件回调，接收和返回JsonValue 这是整个硬件的主要事件
	 */
	private let things: (JsonValue) -> JsonValue

	/* 召集事件，不接受参数  即硬件获取资源  */
	private let rise: Option<() -> Unit>

	/* 遣返事件，不接受参数  即硬件释放资源*/
	private let free: Option<() -> Unit>

	/*
	 *  -------------------------------- *
	 *  事件发生器  该方法会调用硬件，需要传入1个JsonValue参数  它返回1个JsonValue值
	 *
	 *  TODO(20260612-qing): 使用某种方式记录变量是否经过了初始化
	 */
	public func play(input: JsonValue): JsonValue {  return things(input)  }

	/*
	 *  -------------------------------- *
	 *  伪初始化器  该方法会打开硬件，什么也不传入和返回
	 */

	public func build(): Unit {

		if (let Some(callback) <- rise) {  callback()  }
	}

	/*
	 *  -------------------------------- *
	 *  伪解构器   该方法会关闭硬件，什么也不传入和返回
	 */
	public func destroy(): Unit {

		if (let Some(callback) <- free) {  callback()  }
	}

	/*
	 *  ----------------------------------
	 *  获得项目值。如果矩阵单元的原始值是占位符且不为标题行，
	 *  不为首行， 就递归地获取上一行的值
	 */
	public static func getMt(obj: MatrixObject, row: Int64, col: Int64): String
	{
		let raw = obj[row, col]
		if (raw == "-" && row > 1) {
			return getMt(obj, row - 1, col)
		}
		return raw
	}

	/*
	 * ----------------------------------
	 * 解析项目值，先获取矩阵单元的项目值————解析过占位符的值）————然后解析为需要的类型
	 */
	public static func parseMt<T>(obj: MatrixObject, row: Int64, col: Int64): T  where T <: Parsable<T>
	{
		/* 先获取矩阵单元的原始值 */
		let raw = obj[row, col]

		/* 如果原始值为占位符且（非首行的）非标题行 */
		if      (raw == "-" &&   row > 1)  { return  parseMt<T>(obj, row-1, col)  }

		/* 原始值为占位符且为（非标题行的）首行 */
		else if (raw == "-" && !(row > 1)) {  return T.parse("0")  }

		/* 原始值并非占位符  */
		return T.parse(raw)
	}



	/* ---------------------------------- */
}
