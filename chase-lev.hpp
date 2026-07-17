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
    static constexpr int empty=0;
    static constexpr int abort=-1;

    ChaseLevDeque()
        : arrays(64,nullptr),bottoms(64,0),log_capacity(initial_log_size) {
            curr= new CircularArray(initial_log_size);
            arrays[initial_log_size]=curr;
        }

    void pushBottom(int task){
        size_t b= bottom.load();
        size_t t=top.load();

        long long size = b-t;

        if(size>=curr->size()-1){
            CircularArray * a = curr->grow(b,t);
            log_capacity++;
            curr=a;
            arrays[log_capacity]=curr;
        }

        curr->put(b,task); 
        bottom.store(b+1);
    }

    void steal(int& task){

        size_t t=top.load();
        size_t b= bottom.load();

        int size=b-t;

        task=curr->get(t);
        
        if(size>0){
            if(!top.compare_exchange_strong(t,t+1)){
                task=abort;
            }
            return;
        }
        if(size<=0){
            task=empty;
        }
    }

    void popBottom(int& task){
        size_t b=bottom.load();

        b--; bottom.store(b);

        size_t t=top.load();

        long long size = b-t;
        task=curr->get(b);
        if(size<0){
            bottom.store(t);
            task=empty; return;
        }
        if(size==0){
            if(!top.compare_exchange_strong(t,t+1)){
                bottom.store(b+1);
                task=abort;
            }
            bottom.store(b+1);
        }
        return;
    }

};