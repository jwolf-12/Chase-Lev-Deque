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

        if(b==0){
            return false;
        }

        b--;

        bottom.store(b);

        size_t t=top.load();

        if(t<=b){
            if(t==b){
                if(!top.compare_exchange_strong(t,t+1)){
                    bottom.store(b+1);
                    return false;
                }
            }
            task=buffer[b];
            return true;

        }

        bottom.store(b+1);
        return false;
    }

    bool steal(int& task){
        size_t t= top.load();
        size_t b=bottom.load();

        if(t>=b) return false;

        if(!top.compare_exchange_strong(t,t+1)){
            return false;
        }
        task=buffer[t];
        return true;
    }

};