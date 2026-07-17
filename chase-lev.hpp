#pragma once

#include <atomic>
#include <vector>
#include <stdexcept>
#include "circulararray.hpp"

using namespace std;

class ChaseLevDeque{
private:
    static constexpr size_t initial_log_size=0;

    atomic<size_t> bottom=0;
    atomic<size_t> top=0;

    vector<CircularArray *> arrays;
    vector<size_t> bottoms;

    CircularArray* curr;
    size_t log_capacity;

public:

    ChaseLevDeque()
        : arrays(64,nullptr),bottoms(64,0),log_capacity(initial_log_size) {
            curr= new CircularArray(initial_log_size);
            arrays[initial_log_size]=curr;
        }

    void pushBottom(int task){
        size_t b= bottom.load();
        size_t t=top.load();

        CircularArray* a = curr;

        long long size = b-t;

        if(size>=a->size()-1){
            a = curr->grow(b,t);
            log_capacity++;
            curr=a;
            arrays[log_capacity]=curr;
        }

        a->put(b,task); 
        bottom.store(b+1);
    }

};