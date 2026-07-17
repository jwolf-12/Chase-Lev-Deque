#pragma once

#include <vector>
using namespace std;

class CircularArray{
private:
    size_t log_size;
    vector<int> segment;

public:
    CircularArray(size_t log_size)
        : log_size(log_size), segment(1ULL <<log_size){}

    size_t size() const{
        return 1ULL << log_size;
    }

    int get(size_t i) const{
        return segment[i%size()];
    }

    void put(size_t i, int value){
        segment[i%size()]=value;
    }

    size_t getLogSize() const{
        return log_size;
    }

    CircularArray* grow(size_t bottom, size_t top) const{
        CircularArray * larger = new CircularArray(log_size+1);
        for(size_t i=top;i<bottom;i++){
            larger->put(i,get(i));
        }
        return larger;
    }

    void copyinto(size_t start, size_t end, CircularArray * smaller){
        for(size_t i=start;i<end;i++){
            smaller->put(i,get(i));
        }
        return ;
    }
};
