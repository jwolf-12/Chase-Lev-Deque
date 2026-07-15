#pragma once

#include <atomic>
#include <vector>
#include <stdexcept>

using namespace std;

class ChaseLevDeque{

private:
    vector<int> buffer;
    atomic<size_t> top;
    atomic<size_t> bottom;

public:
    ChaseLevDeque(size_t capacity){
        buffer=vector<int>(capacity);
        top=0;
        bottom=0;
    }

    bool isempthy(){
        if(bottom.load()==top.load()) return true;
        return false;
    }
    
    void pushBottom(int task){
        size_t b= bottom.load();
        if(b>=buffer.size()){
            throw runtime_error("Deque failed");
        }
        buffer[b]=task;
        bottom.store(b+1);
    }

    bool popBottom(int& task){
        size_t b=bottom.load();
        b--;
        bottom.store(b);
        size_t t=top.load();
        long long size = b-t;
        if(size<0){
            bottom.store(t);
            return false;
        }
        task=buffer[b];
        if(size>0) return true;
        if(!top.compare_exchange_strong(t,t+1)){
            bottom.store(t+1);
            return false;
        }
        bottom.store(t+1);
        return true;
    }

    bool steal(int& task){
        size_t t= top.load();
        size_t b=bottom.load();

        if(t>=b) return false;

        task=buffer[t];

        if(!top.compare_exchange_strong(t,t+1)){
            return false;
        }
        return true;
    }

};