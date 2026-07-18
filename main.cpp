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
    ChaseLevDeque dq;

    const int BURSTS = 20;
    const int PUSH_PER_BURST = 2000;
    const int DRAIN_PER_BURST = 1900;
    const int TASKS = BURSTS * PUSH_PER_BURST;

    vector<int> results;
    mutex mtx;

    atomic<bool> done(false);

    vector<thread> thieves;

    for(int i = 0; i < 4; i++)
    {
        thieves.emplace_back([&]()
        {
            int task = 0;
            int idle = 0;

            while(true)
            {
                dq.steal(task);

                if(task >= 1)
                {
                    idle = 0;

                    lock_guard<mutex> lock(mtx);
                    results.push_back(task);
                }
                else
                {
                    idle++;

                    if(done.load() && idle > 1000)
                        break;

                    this_thread::yield();
                }
            }
        });
    }

    thread owner([&]()
    {
        int task = 1;

        auto start = chrono::high_resolution_clock::now();

        for(int burst = 0; burst < BURSTS; burst++)
        {
            for(int i = 0; i < PUSH_PER_BURST; i++)
            {
                dq.pushBottom(task);
                task++;
            }

            for(int i = 0; i < DRAIN_PER_BURST; i++)
            {
                int t = 0;
                dq.popBottom(t);

                if(t >= 1)
                {
                    lock_guard<mutex> lock(mtx);
                    results.push_back(t);
                }
            }

            cout << "burst " << burst
                 << " array_size=" << dq.getCurrentSize()
                 << " grows=" << dq.growCount
                 << " shrinks=" << dq.shrinkCount << "\n";
        }

        auto end = chrono::high_resolution_clock::now();
        auto us = chrono::duration_cast<chrono::microseconds>(end - start).count();
        cout << "Owner loop took " << us << " us (" << (double)us / TASKS << " us/task)\n";

        done.store(true);
    });

    owner.join();

    for(auto &t : thieves)
        t.join();

    cout << "received: " << results.size() << "\n";

    set<int> unique(results.begin(), results.end());

    cout << "unique: " << unique.size() << "\n";

    if(unique.size() == results.size())
        cout << "No duplicates\n";
    else
        cout << "Duplicates found\n";

    vector<int> missing;

    for(int i = 1; i <= TASKS; i++)
    {
        if(unique.find(i) == unique.end())
            missing.push_back(i);
    }

    cout << "missing count: " << missing.size() << "\n";

    for(int i = 0; i < min((int)missing.size(), 20); i++)
        cout << missing[i] << " ";

    cout << "\n";
}