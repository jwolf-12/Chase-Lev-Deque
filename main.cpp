#include "chase-lev.hpp"

#include <iostream>
#include <thread>
#include <vector>
#include <mutex>
#include <set>
#include <atomic>
#include <chrono>

using namespace std;

int main()
{
    ChaseLevDeque dq(100000);

    const int TASKS = 10000;

    vector<int> results;
    mutex mtx;

    atomic<bool> done(false);


    // thieves
    vector<thread> thieves;

    for(int i = 0; i < 4; i++)
    {
        thieves.emplace_back([&]()
        {
            int task;

            while(!done.load())
            {
                if(dq.steal(task))
                {
                    lock_guard<mutex> lock(mtx);
                    results.push_back(task);
                }
            }

            while(dq.steal(task))
            {
                lock_guard<mutex> lock(mtx);
                results.push_back(task);
            }
        });
    }


    // owner
    thread owner([&]()
    {
        int task;

        for(int i = 0; i < TASKS; i++)
        {
            dq.pushBottom(i);

            // randomly let owner take work
            if(i % 3 == 0)
            {
                if(dq.popBottom(task))
                {
                    lock_guard<mutex> lock(mtx);
                    results.push_back(task);
                }
            }
        }
    });


    owner.join();

    done.store(true);


    for(auto& t : thieves)
        t.join();


    cout << "received: "
         << results.size()
         << "\n";


    set<int> unique(results.begin(), results.end());

    cout << "unique: "
         << unique.size()
         << "\n";


    if(unique.size() == results.size())
        cout << "No duplicates\n";
    else
        cout << "Duplicates found\n";

    set<int> expected;
    for(int i = 0; i < TASKS; i++) expected.insert(i);

    vector<int> missing;
    for(int x : expected){
        if(unique.find(x) == unique.end()) missing.push_back(x);
    }

    cout << "missing count: " << missing.size() << "\n";
    for(int i = 0; i < min((int)missing.size(), 20); i++){
        cout << missing[i] << " ";
    }
    cout << "\n";
}