#pragma once

#include <atomic>
#include <vector>
#include <stdexcept>
#include "circulararray.hpp"

#include <iostream>

using namespace std;

class ChaseLevDeque{
private:
    static constexpr size_t initial_log_size=4;

    static constexpr size_t K=12;

    atomic<size_t> bottom=0;
    atomic<size_t> top=0;

    vector<CircularArray *> arrays;
    vector<size_t> bottoms;

    atomic<CircularArray*> curr;
    size_t log_capacity;

    size_t lastTop=0;

    void perhapsShrink(size_t b,size_t t){
        if(log_capacity>initial_log_size && b-t<curr.load(memory_order_acquire)->size()/K){
            shrinkCount++;
            curr.load(memory_order_acquire)->copyinto(bottoms[log_capacity-1],b,arrays[log_capacity-1]);
            log_capacity--;
            curr.store(arrays[log_capacity],memory_order_release);
        }
    }

public:
    static constexpr int empty=0;
    static constexpr int abort=-1;

    size_t getCurrentSize() const { return curr.load(memory_order_acquire)->size(); }
    long growCount = 0;
    long shrinkCount = 0;

    ChaseLevDeque()
        : arrays(64,nullptr),bottoms(64,0),log_capacity(initial_log_size) {
            curr.store( new CircularArray(initial_log_size),memory_order_release);
            arrays[initial_log_size]=curr.load(memory_order_acquire);
        }

    void pushBottom(int task){
        size_t b= bottom.load(memory_order_relaxed);
        CircularArray* a = curr.load(memory_order_acquire);

        long long size = b-lastTop;

        if(size>=(a->size()-1)){
            size_t t = top.load(memory_order_acquire);
            lastTop=t;
            size=b-t;
            if(size>=(a->size()-1)){
                bottoms[log_capacity]=b;
                growCount++;
                CircularArray * newa = a->grow(b,lastTop);
                log_capacity++;
                curr.store(newa,memory_order_release);
                a=newa;
                arrays[log_capacity]=a;
            }
        }

        a->put(b,task); 
        atomic_thread_fence(memory_order_release);
        bottom.store(b+1,memory_order_release);
    }

    void steal(int& task){

        size_t t=top.load(memory_order_acquire);
        atomic_thread_fence(memory_order_seq_cst);
        size_t b= bottom.load(memory_order_acquire);

        int size=b-t;

        task=curr.load(memory_order_acquire)->get(t);
        
        if(size>0){
            if(!top.compare_exchange_strong(t,t+1,memory_order_seq_cst)){
                task=abort;
            }
            return;
        }
        if(size<=0){
            task=empty;
        }
    }

    void popBottom(int& task){
        size_t b=bottom.load(memory_order_relaxed);

        b--; bottom.store(b,memory_order_relaxed);
        atomic_thread_fence(memory_order_seq_cst);
        task=curr.load(memory_order_acquire)->get(b);

        size_t t=top.load(memory_order_acquire); 
        lastTop=t;

        long long size = b-t;

        if(size<0){
            bottom.store(t,memory_order_release);
            task=empty; return;
        }
        if(size==0){
            if(!top.compare_exchange_strong(t,t+1,memory_order_seq_cst)){
                bottom.store(b+1,memory_order_release);
                task=abort;
            }
            bottom.store(b+1,memory_order_release);
            lastTop=t+1;
            return;
        }
        perhapsShrink(b,t);
        return;
    }

};