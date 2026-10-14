#include "Async.h"
#include <Arduino.h>
#include <iterator>

// ---------------------------------------------------------------------------
// Job
// ---------------------------------------------------------------------------

Job::Job() {
  this->breakFn = []() {};
  this->whileFn = [](std::function<bool(CallbackParams)>) { return Job(); };
  this->untilFn = [](std::function<bool(CallbackParams)>) { return Job(); };
  this->pause = []() { return Job(); };
  this->resume = []() { return Job(); };
}

Job::Job(long delay, unsigned long mls, unsigned long exeTime, callback cb) : Job()
{
  this->delay = delay;
  this->mls = mls;
  this->executionTime = exeTime;
  this->_cbFn = cb;
}

Job Job::attachUntilFn(std::function<bool(CallbackParams)> untilFn) {
  this->_untilFns.push_back(untilFn);
  return *this;
}

Job Job::attachWhileFn(std::function<bool(CallbackParams)> whileFn) {
  this->_whileFns.push_back(whileFn);
  return *this;
}

Job Job::setActive(bool status) {
  this->_active = status;
  return *this;
}

void Job::run() {
  this->run(millis() - this->mls);
}

void Job::run(unsigned long delayTime) {
  if (!this->_active) return;

  CallbackParams params;
  params.iterationTime = delayTime;
  params.exeTime = this->executionTime;
  params.loopCount = this->loopsCount;

  bool untilPassed = true;
  bool whilePassed = true;
  for (auto &untilFn : this->_untilFns) untilPassed = untilPassed && !untilFn(params);
  for (auto &whileFn : this->_whileFns) whilePassed = whilePassed && whileFn(params);

  if (untilPassed && whilePassed) {
    params.breakFn = this->breakFn;
    if (this->_cbFn) this->_cbFn(params);
  } else if (this->breakFn) {
    this->breakFn();
  }
}

// ---------------------------------------------------------------------------
// Async
// ---------------------------------------------------------------------------

Async::Async() {}

Job Async::put(long delay, unsigned long mls, unsigned long exeTime, callback cb) {
  uint32_t id = _nextId++;
  std::weak_ptr<int> alive = _alive;
  Async *self = this;

  Job job(delay, mls, exeTime, cb);
  job.breakFn = [self, alive, id]() {
    if (!alive.expired()) self->remove(id);
  };
  job.untilFn = [self, alive, id](std::function<bool(CallbackParams)> fn) {
    if (!alive.expired())
      if (Entry *e = self->find(id)) return e->job.attachUntilFn(fn);
    return Job();
  };
  job.whileFn = [self, alive, id](std::function<bool(CallbackParams)> fn) {
    if (!alive.expired())
      if (Entry *e = self->find(id)) return e->job.attachWhileFn(fn);
    return Job();
  };
  job.pause = [self, alive, id]() {
    if (!alive.expired())
      if (Entry *e = self->find(id)) return e->job.setActive(false);
    return Job();
  };
  job.resume = [self, alive, id]() {
    if (!alive.expired())
      if (Entry *e = self->find(id)) return e->job.setActive(true);
    return Job();
  };

  _jobs.push_back(Entry{id, job, false});
  return job;
}

Async::Entry *Async::find(uint32_t id) {
  for (auto &e : _jobs)
    if (e.id == id && !e.removed) return &e;
  return nullptr;
}

void Async::remove(uint32_t id) {
  for (auto it = _jobs.begin(); it != _jobs.end(); ++it) {
    if (it->id != id) continue;
    if (_ticking) it->removed = true;   // идёт обход списка — удалим после tick()
    else _jobs.erase(it);
    return;
  }
}

void Async::purge() {
  _jobs.remove_if([](const Entry &e) { return e.removed; });
}

Job Async::repeat(int delay, callback cb) {
  return this->put(delay, 0, -millis(), cb);
}

Job Async::repeat(int delay, std::function<void()> cb) {
  callback cb2 = [cb](CallbackParams) { cb(); };
  return this->put(delay, millis(), millis(), cb2);
}

Job Async::delay(int delay, std::function<void()> cb) {
  callback cb2 = [cb](CallbackParams params) {
    cb();
    params.breakFn();
  };
  return this->put(delay, millis(), millis(), cb2);
}

void Async::queue(std::function<void()> cb) {
  this->delay(0, cb);
}

void Async::tick() {
  if (_jobs.empty()) return;

  // Обходим только задачи, которые были до начала tick(): добавленные из колбэков
  // выполнятся в следующий раз. Удаление во время обхода только помечает задачу.
  std::weak_ptr<int> alive = _alive;
  auto last = std::prev(_jobs.end());
  _ticking++;
  for (auto it = _jobs.begin();; ++it) {
    bool isLast = (it == last);
    if (!it->removed) {
      Job &job = it->job;
      unsigned long now = millis();
      unsigned long delayTime = now - job.mls;
      if (job.mls == 0 || delayTime >= (unsigned long)job.delay) {
        job.mls = now;
        job.loopsCount++;
        job.executionTime += delayTime;
        job.run(delayTime);
        if (alive.expired()) return;   // Async удалили из колбэка
      }
    }
    if (isLast) break;
  }
  _ticking--;
  if (_ticking == 0) purge();
}

void Async::clearAll() {
  if (_ticking) {
    for (auto &e : _jobs) e.removed = true;
  } else {
    _jobs.clear();
  }
}

size_t Async::size() const {
  size_t n = 0;
  for (auto &e : _jobs)
    if (!e.removed) n++;
  return n;
}
