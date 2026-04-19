#include <random>
#include <algorithm>
#include "Matrix.hh"

namespace qing {

	template <typename T>
	Matrx<T>::Matrx(): row(0), col(0) {}

	template <typename T>
	Matrx<T>::Matrx(const uint64_t row, const uint64_t col)
	: row(row), col(col) {  /* 该构造函数用于从头开始构造 */
		arr = std::vector<T>(col * row);
	}

	template<typename T>
	Matrx<T>::Matrx(const uint64_t row, const uint64_t col, const std::vector<T>& src)
	: row(row), col(col) {  /* 该构造函数用于通过已有的vector进行构造 */
		arr = src;
	}

	template<typename T>
	const T& Matrx<T>::get(uint64_t row, uint64_t col) const {  /* 访问器 - 访问矩阵元素 */
		return arr[col * this->row + row];
	}

	template<typename T>
	T& Matrx<T>::witch(uint64_t row, uint64_t col) {  /* 访问器 - 访问矩阵元素 */
		return arr[col * this->row + row];
	}

	template<typename T>
	const std::vector<T>& Matrx<T>::get_data() const{  /* 访问器 - 用于获取原始数据 */
		return arr;
	}

	template<typename T>
	const uint64_t Matrx<T>::get_col() const {  /* 获取列数 */
		return col;
	}

	template<typename T>
	const uint64_t Matrx<T>::get_row() const {  /* 获取行数 */
		return row;
	}

	template<typename T>
	bool Matrx<T>::has_same_shape(const Matrx<T>& other) {  /* 两个矩阵是否具有相同形状 */
		return other.get_col() == this->get_col() && other.get_row() == this->get_row();
	}

	template<typename T>
	bool Matrx<T>::has_dot_shape(const Matrx<T>& other) { /* 具有矩阵乘法的形状 */
		return this->get_col() == other.get_row();
	}

	template<typename T>
	void Matrx<T>::assert_same_shape(const Matrx<T>& other) { /* 断言两个矩阵具有相同形状 */
		if (!has_same_shape(other))
			throw std::invalid_argument("The shapes of matrix are not same.");   
	}

	template<typename T>
	void Matrx<T>::assert_dot_shape(const Matrx<T>& other) { /* 断言两个矩阵具有点积前置形状 */
		if (!has_dot_shape(other)) {
			throw std::invalid_argument("The shapes of matrix can not be dot.");   
		}
	}

	template<typename T>
	Matrx<T> Matrx<T>::operator+(const Matrx<T>& other) {  /* 加法操作符重定义 - 矩阵加法 */
		//assert_same_shape(other);  /* 检查两个矩阵形状是否一致 */
		std::vector<T> v0  = this->get_data();
		std::vector<T> vec = other.get_data();
		for (auto it = v0.begin(), it_or = vec.begin(); it != v0.end(); ++it, ++it_or )
			*it += *it_or;
		return Matrx<T>{this->get_row(), this->get_col(), v0};
	}

	template<typename T>
	Matrx<T> Matrx<T>::operator+(T& item) {  /* 矩阵加法广播 */
		auto len = this->get_data().size();
		auto v0 = std::vector<T> (len, item);
		auto matrix = (*this) + Matrx(get_row(), get_col(), v0);

		//auto matrix = *this;
		//for (auto it = arr.begin(); it != arr.end(); ++it)
		//	*it += item;
		return matrix;
	}

	template<typename T>
	Matrx<T> Matrx<T>::operator-(const Matrx<T>& other) {  /* 操作符重载 - 矩阵减法 */
		//assert_same_shape(other);  /* 检查两个矩阵形状是否一致 */
		std::vector<T> v0 = this->get_data();
		std::vector <T> vec = other.get_data();
		for (auto it = v0.begin(), it_or = vec.begin(); it != v0.end(); ++it, ++it_or )
			*it -= *it_or;
		return Matrx{this->get_row(), this->get_col(), v0};
	}

	template<typename T>
	Matrx<T> Matrx<T>::operator-(T& item) {  /* 操作符重载 - 矩阵减法广播 */
		auto len = this->get_data().size();
		auto v0 = std::vector<T> (len, item);
		auto matrix = (*this) - Matrx(get_row(), get_col(), v0);

		//Matrx<T> matrix = *this;
		//for (auto it = arr.begin(); it != arr.end(); ++it)
		//	*it -= item;
		return matrix;
	}

	template<typename T>
	const Matrx<T> Matrx<T>::operator*(const Matrx<T>& other) {  /* 操作符重载 - 矩阵乘法 */ 
		//assert_dot_shape(other); /* 它们的形状是否能够点积 */
		auto matrix = Matrx<T>(this->get_row(), other.get_col());
		for (uint64_t i=0; i<this->get_row(); ++i) /* 点积运算 */
 			for (uint64_t j=0; j<other.get_col(); ++j) {
				T sum{0};
				for (uint64_t k=0; k<this->get_col(); ++k)
					sum += this->get(i, k) * other.get(k, j);
				matrix.witch(i, j) = sum;
			}
		return matrix;
	}

	template<typename T>
	Matrx<T> Matrx<T>::operator*(T& item) {  /* 操作符重载 - 矩阵乘法广播 */
		std::vector<T> vec  = this->get_data();
		for (auto it = vec.begin(); it != vec.end(); ++it)
			*it *= item;
		return Matrx<T>{this->get_row(), this->get_col(), vec};
	}

	template<typename T>
	Matrx<T> Matrx<T>::operator/(T& item) {  /* 操作符重载 - 矩阵除法广播 */
		std::vector<T> vec = this->get_data();
		for (auto it = vec.begin(); it != vec.end(); ++it) 
			*it /= item;
		return Matrx<T>(this->get_row(), this->get_col(), vec);
	}

	template class Matrx<float>; /* 显式实例化 */
	template class Matrx<double>;

}

/* FIXME: 形状不合法时返回空指针 */
using namespace qing;
extern "C" {
	void* createMatrix(uint64_t row, uint64_t col) {
		return (void *) new Matrx<float>(row, col);
	}

	void freeMatrix(void* M) {
		delete (Matrx<float>*) M;
	}

	float* witchMatrix(void *m, uint64_t row, uint64_t col) {
		Matrx<float> *M = (Matrx<float>*) m;
		return &(M->witch(row, col));
	}

	uint64_t getrowMatrix(void *m) {
		Matrx<float> *M = (Matrx<float>*) m;
		return M->get_row();
	}

	uint64_t getcolMatrix(void *m) {
		Matrx<float> *M = (Matrx<float>*) m;
		return M->get_col();
	}

	bool sameShapeMatrix(void *m, void *s) {
		Matrx<float> *M = (Matrx<float>*) m;
		Matrx<float> *S = (Matrx<float>*) s;
		return M->has_same_shape(*S);
	}

	bool dotShapeMatrix(void *m, void *s) {
		Matrx<float> *M = (Matrx<float>*) m;
		Matrx<float> *S = (Matrx<float>*) s;
		return M->has_dot_shape(*S);
	}
	
	void* plusMatrix(void *m, void *s) {
		Matrx<float> *M = (Matrx<float>*) m;
		Matrx<float> *S = (Matrx<float>*) s;
		auto a = new Matrx<float>((*M) + (*S));
		return (void *) a;
	}

	void* plusBroadcastMatrix(void *m, float t) {
		Matrx<float> *M = (Matrx<float>*) m;
		auto a = new Matrx<float> ((*M) + t);
		return (void*) a;
	}

	void* subMatrix(void *m, void *s) {
		Matrx<float> *M = (Matrx<float>*) m;
		Matrx<float> *S = (Matrx<float>*) s;
		auto a = new Matrx<float>((*M) - (*S));
		return (void *) a;
	}

	void * subBroadcastMatrix(void *m, float t) {
		Matrx<float> *M = (Matrx<float> *) m;
		auto a  = new Matrx<float> ((*M) - t);
		return (void *) a;
	}

	void* multiBroadcastMatrix(void *m, float t) {
		Matrx<float> *M = (Matrx<float> *) m;
		auto a = new Matrx<float>((*M) * t);
		return (void*) a;
	}

	void* divBroadcastMatrix(void *m, float t) {
		Matrx<float> *M = (Matrx<float> *) m;
		auto a = new Matrx<float>((*M) / t);
		return (void*) a;
	}

	void* dotMatrix(void *m, void *s) {
		Matrx<float> *M = (Matrx<float>*) m;
		Matrx<float> *S = (Matrx<float>*) s;
		auto a = new Matrx<float>((*M) * (*S));
		return (void *) a;
	}

	void* transposeMatrix(void *m) {
		Matrx<float> *M = (Matrx<float> *)m;
		auto N= new Matrx<float>(M->get_col(), M->get_row());
		for (uint64_t row = 0; row< N->get_row(); row++) {
			for (uint64_t col = 0; col < N->get_col(); col++) {
				N->witch(row, col)  = M->get(col, row);
			}
		}
		return (void *)N;
	}

	/* 正态分布Xavier初始化 */
	void* xavierNormalInit(uint64_t fan_in, uint64_t fan_out)
	{
		/* 构建空数组 */
		std::vector<float> weights(fan_in * fan_out);

		/* 高质量的随机数生成器 */
		static std::mt19937 generator(std::random_device{}());

		/* 计算标准差 */
		float sigma = std::sqrt(2.0f / (fan_in + fan_out));

		/* 创建正态分布对象，均值为0，标准差为sigma */
		std::normal_distribution<float> distribution(0.0f, sigma);

		/* 遍历权重矩阵，进行赋值 */
		uint64_t k=0;
		for (uint64_t i=0; i<fan_in; ++i)
			for (uint64_t j=0; j<fan_out; ++j)
				weights[k++] = distribution(generator);

		/* 构建矩阵 */
		auto matrix = new Matrx<float>(fan_in, fan_out, weights);
		return (void *)matrix;
	}

	/* 均匀分布Xavier初始化函数 */
	void* xavierUniformInit(uint64_t fan_in, uint64_t fan_out)
	{	
		/* 构建空的数组 */
		auto weights = std::vector<float>(fan_in * fan_out);

		/* 使用高质量的随机数生成器 */
		static std::mt19937 generator(std::random_device{}());

		/* 计算区间范围：sqrt(2 / (fan_in + fan_out)) */
		float scale = std::sqrt(2.0f / (fan_in + fan_out));

		/* 创建均匀分布对象，范围为[-scale, scale] */
		std::uniform_real_distribution<float> distribution(-scale, scale);

		/* 遍历权重矩阵，进行赋值 */
		uint64_t k=0;
		for (uint64_t i=0; i<fan_in; ++i)
			for (uint64_t j=0; j<fan_out; ++j)
				weights[k++] = distribution(generator);

		/* 构建矩阵对象 */
		auto matrix = new Matrx<float> (fan_in, fan_out, weights);
		return (void *)matrix;
	}

	void* Sigmoid_do(void *m) {
		Matrx<float> *M = (Matrx<float>*) m;
		std::vector<float> v = M->get_data();
		for (float& x: v) {
			x = 1.0 / (1.0 + std::exp(-x));
		}
		auto N = new Matrx<float> (M->get_row(), M->get_col(), v);
		return (void *)N;
	}

	void* Sigmoid_dot(void *m) {
		Matrx<float> *M = (Matrx<float>*) m;
		std::vector<float> v = M->get_data();
		for (float& o: v) {
			o = o * (1.0 - o);
		}
		auto N = new Matrx<float> (M->get_row(), M->get_col(), v);
		return (void *)N;
	}

	void* ReLU_do(void *m) {
		Matrx<float> *M = (Matrx<float>*) m;
		std::vector<float> v = M->get_data();
		for (float& x: v) {
			x = x > 0 ? x : 0;
		}
		auto N = new Matrx<float> (M->get_row(), M->get_col(), v);
		return (void *)N;
	}

	void* ReLU_dot(void *m) {
		Matrx<float> *M = (Matrx<float>*) m;
		std::vector<float> v = M->get_data();
		for (float& x: v) {
			x =  x > 0 ? 1.0f : 0.0f;
		}
		auto N = new Matrx<float> (M->get_row(), M->get_col(), v);
		return (void *)N;
	}


	void* Leaky_ReLU_do(void *m, float alpha) {
		Matrx<float> *M = (Matrx<float>*) m;
		std::vector<float> v = M->get_data();
		for (float& x: v) {
			x =  x > 0 ? x : alpha * x;
		}
		auto N = new Matrx<float> (M->get_row(), M->get_col(), v);
		return (void *)N;
	}

	void* Leaky_ReLU_dot(void *m, float alpha) {
		Matrx<float> *M = (Matrx<float>*) m;
		std::vector<float> v = M->get_data();
		for (float& x: v) {
			x = x > 0 ? 1.0 : alpha;
		}
		auto N = new Matrx<float> (M->get_row(), M->get_col(), v);
		return (void *)N;
	}
}
