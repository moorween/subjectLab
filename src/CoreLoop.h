#ifndef CoreLoop_H
#define CoreLoop_H

// CoreLoop — очередь задач, привязанная к задаче FreeRTOS (и её ядру).
// post() можно вызывать из любой задачи/ядра (не из прерывания);
// задачи выполняются той задачей, которая крутит run().

#include <Arduino.h>
#include <functional>
#include <atomic>
#include <new>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "Async.h"

// Как доставлять значения на другое ядро
enum class Delivery {
  Queue,   // каждое значение отдельной задачей (события, команды)
  Latest   // только последнее: пока задача ждёт в очереди, новые значения её перезаписывают (датчики)
};

class CoreLoop {
  public:
    using Job = std::function<void()>;

    explicit CoreLoop(size_t queueLen = 32, TickType_t postTimeout = pdMS_TO_TICKS(10))
      : _queueLen(queueLen), _postTimeout(postTimeout) {
      _queue = xQueueCreate(queueLen, sizeof(Job*));
    }

    // Поставить задачу в очередь. false — очередь переполнена (задача потеряна, см. dropped()).
    bool post(Job job) { return post(std::move(job), _postTimeout.load()); }

    bool post(Job job, TickType_t wait) {
      if (isCurrent()) wait = 0;   // ждать самого себя — гарантированная блокировка
      Job* p = new (std::nothrow) Job(std::move(job));
      if (!p) { _dropped++; return false; }
      if (xQueueSend(_queue, &p, wait) != pdTRUE) {
        delete p;
        _dropped++;
        return false;
      }
      return true;
    }

    // Вызывать в бесконечном цикле своей задачи. Ждёт задачу до maxWait тиков,
    // выполняет накопившиеся (не больше длины очереди за раз), затем async.tick().
    void run(TickType_t maxWait = 0) {
      _owner = xTaskGetCurrentTaskHandle();
      _core = xPortGetCoreID();
      _lastRun = millis();
      Job* p = nullptr;
      TickType_t wait = maxWait;
      size_t done = 0;
      while (done < _queueLen && xQueueReceive(_queue, &p, wait) == pdTRUE) {
        wait = 0;
        done++;
        try {
          (*p)();
        } catch (const std::exception& e) {
          Serial.printf("CoreLoop job error: %s\n", e.what());
        } catch (...) {
          Serial.println("CoreLoop job error");
        }
        delete p;
        _executed++;
      }
      async.tick();
    }

    // true, если вызвано из задачи, которая крутит run() этого цикла
    bool isCurrent() const {
      TaskHandle_t owner = _owner;
      return owner != nullptr && xTaskGetCurrentTaskHandle() == owner;
    }

    TaskHandle_t task() const { return _owner; }
    int core() const { return _core; }
    uint32_t dropped() const { return _dropped; }
    size_t pending() const { return uxQueueMessagesWaiting(_queue); }
    uint32_t executed() const { return _executed; }   // сколько задач выполнено
    uint32_t lastRun() const { return _lastRun; }      // millis() последнего входа в run()
    // Сколько ждать места в очереди в post() без явного wait (по умолчанию 10 мс)
    void setPostTimeout(TickType_t t) { _postTimeout = t; }
    TickType_t postTimeout() const { return _postTimeout; }

    // Таймеры этого цикла. Async не потокобезопасен: трогать только из задачи цикла
    // (или через post()).
    Async async;

  private:
    QueueHandle_t _queue;
    size_t _queueLen;
    std::atomic<TickType_t> _postTimeout;
    std::atomic<TaskHandle_t> _owner{nullptr};
    std::atomic<int> _core{-1};
    std::atomic<uint32_t> _dropped{0};
    std::atomic<uint32_t> _executed{0};
    std::atomic<uint32_t> _lastRun{0};
};

#endif // CoreLoop_H
