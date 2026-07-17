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

    const int TASKS = 10000;

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
        for(int i = 1; i <= TASKS; i++)
        {
            dq.pushBottom(i);

            if(i % 3 == 0)
            {
                int task = 0;
                dq.popBottom(task);

                if(task >= 1)
                {
                    lock_guard<mutex> lock(mtx);
                    results.push_back(task);
                }
            }
        }

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