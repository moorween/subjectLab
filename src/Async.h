#ifndef Async_H
#define Async_H

// Async — простые таймеры: delay (один раз), repeat (периодически).
// tick() нужно вызывать в цикле. Не потокобезопасен: все вызовы — из одной задачи
// (для CoreLoop::async это задача цикла).
//
// Внутри колбэка можно безопасно: отменять свою и чужие задачи (breakFn),
// вызывать clearAll(), добавлять новые задачи. Новые задачи выполнятся не раньше
// следующего tick().

#include <functional>
#include <list>
#include <memory>
#include <cstdint>
#include <cstddef>

class CallbackParams {
  public:
    double iterationTime = 0;   // сколько мс прошло с прошлого запуска
    long exeTime = 0;
    long loopCount = 0;         // номер запуска, с 1
    std::function<void()> breakFn;   // отменить эту задачу
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

    // Выполнить колбэк сейчас (у копии из delay() — выполнит и отменит задачу в Async)
    void run();
    void run(unsigned long);

    // Управление задачей, которая хранится в Async. Работают и у копии, которую вернули
    // delay()/repeat(); после отмены задачи или удаления Async ничего не делают.
    std::function<Job(std::function<bool(CallbackParams)>)> untilFn;  // остановить, когда fn вернёт true
    std::function<Job(std::function<bool(CallbackParams)>)> whileFn;  // выполнять, пока fn возвращает true
    std::function<Job()> pause;
    std::function<Job()> resume;
    std::function<void()> breakFn;                                    // отменить

    long delay = 0;
    long loopsCount = 0;
    unsigned long mls = 0;
    unsigned long executionTime = 0;
    bool _active = true;

  private:
    std::list<std::function<bool(CallbackParams)>> _untilFns;
    std::list<std::function<bool(CallbackParams)>> _whileFns;
    callback _cbFn;
};

class Async {
  public:
    Async();
    Async(const Async &) = delete;
    Async &operator=(const Async &) = delete;

    void tick();
    void clearAll();

    Job delay(int delay, std::function<void()>);       // один раз через delay мс
    Job repeat(int delay, std::function<void()>);      // каждые delay мс, первый раз через delay мс
    Job repeat(int delay, callback cb);                // каждые delay мс, первый раз на ближайшем tick()
    void queue(std::function<void()> cb);              // один раз на ближайшем tick()

    size_t size() const;                               // сколько задач активно

  private:
    struct Entry {
      uint32_t id;
      Job job;
      bool removed;
    };

    Job put(long, unsigned long, unsigned long, callback);
    Entry *find(uint32_t id);
    void remove(uint32_t id);
    void purge();

    std::list<Entry> _jobs;
    uint32_t _nextId = 1;
    int _ticking = 0;                                   // >0 — идёт tick(): удаление откладывается
    std::shared_ptr<int> _alive = std::make_shared<int>(0);   // для Job, переживших Async
};

#endif // Async_H
