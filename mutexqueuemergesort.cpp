// Build with: g++ -O3 -march=native -pthread mutex-task-merge-v2.cpp -o mutexsort
//
// Memory orders below are deliberately matched to chase-lev-fast.cpp:
// relaxed for bookkeeping counters that don't need to publish anything
// (nextTaskId, tasksRemaining, childrenDone's reset-to-0), acq_rel on the
// childrenDone fetch_add that decides who "wins" the fan-in (needs to both
// see the sibling's writes and publish this thread's own). Without this
// alignment, the mutex version would previously default to seq_cst on all
// of these, which adds cost unrelated to "uses a mutex for scheduling" and
// would bias the comparison.

#include <atomic>
#include <vector>
#include <thread>
#include <algorithm>
#include <iostream>
#include <chrono>
#include <queue>
#include <mutex>
#include <condition_variable>

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

queue<int> taskQueue;
mutex queueMutex;
condition_variable cv;

atomic<int> tasksRemaining{0};
atomic<bool> done(false);


int makeSplitTask(int l, int r, int parent)
{
    int id = nextTaskId.fetch_add(1, memory_order_relaxed);
    Task &t = taskTable[id];
    t.type = SPLIT;
    t.start = l; t.end = r;
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

void pushTask(int id)
{
    { lock_guard<mutex> lock(queueMutex); taskQueue.push(id); }
    cv.notify_one();
}

bool popTask(int &id)
{
    unique_lock<mutex> lock(queueMutex);
    cv.wait(lock, [] {
        return !taskQueue.empty() ||
               (done.load(memory_order_relaxed) &&
                tasksRemaining.load(memory_order_relaxed) == 0);
    });
    if (taskQueue.empty()) return false;
    id = taskQueue.front();
    taskQueue.pop();
    return true;
}

void mergeRange(int l, int mid, int r)
{
    std::merge(arr.begin()+l, arr.begin()+mid, arr.begin()+mid, arr.begin()+r, scratch.begin()+l);
    std::copy(scratch.begin()+l, scratch.begin()+r, arr.begin()+l);
}

void executeTask(int id);
void executeMergeTask(int id);

void finishTask(int id)
{
    while (id != -1)
    {
        tasksRemaining.fetch_sub(1, memory_order_relaxed);
        int parentId = taskTable[id].parent;
        if (parentId == -1) return;

        Task &p = taskTable[parentId];
        if (p.childrenDone.fetch_add(1, memory_order_acq_rel) + 1 != 2) return;

        if (p.type == SPLIT)
        {
            int mid = (p.start + p.end) / 2;
            int rangeSize = p.end - p.start;

            if (rangeSize > PARALLEL_MERGE_THRESHOLD)
            {
                // Increment (inside makeMergeTask) before decrement, so
                // tasksRemaining never transiently dips to a value that
                // could make main's polling loop or a worker's shutdown
                // check observe 0 before this merge task actually exists.
                int rootMergeId = makeMergeTask(p.start, mid, mid, p.end, p.start, p.parent, MERGE_ROOT);
                tasksRemaining.fetch_sub(1, memory_order_relaxed);
                pushTask(rootMergeId);
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
            // Single, safe, one-shot copy-back for the whole subtree.
            std::copy(scratch.begin() + p.l1, scratch.begin() + p.r2, arr.begin() + p.l1);
            id = parentId;
            continue;
        }
        else // p.type == MERGE
        {
            id = parentId;
            continue;
        }
    }
}

void executeTask(int id)
{
    while (true)
    {
        Task &t = taskTable[id];
        if (t.end - t.start <= CUTOFF)
        {
            sort(arr.begin()+t.start, arr.begin()+t.end);
            finishTask(id);
            return;
        }
        int mid = (t.start + t.end) / 2;
        int right = makeSplitTask(mid, t.end, id);
        pushTask(right);
        id = makeSplitTask(t.start, mid, id);
    }
}

void executeMergeTask(int id)
{
    while (true)
    {
        Task &t = taskTable[id];
        int n1 = t.r1 - t.l1, n2 = t.r2 - t.l2;

        if (n1 + n2 <= MERGE_CUTOFF)
        {
            // scratch only — no copy-back at leaf level (see finishTask's
            // MERGE_ROOT branch for why).
            std::merge(arr.begin()+t.l1, arr.begin()+t.r1, arr.begin()+t.l2, arr.begin()+t.r2, scratch.begin()+t.dest);
            finishTask(id);
            return;
        }

        int aL, aR, bL, bR; bool aIsFirst;
        if (n1 >= n2) { aL=t.l1; aR=t.r1; bL=t.l2; bR=t.r2; aIsFirst=true; }
        else          { aL=t.l2; aR=t.r2; bL=t.l1; bR=t.r1; aIsFirst=false; }

        int aMid = aL + (aR-aL)/2;
        int splitVal = arr[aMid];
        int bMid = int(std::lower_bound(arr.begin()+bL, arr.begin()+bR, splitVal) - arr.begin());
        int leftCount = (aMid-aL) + (bMid-bL);
        int destSplit = t.dest + leftCount;

        int c1, c2;
        if (aIsFirst) {
            c1 = makeMergeTask(aL, aMid, bL, bMid, t.dest, id, MERGE);
            c2 = makeMergeTask(aMid, aR, bMid, bR, destSplit, id, MERGE);
        } else {
            c1 = makeMergeTask(bL, bMid, aL, aMid, t.dest, id, MERGE);
            c2 = makeMergeTask(bMid, bR, aMid, aR, destSplit, id, MERGE);
        }

        pushTask(c2);
        id = c1;
    }
}

void runTask(int id)
{
    Task &t = taskTable[id];
    if (t.type == SPLIT) executeTask(id);
    else executeMergeTask(id);
}

void workerLoop()
{
    int id;
    while (popTask(id))
        runTask(id);
}

int main(int argv, char* argc[])
{
    NUM_THREADS=stoi(argc[1]);
    arr.resize(N); scratch.resize(N);
    srand(0);
    for (int i = 0; i < N; i++) arr[i] = rand();

    auto start = chrono::high_resolution_clock::now();

    int root = makeSplitTask(0, N, -1);
    pushTask(root);

    vector<thread> workers;
    for (int i = 0; i < NUM_THREADS; i++) workers.emplace_back(workerLoop);

    while (tasksRemaining.load(memory_order_relaxed) != 0)
        this_thread::yield();

    // done must be set while holding queueMutex, or a worker that just
    // checked its predicate (saw done==false) and is about to enter the
    // blocking part of cv.wait can miss this notify entirely and sleep
    // forever.
    {
        lock_guard<mutex> lock(queueMutex);
        done.store(true, memory_order_relaxed);
    }
    cv.notify_all();

    for (auto &t : workers) t.join();

    auto end = chrono::high_resolution_clock::now();

    cout << (is_sorted(arr.begin(), arr.end()) ? "Sorted correctly\n" : "SORT FAILED\n");
    cout << "Mutex queue time: " << chrono::duration_cast<chrono::microseconds>(end-start).count() << " us\n";
    cout << "Tasks created: " << nextTaskId.load()-1 << "\n";
}