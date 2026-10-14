#ifndef Subject_H
#define Subject_H

#include <Arduino.h>
#include <list>
#include <vector>
#include <memory>
#include <functional>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <tuple>
#include <optional>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "Async.h"
#include "CoreLoop.h"

template <typename T> class Subject;

// ---------------------------------------------------------------------------
// Subscription — одна подписка. Колбэк выполняется либо сразу (в задаче,
// которая рассылает значение), либо в своём CoreLoop.
// ---------------------------------------------------------------------------
template <typename T>
class Subscription : public std::enable_shared_from_this<Subscription<T>> {
  public:
    using Callback = std::function<void(T, Subscription<T>*)>;

    Subscription(Callback callback, CoreLoop* loop = nullptr, Delivery delivery = Delivery::Queue)
      : _callback(callback), _loop(loop), _delivery(delivery) {}

    void complete() { _completed = true; }
    bool completed() const { return _completed; }

    // Вызвать колбэк здесь и сейчас
    void call(T value) {
      if (!_completed) _callback(value, this);
    }

    // Доставить значение: сразу или через свой CoreLoop
    void deliver(T value) {
      if (_completed) return;
      if (!_loop || _loop->isCurrent()) {
        call(value);
        return;
      }
      auto self = this->shared_from_this();
      if (_delivery == Delivery::Latest) {
        {
          std::lock_guard<std::mutex> lock(_pendingMx);
          _pending = value;
          if (_scheduled) return;
          _scheduled = true;
        }
        bool ok = _loop->post([self]() {
          T v;
          {
            std::lock_guard<std::mutex> lock(self->_pendingMx);
            v = self->_pending;
            self->_scheduled = false;
          }
          self->call(v);
        });
        if (!ok) {
          std::lock_guard<std::mutex> lock(_pendingMx);
          _scheduled = false;
        }
      } else {
        _loop->post([self, value]() { self->call(value); });
      }
    }

  private:
    Callback _callback;
    std::atomic<bool> _completed{false};
    CoreLoop* _loop;
    Delivery _delivery;
    std::mutex _pendingMx;
    T _pending{};
    bool _scheduled = false;
};

// ---------------------------------------------------------------------------
// Subject — поток значений.
//
// Домашний цикл (on() / create(CoreLoop&)): если задан, вся обработка
// (фильтры, рассылка) идёт в задаче этого цикла, а next() из других задач
// ставится в его очередь. subscribe(), complete(), get() безопасны из любой задачи.
// Без домашнего цикла работает синхронно в вызывающей задаче.
// ---------------------------------------------------------------------------
template <typename T>
class Subject : public std::enable_shared_from_this<Subject<T>> {
  public:
    using Filter = std::function<T(T, std::shared_ptr<Subject<T>>)>;

    Subject();
    Subject(Async *async, int delay = 100);
    Subject(CoreLoop &loop, int delay = 100, Delivery delivery = Delivery::Queue);
    virtual ~Subject() {}

    // Объект из create() нужно хранить (shared_ptr): таймер держит указатель this.
    static std::shared_ptr<Subject<T>> create(Async *async, int delay = 100) {
      return std::make_shared<Subject<T>>(async, delay);
    }
    static std::shared_ptr<Subject<T>> create(CoreLoop &loop, int delay = 100, Delivery delivery = Delivery::Queue) {
      return std::make_shared<Subject<T>>(loop, delay, delivery);
    }
    static std::shared_ptr<Subject<T>> create() {
      return std::make_shared<Subject<T>>();
    }

    // Назначить домашний цикл (до начала использования из других задач)
    Subject<T>* on(CoreLoop &loop, Delivery delivery = Delivery::Queue) {
      _home = &loop;
      _delivery = delivery;
      return this;
    }
    CoreLoop* home() const { return _home; }

    std::shared_ptr<Subject<T>> pipe(std::list<Filter>);
    // Дочерний Subject, который обрабатывает и рассылает значения в цикле loop
    std::shared_ptr<Subject<T>> observeOn(CoreLoop &loop, Delivery delivery = Delivery::Queue);

    std::shared_ptr<Subject<T>> fromPin(int pin, unsigned char mode);
    std::shared_ptr<Subject<T>> fromPin(int pin);

    virtual std::shared_ptr<Subscription<T>> subscribe(std::function<void(T)>);
    virtual std::shared_ptr<Subscription<T>> subscribe(std::function<void(T, Subscription<T>*)>);
    // Колбэк будет выполняться в задаче цикла loop
    std::shared_ptr<Subscription<T>> subscribe(std::function<void(T)>, CoreLoop &loop, Delivery delivery = Delivery::Queue);
    virtual std::shared_ptr<Subscription<T>> subscribe(std::function<void(T, Subscription<T>*)>, CoreLoop &loop, Delivery delivery = Delivery::Queue);

    std::shared_ptr<Subject<T>> rejectNext() {
      this->_rejectNext = true;
      return this->shared_from_this();
    }
    std::shared_ptr<Subject<T>> forceUpdate() {
      this->_forceUpdate = true;
      return this->shared_from_this();
    }

    bool completed() const { return _completed; }
    void complete();

    void next(T);
    void next(T, bool);
    void update();

    // Потокобезопасное чтение текущего значения
    T get() {
      std::lock_guard<std::recursive_mutex> lock(_mx);
      return value;
    }

    // Текущее значение (после фильтров) и последнее входное. Из других задач читайте get().
    T value;
    T rawValue;

  protected:
    void nextLocal(T, bool);
    void post(T, bool);
    void startTimer();

    // Обёртка для отложенного вызова (в очереди цикла, в таймере): если объектом владеет
    // shared_ptr, вызов держит его через weak_ptr и пропускается, когда объект уже удалён;
    // глобальный объект или поле класса вызывается напрямую.
    template <typename F>
    std::function<void()> guarded(F fn) {
      std::weak_ptr<Subject<T>> weak = this->weak_from_this();
      bool owned = !weak.expired();
      Subject<T>* self = this;
      return [weak, owned, self, fn]() {
        if (owned) {
          if (auto s = weak.lock()) fn(s.get());
        } else {
          fn(self);
        }
      };
    }

    std::recursive_mutex _mx;
    bool _hasValue = false;   // было ли хоть одно значение
    bool _hasEmitted = false; // уходило ли значение подписчикам
    T _lastValue;

  private:
    std::function<void()> tick = []() {};
    std::list<std::shared_ptr<Subscription<T>>> _subscriptions;
    std::list<std::shared_ptr<Subject<T>>> _subjects;
    std::list<Filter> _pipe;
    bool _rejectNext = false;
    std::atomic<bool> _forceUpdate{false};
    std::atomic<bool> _completed{false};

    CoreLoop* _home = nullptr;
    Delivery _delivery = Delivery::Queue;
    int _timerDelay = 100;
    bool _timerStarted = false;
    std::mutex _pendingMx;
    T _pending{};
    bool _pendingUpdate = false;
    bool _scheduled = false;
};

// ---------------------------------------------------------------------------
// BehaviorSubject — новый подписчик сразу получает текущее значение
// ---------------------------------------------------------------------------
template <typename T>
class BehaviorSubject : public Subject<T> {
  public:
    BehaviorSubject(T initialValue) : Subject<T>() { init(initialValue); }
    BehaviorSubject(T initialValue, Async *async, int delay = 100) : Subject<T>(async, delay) { init(initialValue); }
    BehaviorSubject(T initialValue, CoreLoop &loop, int delay = 100, Delivery delivery = Delivery::Queue)
      : Subject<T>(loop, delay, delivery) { init(initialValue); }

    using Subject<T>::subscribe;

    std::shared_ptr<Subscription<T>> subscribe(std::function<void(T, Subscription<T>*)> callback) override {
      auto subscription = Subject<T>::subscribe(callback);
      subscription->deliver(this->get());
      return subscription;
    }
    std::shared_ptr<Subscription<T>> subscribe(std::function<void(T, Subscription<T>*)> callback, CoreLoop &loop,
                                               Delivery delivery = Delivery::Queue) override {
      auto subscription = Subject<T>::subscribe(callback, loop, delivery);
      subscription->deliver(this->get());
      return subscription;
    }

  private:
    void init(T initialValue) {
      this->value = initialValue;
      this->rawValue = initialValue;
      this->_lastValue = initialValue;
      this->_hasValue = true;
      this->_hasEmitted = true;
    }
};

// ---------------------------------------------------------------------------
// SubFilters — готовые фильтры для pipe()
// ---------------------------------------------------------------------------
template <typename T>
class SubFilters {
  public:
    using Filter = typename Subject<T>::Filter;

    // Пропускает только значения, равные matchTo
    static Filter matchTo(T matchTo) {
      return [matchTo] (T value, std::shared_ptr<Subject<T>> subject) {
        if (!(value == matchTo)) subject->rejectNext();
        return value;
      };
    }
    // Пропускает значения, для которых predicate вернул true
    static Filter filter(std::function<bool(T)> predicate) {
      return [predicate] (T value, std::shared_ptr<Subject<T>> subject) {
        if (!predicate(value)) subject->rejectNext();
        return value;
      };
    }
    // Переводит Subject в режим forceUpdate (каждое значение рассылается)
    static Filter forceUpdate() {
      return [] (T value, std::shared_ptr<Subject<T>> subject) {
        subject->forceUpdate();
        return value;
      };
    }
    // Скользящее среднее по frame последним значениям
    static Filter avg(int frame) {
      return [frame,
              buffer = std::vector<T>(frame),
              index = 0,
              filled = 0,
              sum = T(0)] (T value, std::shared_ptr<Subject<T>>) mutable {
        if (filled == frame) {
          sum -= buffer[index];
        } else {
          filled++;
        }
        buffer[index] = value;
        sum += value;
        index = (index + 1) % frame;
        return sum / static_cast<T>(filled);
      };
    }
    // Медиана по frame последним значениям (при чётном количестве — среднее двух центральных)
    static Filter median(int frame) {
      return [frame,
              buffer = std::vector<T>(),
              sorted = std::vector<T>(),
              index = 0,
              filled = 0] (T value, std::shared_ptr<Subject<T>>) mutable {
        if (buffer.size() < static_cast<size_t>(frame)) {
          buffer.push_back(value);
          filled++;
        } else {
          buffer[index] = value;
        }
        index = (index + 1) % frame;
        sorted.assign(buffer.begin(), buffer.begin() + filled);
        std::sort(sorted.begin(), sorted.end());
        if (filled % 2 == 1) {
          return sorted[filled / 2];
        }
        return (sorted[filled / 2 - 1] + sorted[filled / 2]) / static_cast<T>(2);
      };
    }
};

// ---------------------------------------------------------------------------
// Action — вызов функции в другой задаче/ядре с параметрами.
// Значения не сравниваются: каждый call() доставляется.
// ---------------------------------------------------------------------------
template <typename... Args>
class Action {
  public:
    using Params = std::tuple<Args...>;

    Action() : _s(std::make_shared<State>()) {}
    explicit Action(CoreLoop &home) : _s(std::make_shared<State>()) { _s->home = &home; }
    Action(const Action &) = delete;
    Action &operator=(const Action &) = delete;

    // Вызовы, которые ещё стоят в очереди, после удаления Action ничего не делают
    ~Action() {
      std::lock_guard<std::mutex> lock(_s->mx);
      _s->alive = false;
      _s->handlers.clear();
    }

    // Обработчик выполнится в задаче домашнего цикла
    std::shared_ptr<Subscription<Params>> subscribe(std::function<void(Args...)> handler) {
      auto subscription = std::make_shared<Subscription<Params>>(
        [handler](Params params, Subscription<Params>*) { std::apply(handler, params); });
      std::lock_guard<std::mutex> lock(_s->mx);
      _s->handlers.push_back(subscription);
      return subscription;
    }

    // Вызвать с любого ядра. false — очередь домашнего цикла переполнена.
    bool call(Args... args) { return send(Params(args...), false, 0); }

    // То же, но ждать места в очереди до wait тиков
    bool callWait(TickType_t wait, Args... args) { return send(Params(args...), true, wait); }

  private:
    struct State {
      CoreLoop *home = nullptr;
      std::mutex mx;
      std::list<std::shared_ptr<Subscription<Params>>> handlers;
      bool alive = true;

      void dispatch(const Params &params) {
        std::vector<std::shared_ptr<Subscription<Params>>> snapshot;
        {
          std::lock_guard<std::mutex> lock(mx);
          if (!alive) return;
          handlers.remove_if([](const std::shared_ptr<Subscription<Params>> &h) { return h->completed(); });
          snapshot.assign(handlers.begin(), handlers.end());
        }
        for (auto &h : snapshot) {
          try {
            h->call(params);
          } catch (const std::exception &e) {
            Serial.printf("Action handler error: %s\n", e.what());
          } catch (...) {
            Serial.println("Unknown action handler error");
          }
        }
      }
    };

    bool send(Params params, bool useWait, TickType_t wait) {
      std::shared_ptr<State> s = _s;
      if (s->home && !s->home->isCurrent()) {
        auto job = [s, params]() { s->dispatch(params); };
        return useWait ? s->home->post(job, wait) : s->home->post(job);
      }
      s->dispatch(params);
      return true;
    }

    std::shared_ptr<State> _s;
};

// ---------------------------------------------------------------------------
// Remote — вызов функции в другой задаче/ядре с ответом.
//
//   Remote<float(int)> readAdc(core1Loop);
//   readAdc.handle([](int pin) { return analogRead(pin) * 3.3f / 4095; });   // выполняется на ядре 1
//   readAdc.call(34, [](float v) { ... }, core0Loop);   // ответ — в задаче core0Loop
//   auto v = readAdc.callSync(pdMS_TO_TICKS(100), 34);   // ждать ответ (std::optional)
// ---------------------------------------------------------------------------
template <typename Signature> class Remote;

template <typename R, typename... Args>
class Remote<R(Args...)> {
  public:
    using Params = std::tuple<Args...>;
    using Handler = std::function<R(Args...)>;
    using Reply = std::function<void(R)>;

    Remote() : _s(std::make_shared<State>()) {}
    explicit Remote(CoreLoop &home) : _s(std::make_shared<State>()) { _s->home = &home; }
    Remote(const Remote &) = delete;
    Remote &operator=(const Remote &) = delete;

    // Вызовы, которые ещё стоят в очереди, после удаления Remote ничего не делают
    ~Remote() {
      std::lock_guard<std::mutex> lock(_s->mx);
      _s->alive = false;
      _s->handler = nullptr;
    }

    // Задать обработчик (один; новый заменяет старый). Выполняется в задаче домашнего цикла.
    void handle(Handler handler) {
      std::lock_guard<std::mutex> lock(_s->mx);
      _s->handler = handler;
    }

    // Вызвать с любого ядра, ответ выполнится в задаче цикла replyLoop.
    // false — очередь домашнего цикла полна. Если обработчика нет или он бросил
    // исключение, ответа не будет.
    bool call(Args... args, Reply reply, CoreLoop &replyLoop) {
      return send(Params(args...), reply, &replyLoop);
    }

    // То же, ответ выполнится в домашнем цикле (сразу после обработчика)
    bool call(Args... args, Reply reply) {
      return send(Params(args...), reply, nullptr);
    }

    // Вызвать и ждать результат до timeout тиков. Пусто — очередь полна, нет обработчика,
    // исключение в обработчике или не дождались. Из домашнего цикла выполняется сразу.
    // Не вызывайте из цикла A функцию цикла B, которая сама синхронно ждёт цикл A, —
    // оба будут ждать до таймаута.
    std::optional<R> callSync(TickType_t timeout, Args... args) {
      Params params(args...);
      std::shared_ptr<State> s = _s;
      if (!s->home || s->home->isCurrent()) return s->invoke(params);

      std::shared_ptr<Wait> w = std::make_shared<Wait>();   // переживёт таймаут, если ответ опоздает
      if (!w->sem) return std::nullopt;
      TickType_t start = xTaskGetTickCount();
      bool posted = s->home->post([s, params, w]() {
        w->result = s->invoke(params);
        xSemaphoreGive(w->sem);
      }, timeout);
      if (!posted) return std::nullopt;

      TickType_t left = portMAX_DELAY;
      if (timeout != portMAX_DELAY) {
        TickType_t spent = xTaskGetTickCount() - start;
        left = spent >= timeout ? 0 : timeout - spent;
      }
      if (xSemaphoreTake(w->sem, left) != pdTRUE) return std::nullopt;
      return w->result;
    }

  private:
    struct State {
      CoreLoop *home = nullptr;
      std::mutex mx;
      Handler handler;
      bool alive = true;

      std::optional<R> invoke(const Params &params) {
        Handler h;
        {
          std::lock_guard<std::mutex> lock(mx);
          if (!alive || !handler) return std::nullopt;
          h = handler;
        }
        try {
          return std::apply(h, params);
        } catch (const std::exception &e) {
          Serial.printf("Remote handler error: %s\n", e.what());
        } catch (...) {
          Serial.println("Unknown remote handler error");
        }
        return std::nullopt;
      }
    };

    struct Wait {
      SemaphoreHandle_t sem = xSemaphoreCreateBinary();
      std::optional<R> result;
      ~Wait() { if (sem) vSemaphoreDelete(sem); }
    };

    bool send(Params params, Reply reply, CoreLoop *replyLoop) {
      std::shared_ptr<State> s = _s;
      auto job = [s, params, reply, replyLoop]() {
        std::optional<R> result = s->invoke(params);
        if (!result || !reply) return;
        R value = *result;
        if (replyLoop && !replyLoop->isCurrent()) {
          replyLoop->post([reply, value]() { reply(value); });
          return;
        }
        try {
          reply(value);
        } catch (const std::exception &e) {
          Serial.printf("Remote reply error: %s\n", e.what());
        } catch (...) {
          Serial.println("Unknown remote reply error");
        }
      };
      if (s->home && !s->home->isCurrent()) return s->home->post(job);
      job();
      return true;
    }

    std::shared_ptr<State> _s;
};

// ---------------------------------------------------------------------------
// Реализация Subject
// ---------------------------------------------------------------------------

template <typename T>
Subject<T>::Subject() : value(T()), rawValue(T()), _lastValue(T()) {}

template <typename T>
Subject<T>::Subject(Async *async, int delay) : value(T()), rawValue(T()), _lastValue(T())
{
  async->repeat(delay, [this](CallbackParams cb) { this->tick(); });
}

template <typename T>
Subject<T>::Subject(CoreLoop &loop, int delay, Delivery delivery) : value(T()), rawValue(T()), _lastValue(T())
{
  _home = &loop;
  _delivery = delivery;
  _timerDelay = delay;   // таймер запускается в fromPin()
}

// Вызывается в задаче домашнего цикла (Async не потокобезопасен)
template <typename T>
void Subject<T>::startTimer() {
  if (_timerStarted || !_home) return;
  _timerStarted = true;
  auto tickFn = guarded([](Subject<T>* s) { s->tick(); });
  _home->async.repeat(_timerDelay, [tickFn](CallbackParams) { tickFn(); });
}

template <typename T>
std::shared_ptr<Subject<T>> Subject<T>::fromPin(int pin, unsigned char mode)
{
  auto setup = guarded([pin, mode](Subject<T>* s) {
    pinMode(pin, mode);
    if (std::is_same<T, bool>::value) {
      s->tick = [s, pin]() { s->next(T(digitalRead(pin))); };
    } else {
      s->tick = [s, pin]() { s->next(T(analogRead(pin))); };
    }
    s->startTimer();
  });
  // tick вызывается в задаче домашнего цикла — там же его и меняем
  if (_home && !_home->isCurrent()) _home->post(setup, portMAX_DELAY);
  else setup();

  return this->shared_from_this();
}

template <typename T>
std::shared_ptr<Subject<T>> Subject<T>::fromPin(int pin) {
  #ifdef INPUT_PULLDOWN
    return this->fromPin(pin, INPUT_PULLDOWN);
  #else
    return this->fromPin(pin, INPUT);
  #endif
}

template <typename T>
std::shared_ptr<Subject<T>> Subject<T>::pipe(std::list<Filter> pipe) {
  auto child = std::make_shared<Subject<T>>();
  child->_pipe = pipe;
  std::lock_guard<std::recursive_mutex> lock(_mx);
  this->_subjects.push_back(child);
  return child;
}

template <typename T>
std::shared_ptr<Subject<T>> Subject<T>::observeOn(CoreLoop &loop, Delivery delivery) {
  auto child = std::make_shared<Subject<T>>();
  child->on(loop, delivery);
  std::lock_guard<std::recursive_mutex> lock(_mx);
  this->_subjects.push_back(child);
  return child;
}

template <typename T>
std::shared_ptr<Subscription<T>> Subject<T>::subscribe(std::function<void(T)> callback)
{
  return this->subscribe([callback](T value, Subscription<T>*) { callback(value); });
}

template <typename T>
std::shared_ptr<Subscription<T>> Subject<T>::subscribe(std::function<void(T, Subscription<T>*)> callback)
{
  auto subscription = std::make_shared<Subscription<T>>(callback);
  std::lock_guard<std::recursive_mutex> lock(_mx);
  this->_subscriptions.push_back(subscription);
  return subscription;
}

template <typename T>
std::shared_ptr<Subscription<T>> Subject<T>::subscribe(std::function<void(T)> callback, CoreLoop &loop, Delivery delivery)
{
  return this->subscribe([callback](T value, Subscription<T>*) { callback(value); }, loop, delivery);
}

template <typename T>
std::shared_ptr<Subscription<T>> Subject<T>::subscribe(std::function<void(T, Subscription<T>*)> callback, CoreLoop &loop, Delivery delivery)
{
  auto subscription = std::make_shared<Subscription<T>>(callback, &loop, delivery);
  std::lock_guard<std::recursive_mutex> lock(_mx);
  this->_subscriptions.push_back(subscription);
  return subscription;
}

template <typename T>
void Subject<T>::complete() {
  std::lock_guard<std::recursive_mutex> lock(_mx);
  _completed = true;
  for (auto &s : _subscriptions) s->complete();
  _subscriptions.clear();
  _subjects.clear();
}

template <typename T>
void Subject<T>::next(T value) {
  this->next(value, this->_forceUpdate);
}

template <typename T>
void Subject<T>::next(T value, bool update)
{
  if (_completed) return;
  if (_home && !_home->isCurrent()) {
    post(value, update);
    return;
  }
  nextLocal(value, update);
}

// Отправить значение в домашний цикл
template <typename T>
void Subject<T>::post(T value, bool update)
{
  if (_delivery == Delivery::Latest) {
    {
      std::lock_guard<std::mutex> lock(_pendingMx);
      _pending = value;
      _pendingUpdate = _pendingUpdate || update;
      if (_scheduled) return;
      _scheduled = true;
    }
    bool ok = _home->post(guarded([](Subject<T>* s) {
      T v;
      bool u;
      {
        std::lock_guard<std::mutex> lock(s->_pendingMx);
        v = s->_pending;
        u = s->_pendingUpdate;
        s->_pendingUpdate = false;
        s->_scheduled = false;
      }
      s->nextLocal(v, u);
    }));
    if (!ok) {
      std::lock_guard<std::mutex> lock(_pendingMx);
      _scheduled = false;
    }
  } else {
    _home->post(guarded([value, update](Subject<T>* s) { s->nextLocal(value, update); }));
  }
}

// Обработка значения в текущей задаче
template <typename T>
void Subject<T>::nextLocal(T value, bool update)
{
  if (_completed) return;

  {
    std::lock_guard<std::recursive_mutex> lock(_mx);
    if (!update && _hasValue && value == this->rawValue) {
      return;
    }
    this->rawValue = value;
    _hasValue = true;
  }

  T result = value;
  if (!_pipe.empty()) {
    auto self = this->shared_from_this();
    for (const auto& fn : _pipe) {
      try {
        if (fn) {
          result = fn(result, self);
          if (this->_rejectNext) {
            this->_rejectNext = false;
            return;
          }
        }
      }
      catch(const std::exception& e) {
        Serial.printf("Filter error: %s\n", e.what());
        return;
      }
      catch(...) {
        Serial.println("Unknown filter error");
        return;
      }
    }
  }

  // Снимок списков под замком; колбэки вызываются без замка, поэтому из них
  // можно подписываться, отписываться и вызывать complete()
  std::vector<std::shared_ptr<Subject<T>>> subjects;
  std::vector<std::shared_ptr<Subscription<T>>> subscriptions;
  bool notify;
  {
    std::lock_guard<std::recursive_mutex> lock(_mx);
    this->value = result;

    _subjects.remove_if([](const std::shared_ptr<Subject<T>> &s) { return s->completed(); });
    subjects.assign(_subjects.begin(), _subjects.end());

    notify = update || !_hasEmitted || !(value == this->_lastValue);
    if (notify) {
      this->_lastValue = value;
      _hasEmitted = true;
      _subscriptions.remove_if([](const std::shared_ptr<Subscription<T>> &s) { return s->completed(); });
      subscriptions.assign(_subscriptions.begin(), _subscriptions.end());
    }
  }

  for (auto &subject : subjects) {
    try {
      subject->next(result);
    }
    catch(const std::exception& e) {
      Serial.printf("Subject error: %s\n", e.what());
    }
    catch(...) {
      Serial.println("Unknown subject error");
    }
  }

  for (auto &subscription : subscriptions) {
    try {
      subscription->deliver(result);
    }
    catch(const std::exception& e) {
      Serial.printf("Subscriber error: %s\n", e.what());
    }
    catch(...) {
      Serial.println("Unknown subscriber error");
    }
  }
}

template <typename T>
void Subject<T>::update() {
  T raw;
  {
    std::lock_guard<std::recursive_mutex> lock(_mx);
    raw = this->rawValue;
  }
  this->next(raw, true);
}

#endif // Subject_H
