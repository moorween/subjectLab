#ifndef Async_H
#define Async_H
#include <functional>
void once(int delay, void (*callback)(double));

#include <map>
#include <list>
// #include <multiset>

class CallbackParams {
  public:
    double iterationTime;
    long exeTime;
    long loopCount;
    std::function<void()> breakFn;
    std::function<void()> cbFn;
};

using callback = std::function<void(CallbackParams)>;

class Job {
  public:
    Job();
    Job(long, unsigned long, unsigned long, callback);
    Job attachUntilFn(std::function<bool(CallbackParams)>);
    Job attachWhileFn(std::function<bool(CallbackParams)>);
    Job setActive(bool status);
    void run();
    void run(unsigned long);
    std::function<Job(std::function<bool(CallbackParams)>)> untilFn;
    std::function<Job(std::function<bool(CallbackParams)>)> whileFn;
    std::function<Job()> pause;
    // std::function<Job()> resumeFn;
    std::function<void()> breakFn;

    long delay;
    long loopsCount;
    unsigned long mls;
    unsigned long executionTime;
    bool _active = true;
  private:
    std::list <std::function<bool(CallbackParams)>> _untilFns;
    std::list <std::function<bool(CallbackParams)>> _whileFns;
    callback _cbFn;
};

class Async {
  public:
    Async();
    void tick();
    void clearAll();
    Job delay(int delay, std::function<void()>);
    Job repeat(int delay, std::function<void()>);
    Job repeat(int delay, callback cb);
    
    void queue(std::function<void()> cb);
  private:
    std::multimap <int, Job> _jobs;
    // std::list <int, std::function<void()>> _queue;
    Job put(long, unsigned long, unsigned long, callback);
};

#endif // Async_H
