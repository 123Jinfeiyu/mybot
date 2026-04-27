/*

 ___col___
/         \
+---------+ \
| + + + + | | row - 第一维度
| + + + + | |
+---------+ /

*/

#pragma once
#include <cmath>
#include <vector>
#include <iostream>
#include <stdexcept>
#include <cstdint>
namespace qing {
template <class T>	
class Matrx {  
public:
    Matrx();
    Matrx(const uint64_t row, const uint64_t col);
    Matrx(const uint64_t row, const uint64_t col, const std::vector<T>& src);
    const T& get(uint64_t row, uint64_t col) const;
    T& witch(uint64_t row, uint64_t col);
    const std::vector<T>& get_data() const;
    const uint64_t get_col() const;
    const uint64_t get_row() const;
    bool has_same_shape(const Matrx<T>& other);
    bool has_dot_shape(const Matrx<T>& other);
    void assert_same_shape(const Matrx<T>& other);
    void assert_dot_shape(const Matrx<T>& other);
    Matrx<T> operator+(const Matrx<T>& other);
    Matrx<T> operator+(T& item);
    Matrx<T> operator-(const Matrx<T>& other);
    Matrx<T> operator-(T& item);
    Matrx<T> operator*(const Matrx<T>& other);
    Matrx<T> operator*(T& item);
    Matrx<T> operator/(T& item);
    Matrx<T> operator/(const Matrx<T>& other);
    Matrx<T> dot(const Matrx<T>& other);

    void save(std::ostream& out) {
        out << row << " " << col;
        for (const auto& item : arr) {
            out << " ";
            out << item;
        }
    }

private:
    uint64_t row, col;
    std::vector<T> arr;
};
}