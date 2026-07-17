#pragma once

#include <atomic>
#include <vector>
#include <stdexcept>
#include "circulararray.hpp"

using namespace std;

class ChaseLevADeque{
private:
    final static size_t initial_log_size=0;

    atomic<size_t> bottom=0;
    atomic<size_t> top=0;

    vector<CircularArray *> arrays(64,nullptr);
    vector<size_t> bottoms(64,0);

    CircularArray a= new CircularArray(initial_log_size);
    ararys[initial_log_size]=&a;

    

};