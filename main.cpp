#include <iostream>
#include "RingBuffer.hpp"
using namespace buffers;
using namespace  std;
void test1(){
    ring_buffer<std::vector<int>, 3> b1;

    for(auto i = 0; i < 9; ++i){
        b1.push_back(std::vector<int>{i, i + 1, i + 2});
    }

    auto b2 = b1;

    ring_buffer<std::vector<int>, 3> b3;
    b3 = b2;
    b3.pop_front();

    std::for_each(b3.cbegin(), b3.cend(), [](std::vector<int> value) {
        std::cout << value[0] << " " << value[1] << " " << value[2] << std::endl;
    });
}
void test2(){

    ring_buffer<int, 3> b1;
    for(int i=0;i<10;i++){
        b1.push_back(i);
        cout << b1.front()  << endl;
    }
}
int main() {

   test2();

    return 0;
}
