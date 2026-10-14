#include "Async.h"
#include <Arduino.h>

Job::Job() {
  
}
Job::Job(long delay, unsigned long mls, unsigned long exeTime, callback cb): delay(delay), mls(mls), executionTime(exeTime), _cbFn(cb)
{
  this->loopsCount = 0;
  this->breakFn = [](){ return; };
}

Job Job::attachUntilFn(std::function<bool(CallbackParams)> untilFn) {
  this->_untilFns.push_back(untilFn);
  // this->_untilFns.insert(this->_untilFns.end(), untilFn);
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

  bool untilPassed = true;
  bool whilePassed = true;

  CallbackParams params;
  params.iterationTime = delayTime;
  params.exeTime = this->executionTime;
  params.loopCount = this->loopsCount;

  for (std::function<bool(CallbackParams)> untilFn : this->_untilFns) {
    untilPassed = untilPassed && !untilFn(params);
  }

  for (std::function<bool(CallbackParams)> whileFn : this->_whileFns) {
    whilePassed = whilePassed && whileFn(params);
  }

  if (untilPassed && whilePassed) {
    params.breakFn = this->breakFn;
    this->_cbFn(params);
  } else {
    this->breakFn();
  }
}

Async::Async() {
};

Job Async::put(long delay, unsigned long mls, unsigned long exeTime, callback cb) {
  int key = delay + (millis() / 1000);
  Job job = Job(delay, mls, exeTime, cb);
  auto it = this->_jobs.insert({key, job});

  it->second.breakFn = [this, key]() {
    auto it = this->_jobs.find(key);
    if (it != this->_jobs.end()) {
      this->_jobs.erase(it);
    }
  };

  it->second.untilFn = [&it](std::function<bool(CallbackParams)> untilFn) {
    return it->second.attachUntilFn(untilFn);
  };

  it->second.whileFn = [&it](std::function<bool(CallbackParams)> whileFn) {
    return it->second.attachWhileFn(whileFn);
  };

  it->second.pause = [&it]() {
    return it->second.setActive(false);
  };

  // it->second.resumeFn = [&it]() {
  //   return it->second.setActive(true);
  // };

  return it->second;
}

Job Async::repeat(int delay, callback cb) {
  return this->put(delay, 0, -millis(), cb);
}

Job Async::repeat(int delay, std::function<void()> cb) {
  callback cb2 = [cb](CallbackParams cb3) {
    cb();
  };
  return this->put(delay, millis(), millis(), cb2);
}

Job Async::delay(int delay, std::function<void()> cb) {
  callback cb2 = [cb](CallbackParams cb3) {
    cb();
    cb3.breakFn();
  };
  return this->put(delay, millis(), millis(), cb2);
}

void Async::tick() {
  for (std::multimap<int, Job>::iterator it = this->_jobs.begin(); it != this->_jobs.end();  it++) {
      unsigned long delayTime = millis() - it->second.mls;
      if ((it->second.mls == 0) || (delayTime >= it->second.delay)) {
        it->second.mls = millis();
        it->second.loopsCount ++;
        it->second.executionTime += delayTime;

        it->second.run(delayTime);
      }
  }
}

void Async::clearAll() {
  this->_jobs.clear();
}
