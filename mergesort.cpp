// Build with: g++ -O3 -march=native -pthread chase-lev-fast.cpp -o sortfast

#include "chase-lev.hpp"

#include <atomic>
#include <vector>
#include <thread>
#include <algorithm>
#include <iostream>
#include <chrono>

using namespace std;

vector<int> arr;
vector<int> scratch;

int NUM_THREADS = 8;
const int CUTOFF = 1024;
const int MERGE_CUTOFF = 512;
const int PARALLEL_MERGE_THRESHOLD = 4096;
const int N = 1000000;

enum TaskType : int { SPLIT, MERGE, MERGE_ROOT };

struct alignas(64) Task {
    int type = SPLIT;
    int start = 0, end = 0;
    int l1 = 0, r1 = 0, l2 = 0, r2 = 0, dest = 0;
    int parent = -1;
    atomic<int> childrenDone{0};
};

const int MAX_TASKS = 1 << 18;

vector<Task> taskTable(MAX_TASKS);
atomic<int> nextTaskId{1};

vector<ChaseLevDeque> deques(16);

atomic<int> tasksRemaining{0};
atomic<bool> done(false);


int makeSplitTask(int l, int r, int parent)
{
    int id = nextTaskId.fetch_add(1, memory_order_relaxed);
    Task &t = taskTable[id];
    t.type = SPLIT;
    t.start = l;
    t.end = r;
    t.parent = parent;
    t.childrenDone.store(0, memory_order_relaxed);
    tasksRemaining.fetch_add(1, memory_order_relaxed);
    return id;
}

int makeMergeTask(int l1, int r1, int l2, int r2, int dest, int parent, TaskType type)
{
    int id = nextTaskId.fetch_add(1, memory_order_relaxed);
    Task &t = taskTable[id];
    t.type = type;
    t.l1 = l1; t.r1 = r1;
    t.l2 = l2; t.r2 = r2;
    t.dest = dest;
    t.parent = parent;
    t.childrenDone.store(0, memory_order_relaxed);
    tasksRemaining.fetch_add(1, memory_order_relaxed);
    return id;
}


void mergeRange(int l, int mid, int r)
{
    std::merge(arr.begin() + l, arr.begin() + mid,
               arr.begin() + mid, arr.begin() + r,
               scratch.begin() + l);
    std::copy(scratch.begin() + l, scratch.begin() + r, arr.begin() + l);
}


void executeTask(int id, int worker);
void executeMergeTask(int id, int worker);


void finishTask(int id, int worker)
{
    while (id != -1)
    {
        tasksRemaining.fetch_sub(1, memory_order_relaxed);

        int parentId = taskTable[id].parent;
        if (parentId == -1)
            return;

        Task &p = taskTable[parentId];

        if (p.childrenDone.fetch_add(1, memory_order_acq_rel) + 1 != 2)
            return;

        if (p.type == SPLIT)
        {
            int mid = (p.start + p.end) / 2;
            int rangeSize = p.end - p.start;

            if (rangeSize > PARALLEL_MERGE_THRESHOLD)
            {
                // p (the SPLIT node) is done — account for it here, since
                // we `return` below instead of looping back to the top.
                int rootMergeId = makeMergeTask(p.start, mid, mid, p.end,
                                                 p.start, p.parent, MERGE_ROOT);
                tasksRemaining.fetch_sub(1, memory_order_relaxed);
                deques[worker].pushBottom(rootMergeId);
                return;
            }
            else
            {
                mergeRange(p.start, mid, p.end);
                id = parentId;
                continue;
            }
        }
        else if (p.type == MERGE_ROOT)
        {
            // The ENTIRE parallel-merge subtree just finished. This is the
            // ONLY point where we copy scratch back into arr for this
            // subtree — doing it here, once, single-threaded, avoids the
            // data race that occurs if individual merge leaves copy back
            // to arr while sibling leaves are still concurrently reading
            // from arr for their own split-point binary searches.
            std::copy(scratch.begin() + p.l1, scratch.begin() + p.r2,
                      arr.begin() + p.l1);
            id = parentId;
            continue;
        }
        else // p.type == MERGE (internal, non-root merge node)
        {
            id = parentId;
            continue;
        }
    }
}


void executeTask(int id, int worker)
{
    while (true)
    {
        Task &t = taskTable[id];

        if (t.end - t.start <= CUTOFF)
        {
            sort(arr.begin() + t.start, arr.begin() + t.end);
            finishTask(id, worker);
            return;
        }

        int mid = (t.start + t.end) / 2;
        int right = makeSplitTask(mid, t.end, id);
        deques[worker].pushBottom(right);
        id = makeSplitTask(t.start, mid, id);
    }
}


void executeMergeTask(int id, int worker)
{
    while (true)
    {
        Task &t = taskTable[id];

        int n1 = t.r1 - t.l1;
        int n2 = t.r2 - t.l2;

        if (n1 + n2 <= MERGE_CUTOFF)
        {
            // Write ONLY to scratch here — never copy back to arr from a
            // leaf. A sibling merge task may still be reading arr for its
            // own binary search at this exact moment.
            std::merge(arr.begin() + t.l1, arr.begin() + t.r1,
                       arr.begin() + t.l2, arr.begin() + t.r2,
                       scratch.begin() + t.dest);
            finishTask(id, worker);
            return;
        }

        int aL, aR, bL, bR;
        bool aIsFirst;

        if (n1 >= n2) { aL = t.l1; aR = t.r1; bL = t.l2; bR = t.r2; aIsFirst = true; }
        else          { aL = t.l2; aR = t.r2; bL = t.l1; bR = t.r1; aIsFirst = false; }

        int aMid = aL + (aR - aL) / 2;
        int splitVal = arr[aMid];
        int bMid = int(std::lower_bound(arr.begin() + bL, arr.begin() + bR, splitVal)
                        - arr.begin());

        int leftCount = (aMid - aL) + (bMid - bL);
        int destSplit = t.dest + leftCount;

        int c1, c2;
        if (aIsFirst)
        {
            c1 = makeMergeTask(aL, aMid, bL, bMid, t.dest, id, MERGE);
            c2 = makeMergeTask(aMid, aR, bMid, bR, destSplit, id, MERGE);
        }
        else
        {
            c1 = makeMergeTask(bL, bMid, aL, aMid, t.dest, id, MERGE);
            c2 = makeMergeTask(bMid, bR, aMid, aR, destSplit, id, MERGE);
        }

        deques[worker].pushBottom(c2);
        id = c1;
    }
}


void runTask(int id, int worker)
{
    Task &t = taskTable[id];
    if (t.type == SPLIT)
        executeTask(id, worker);
    else
        executeMergeTask(id, worker);
}


void workerLoop(int id)
{
    int task;
    int victim = id + 1;

    while (true)
    {
        task = ChaseLevDeque::empty;
        deques[id].popBottom(task);

        if (task <= 0)
        {
            bool found = false;
            for (int i = 0; i < NUM_THREADS - 1; i++)
            {
                victim++;
                if (victim >= NUM_THREADS) victim = 0;
                if (victim == id) continue;

                deques[victim].steal(task);
                if (task > 0) { found = true; break; }
            }

            if (!found)
            {
                if (done.load(memory_order_relaxed) &&
                    tasksRemaining.load(memory_order_relaxed) == 0)
                    break;
                this_thread::yield();
                continue;
            }
        }

        runTask(task, id);
    }
}


int main(int argc,char* argv[])
{
    NUM_THREADS=stoi(argv[1]);

    arr.resize(N);
    scratch.resize(N);

    srand(0);
    for (int i = 0; i < N; i++)
        arr[i] = rand();

    auto start = chrono::high_resolution_clock::now();

    int root = makeSplitTask(0, N, -1);
    deques[0].pushBottom(root);

    vector<thread> workers;
    for (int i = 0; i < NUM_THREADS; i++)
        workers.emplace_back(workerLoop, i);

    while (tasksRemaining.load(memory_order_relaxed) != 0)
        this_thread::yield();

    done.store(true, memory_order_relaxed);

    for (auto &t : workers)
        t.join();

    auto end = chrono::high_resolution_clock::now();

    cout << (is_sorted(arr.begin(), arr.end()) ? "Sorted correctly\n" : "SORT FAILED\n");
    cout << "Chase-Lev time: "
         << chrono::duration_cast<chrono::microseconds>(end - start).count()
         << " us\n";
    cout << "Tasks created: " << nextTaskId.load() - 1 << "\n";
}