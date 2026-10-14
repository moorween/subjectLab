// subjectLab — проверка всех функций библиотеки на реальном ESP32.
// Вывод в Serial (115200). Периферия не нужна; GPIO27 и GPIO33 должны быть свободны.
// Повторный запуск тестов: отправить 'r' в монитор порта.

#include <Arduino.h>
#include <vector>
#include <cmath>
#include <stdexcept>
#include <Subject.h>   // подключает также CoreLoop.h и Async.h

static const int PIN_DIGITAL = 27;   // тест fromPin для bool
static const int PIN_ANALOG  = 33;   // тест fromPin для float (ADC1)

// Циклы задач: core0Loop — задача на ядре 0, core1Loop — задача на ядре 1.
// Тесты выполняет задача Arduino loopTask (ядро 1), отдельная от задач циклов.
CoreLoop core0Loop(32);
CoreLoop core1Loop(32);
CoreLoop smallLoop(4, 0);            // маленькая очередь без ожидания — для теста переполнения
TaskHandle_t core0Task = nullptr, core1Task = nullptr, smallTask = nullptr, driverTask = nullptr;

static void coreLoopTask(void *param) {
  CoreLoop *loop = static_cast<CoreLoop *>(param);
  for (;;) loop->run(pdMS_TO_TICKS(5));
}

// ---------------------------------------------------------------------------
// Мини-фреймворк
// ---------------------------------------------------------------------------
static const int MAX_TESTS = 40;
static int gPass = 0, gFail = 0, gTestFail = 0, gTests = 0, gTestsFailed = 0;
static const char *gTestName = "";
static bool gQuiet = false;          // серийный режим: печатать только ошибки
static bool gHeaderShown = false;
static int gRunNo = 0;             // номер прогона: глобальные объекты живут между прогонами
static int gFailRuns[MAX_TESTS + 1]; // сколько прогонов провалил каждый тест (серийный режим)
static const char *gNames[MAX_TESTS + 1];

static void showHeader() {
  if (gHeaderShown) return;
  gHeaderShown = true;
  Serial.printf("\n[%2d] %s\n", gTests, gTestName);
}

static void failHere(int line) {
  gFail++; gTestFail++;
  showHeader();
  Serial.printf("    FAIL (строка %d): ", line);
}

static void check(bool ok, const char *expr, int line) {
  if (ok) { gPass++; return; }
  failHere(line);
  Serial.println(expr);
}

template <typename A, typename B>
static void checkEq(A a, B b, const char *expr, int line) {
  if (a == b) { gPass++; return; }
  failHere(line);
  Serial.printf("%s — получено ", expr);
  Serial.print(a);
  Serial.print(", ожидалось ");
  Serial.println(b);
}

static void checkNear(float a, float b, const char *expr, int line) {
  if (fabsf(a - b) < 0.001f) { gPass++; return; }
  failHere(line);
  Serial.printf("%s — получено %.4f, ожидалось %.4f\n", expr, a, b);
}

// Ждать условия до timeoutMs
template <typename F>
static bool waitFor(F cond, uint32_t timeoutMs = 1000) {
  uint32_t start = millis();
  while (!cond()) {
    if (millis() - start > timeoutMs) return false;
    vTaskDelay(1);
  }
  return true;
}

static const char *taskStateName(TaskHandle_t t) {
  if (!t) return "нет";
  switch (eTaskGetState(t)) {
    case eRunning:   return "выполняется";
    case eReady:     return "готова";
    case eBlocked:   return "ждёт";
    case eSuspended: return "приостановлена";
    case eDeleted:   return "удалена";
    default:         return "?";
  }
}

// Состояние циклов и пробная задача в каждый — что с очередями в момент сбоя
static void dumpLoops() {
  struct L { const char *name; CoreLoop *loop; } loops[] = {
    {"core0Loop", &core0Loop}, {"core1Loop", &core1Loop}, {"smallLoop", &smallLoop}};
  showHeader();
  Serial.printf("      диагностика (%lu мс), задача тестов: ядро %d, приоритет %u\n",
                (unsigned long)millis(), xPortGetCoreID(), (unsigned)uxTaskPriorityGet(nullptr));
  for (auto &l : loops) {
    TaskHandle_t t = l.loop->task();
    Serial.printf("      %s: ядро %d, задача %s, приоритет %u, в очереди %u, выполнено %lu, потеряно %lu, run() был %lu мс назад\n",
                  l.name, l.loop->core(), taskStateName(t), t ? (unsigned)uxTaskPriorityGet(t) : 0,
                  (unsigned)l.loop->pending(), (unsigned long)l.loop->executed(), (unsigned long)l.loop->dropped(),
                  (unsigned long)(millis() - l.loop->lastRun()));
  }
  for (auto &l : loops) {
    std::atomic<uint32_t> *doneAt = new std::atomic<uint32_t>(0);   // не на стеке: задача может выполниться позже
    uint32_t t0 = millis();
    bool posted = l.loop->post([doneAt]() { *doneAt = millis(); }, pdMS_TO_TICKS(100));
    bool ok = posted && waitFor([&]() { return doneAt->load() != 0; }, 500);
    if (ok) {
      Serial.printf("      проба %s: выполнена через %lu мс\n", l.name, (unsigned long)(doneAt->load() - t0));
      delete doneAt;
    } else {
      Serial.printf("      проба %s: %s\n", l.name, posted ? "НЕ выполнена за 500 мс" : "не поставлена (очередь полна)");
      // doneAt не удаляем: задача ещё может выполниться
    }
  }
}

// Дождаться условия; при неудаче — диагностика и проверка, не придёт ли событие с опозданием
template <typename F>
static void checkWait(F cond, uint32_t timeoutMs, const char *expr, int line) {
  uint32_t t0 = millis();
  if (waitFor(cond, timeoutMs)) { gPass++; return; }
  failHere(line);
  Serial.printf("не дождались за %lu мс: %s\n", (unsigned long)timeoutMs, expr);
  dumpLoops();
  if (waitFor(cond, 3000)) Serial.printf("      событие пришло с опозданием: через %lu мс\n", (unsigned long)(millis() - t0));
  else Serial.printf("      событие не пришло и через %lu мс\n", (unsigned long)(millis() - t0));
}

#define CHECK(c)            check((c), #c, __LINE__)
#define CHECK_EQ(a, b)      checkEq((a), (b), #a " == " #b, __LINE__)
#define CHECK_NEAR(a, b)    checkNear((a), (b), #a " ≈ " #b, __LINE__)
#define CHECK_WAIT(c, ms)   checkWait([&]() { return (c); }, (ms), #c, __LINE__)
#define INFO(...)           do { if (!gQuiet) Serial.printf("    · " __VA_ARGS__); } while (0)

// Барьер: дождаться, пока все циклы выполнят всё, что уже стоит в очереди.
// Иначе задачи предыдущего теста могли бы обратиться к его локальным переменным.
static void syncLoops() {
  CoreLoop *loops[] = {&core0Loop, &core1Loop, &smallLoop};
  const char *names[] = {"core0Loop", "core1Loop", "smallLoop"};
  for (int i = 0; i < 3; i++) {
    auto *flag = new std::atomic<bool>(false);
    bool posted = loops[i]->post([flag]() { *flag = true; }, pdMS_TO_TICKS(1000));
    if (posted && waitFor([&]() { return flag->load(); }, 2000)) {
      delete flag;
    } else {
      showHeader();
      Serial.printf("    ! %s не освободился после теста\n", names[i]);
      gFail++; gTestFail++;
    }
  }
}

static void beginTest(const char *name) {
  gTestName = name;
  gTestFail = 0;
  gTests++;
  if (gTests <= MAX_TESTS) gNames[gTests] = name;
  gHeaderShown = false;
  if (!gQuiet) showHeader();
}

static void endTest() {
  syncLoops();
  if (gTestFail == 0) {
    if (!gQuiet) Serial.println("    OK");
  } else {
    gTestsFailed++;
    if (gTests <= MAX_TESTS) gFailRuns[gTests]++;
    Serial.printf("    ПРОВАЛЕНО проверок: %d\n", gTestFail);
  }
}

static TaskHandle_t currentTask() { return xTaskGetCurrentTaskHandle(); }

// Заблокировать задачу цикла, пока не отпустим (для проверки очередей)
struct LoopBlocker {
  SemaphoreHandle_t sem = xSemaphoreCreateBinary();
  std::atomic<bool> started{false}, finished{false};
  void block(CoreLoop &loop) {
    started = false;
    finished = false;
    loop.post([this]() {
      started = true;
      xSemaphoreTake(sem, portMAX_DELAY);
      finished = true;
    }, portMAX_DELAY);
    waitFor([this]() { return started.load(); });
  }
  // Отпустить и дождаться, пока задача выйдет из xSemaphoreTake (иначе семафор удалят раньше времени)
  void release() {
    xSemaphoreGive(sem);
    waitFor([this]() { return finished.load(); });
  }
  ~LoopBlocker() { vSemaphoreDelete(sem); }
};

struct Point {
  int x, y;
  bool operator==(const Point &o) const { return x == o.x && y == o.y; }
};

// ---------------------------------------------------------------------------
// Синхронные тесты (без домашнего цикла)
// ---------------------------------------------------------------------------

void testNext() {
  beginTest("next(): доставка, отсев повторов, next(v, true), update(), get()");
  Subject<int> s;
  std::vector<int> got;
  s.subscribe([&](int v) { got.push_back(v); });

  s.next(0);          // первое значение доставляется, даже если равно T()
  s.next(0);          // повтор — отсекается
  s.next(1);
  s.next(1);
  s.next(2);
  CHECK(got == std::vector<int>({0, 1, 2}));

  s.next(2, true);    // принудительно
  CHECK_EQ(got.size(), 4u);
  s.update();         // повторить последнее
  CHECK_EQ(got.size(), 5u);
  CHECK_EQ(got.back(), 2);

  CHECK_EQ(s.value, 2);
  CHECK_EQ(s.rawValue, 2);
  CHECK_EQ(s.get(), 2);
  endTest();
}

void testForceUpdate() {
  beginTest("forceUpdate(): каждое значение рассылается");
  auto s = Subject<int>::create();
  int n = 0;
  s->subscribe([&](int) { n++; });
  s->next(5);
  s->next(5);
  CHECK_EQ(n, 1);
  s->forceUpdate();
  s->next(5);
  s->next(5);
  CHECK_EQ(n, 3);
  endTest();
}

void testSubscriptions() {
  beginTest("subscribe() обе перегрузки, Subscription::complete()/completed()");
  Subject<int> s;
  int a = 0, b = 0, c = 0;
  auto subA = s.subscribe([&](int) { a++; });
  auto subB = s.subscribe([&](int v, Subscription<int> *sub) {
    b++;
    if (v == 2) sub->complete();   // отписка из колбэка
  });
  s.subscribe([&](int) { c++; });

  s.next(1); s.next(2); s.next(3);
  CHECK_EQ(a, 3);
  CHECK_EQ(b, 2);
  CHECK_EQ(c, 3);
  CHECK(subB->completed());

  subA->complete();                 // отписка снаружи
  CHECK(subA->completed());
  s.next(4);
  CHECK_EQ(a, 3);
  CHECK_EQ(c, 4);

  // Подписка из колбэка: новый подписчик получает значения со следующего
  Subject<int> s3;
  int late = 0;
  bool added = false;
  s3.subscribe([&](int) {
    if (!added) { added = true; s3.subscribe([&](int) { late++; }); }
  });
  s3.next(1);
  CHECK_EQ(late, 0);
  s3.next(2);
  CHECK_EQ(late, 1);
  endTest();
}

void testComplete() {
  beginTest("Subject::complete(): остановка рассылки, вызов из колбэка");
  Subject<int> s;
  int n = 0;
  auto sub = s.subscribe([&](int) { n++; });
  s.next(1);
  s.complete();
  s.next(2);
  CHECK_EQ(n, 1);
  CHECK(s.completed());
  CHECK(sub->completed());

  Subject<int> s2;
  int m = 0;
  s2.subscribe([&](int) { m++; s2.complete(); });
  s2.subscribe([&](int) { m++; });   // уже не должен вызваться
  s2.next(1);
  s2.next(2);
  CHECK_EQ(m, 1);
  endTest();
}

void testPipe() {
  beginTest("pipe(): преобразования, цепочки, ветвление, вложенность");
  auto src = Subject<int>::create();
  std::vector<int> x2, chain, nested;

  src->pipe({ [](int v, auto) { return v * 2; } })
     ->subscribe([&](int v) { x2.push_back(v); });

  auto p2 = src->pipe({
    [](int v, auto) { return v + 1; },
    [](int v, auto) { return v * 10; },
  });
  p2->subscribe([&](int v) { chain.push_back(v); });

  src->next(1);
  src->next(2);
  CHECK(x2 == std::vector<int>({2, 4}));
  CHECK(chain == std::vector<int>({20, 30}));
  CHECK_EQ(p2->value, 30);
  CHECK_EQ(p2->rawValue, 2);

  p2->pipe({ [](int v, auto) { return -v; } })->subscribe([&](int v) { nested.push_back(v); });
  src->next(3);
  CHECK(nested == std::vector<int>({-40}));

  // pipe() у обычного (не shared_ptr) Subject
  Subject<int> plain;
  int pv = 0;
  plain.pipe({ [](int v, auto) { return v + 100; } })->subscribe([&](int v) { pv = v; });
  plain.next(1);
  CHECK_EQ(pv, 101);

  // Завершённый дочерний Subject больше ничего не получает
  CHECK_EQ(chain.size(), 3u);      // 20, 30, 40
  p2->complete();
  src->next(4);
  CHECK_EQ(chain.size(), 3u);
  CHECK_EQ(x2.back(), 8);          // соседняя ветка работает
  endTest();
}

void testFilters() {
  beginTest("rejectNext(), SubFilters::matchTo(), SubFilters::filter()");
  auto src = Subject<int>::create();
  std::vector<int> evens, threes, big;
  int afterReject = 0;

  src->pipe({ [](int v, std::shared_ptr<Subject<int>> s) {
    if (v % 2) s->rejectNext();
    return v;
  } })->subscribe([&](int v) { evens.push_back(v); });

  src->pipe({ SubFilters<int>::matchTo(3) })->subscribe([&](int v) { threes.push_back(v); });
  src->pipe({ SubFilters<int>::filter([](int v) { return v > 3; }) })->subscribe([&](int v) { big.push_back(v); });
  src->pipe({
    SubFilters<int>::matchTo(100),
    [&](int v, auto) { afterReject++; return v; },   // после отказа не вызывается
  });

  for (int v = 1; v <= 5; v++) src->next(v);
  CHECK(evens == std::vector<int>({2, 4}));
  CHECK(threes == std::vector<int>({3}));
  CHECK(big == std::vector<int>({4, 5}));
  CHECK_EQ(afterReject, 0);
  endTest();
}

void testAvgMedian() {
  beginTest("SubFilters::avg(), SubFilters::median()");
  auto src = Subject<float>::create();
  std::vector<float> avg, med3, med4;
  src->pipe({ SubFilters<float>::avg(3) })->subscribe([&](float v) { avg.push_back(v); });
  src->pipe({ SubFilters<float>::median(3) })->subscribe([&](float v) { med3.push_back(v); });
  src->pipe({ SubFilters<float>::median(4) })->subscribe([&](float v) { med4.push_back(v); });

  for (float v : {5.0f, 1.0f, 9.0f, 3.0f}) src->next(v);

  // avg(3): 5 | (5+1)/2 | (5+1+9)/3 | (1+9+3)/3
  CHECK_EQ(avg.size(), 4u);
  if (avg.size() == 4) {
    CHECK_NEAR(avg[0], 5.0f);
    CHECK_NEAR(avg[1], 3.0f);
    CHECK_NEAR(avg[2], 5.0f);
    CHECK_NEAR(avg[3], 13.0f / 3);
  }
  // median(3): [5] | [5,1] | [5,1,9] | [3,1,9]
  CHECK_EQ(med3.size(), 4u);
  if (med3.size() == 4) {
    CHECK_NEAR(med3[0], 5.0f);
    CHECK_NEAR(med3[1], 3.0f);
    CHECK_NEAR(med3[2], 5.0f);
    CHECK_NEAR(med3[3], 3.0f);
  }
  // median(4), чётное окно: [5,1,9,3] → (3+5)/2
  CHECK_EQ(med4.size(), 4u);
  if (med4.size() == 4) CHECK_NEAR(med4[3], 4.0f);
  endTest();
}

void testForceUpdateFilter() {
  beginTest("SubFilters::forceUpdate(): дочерний Subject пропускает повторы");
  auto src = Subject<int>::create();
  int n = 0;
  src->pipe({ SubFilters<int>::forceUpdate() })->subscribe([&](int) { n++; });
  src->next(1);
  CHECK_EQ(n, 1);
  src->next(1);           // родитель отсекает повтор сам
  CHECK_EQ(n, 1);
  src->next(1, true);     // родитель пропустил принудительно — дочерний тоже
  CHECK_EQ(n, 2);
  endTest();
}

void testBehavior() {
  beginTest("BehaviorSubject: начальное значение новым подписчикам");
  BehaviorSubject<int> b(5);
  int g1 = -1, g2 = -1, cnt = 0;
  b.subscribe([&](int v) { g1 = v; cnt++; });
  CHECK_EQ(g1, 5);
  b.subscribe([&](int v, Subscription<int> *) { g2 = v; });
  CHECK_EQ(g2, 5);
  b.next(5);              // то же значение — повтор
  CHECK_EQ(cnt, 1);
  b.next(0);
  CHECK_EQ(g1, 0);
  CHECK_EQ(cnt, 2);

  std::shared_ptr<Subject<int>> base = std::make_shared<BehaviorSubject<int>>(7);
  int g3 = -1;
  base->subscribe([&](int v) { g3 = v; });   // через указатель на базовый класс
  CHECK_EQ(g3, 7);
  endTest();
}

void testExceptions() {
  beginTest("Исключения в подписчике и фильтре (ниже 2 ожидаемых сообщения об ошибке)");
  Subject<int> s;
  int after = 0;
  s.subscribe([](int) { throw std::runtime_error("ожидаемая ошибка подписчика"); });
  s.subscribe([&](int) { after++; });
  s.next(1);
  CHECK_EQ(after, 1);     // остальные подписчики вызваны

  auto src = Subject<int>::create();
  int got = 0;
  src->pipe({ [](int, auto) -> int { throw std::runtime_error("ожидаемая ошибка фильтра"); } })
     ->subscribe([&](int) { got++; });
  src->next(1);
  CHECK_EQ(got, 0);       // ошибка фильтра прерывает обработку
  endTest();
}

void testTypes() {
  beginTest("Типы: String и структура только с operator==");
  Subject<String> s;
  std::vector<String> got;
  s.subscribe([&](String v) { got.push_back(v); });
  s.next("Idle");
  s.next("Idle");
  s.next("Working");
  CHECK_EQ(got.size(), 2u);
  if (got.size() == 2) CHECK(got[1] == "Working");

  Subject<Point> p;
  int n = 0;
  p.subscribe([&](Point) { n++; });
  p.next({1, 2});
  p.next({1, 2});
  p.next({2, 2});
  CHECK_EQ(n, 2);
  endTest();
}

void testFromPinAsync() {
  beginTest("fromPin() + Async: digitalRead (GPIO27) и analogRead (GPIO33)");
  Async a;
  auto d = Subject<bool>::create(&a, 1);
  std::vector<bool> got;
  d->subscribe([&](bool v) { got.push_back(v); });
  d->fromPin(PIN_DIGITAL, INPUT_PULLUP);
  delay(2); a.tick();
  CHECK_EQ(d->value, true);
  pinMode(PIN_DIGITAL, INPUT_PULLDOWN);
  delay(2); a.tick();
  CHECK_EQ(d->value, false);
  CHECK(got == std::vector<bool>({true, false}));

  Async a2;
  auto an = Subject<float>::create(&a2, 1);
  int ticks = 0;
  an->forceUpdate();
  an->subscribe([&](float) { ticks++; });
  an->fromPin(PIN_ANALOG, INPUT_PULLUP);
  delay(5); a2.tick();
  float hi = an->value;
  pinMode(PIN_ANALOG, INPUT_PULLDOWN);
  delay(5); a2.tick();
  float lo = an->value;
  CHECK_EQ(ticks, 2);
  CHECK(hi >= 0 && hi <= 4095);
  CHECK(lo >= 0 && lo <= 4095);
  INFO("АЦП с подтяжкой вверх: %.0f, вниз: %.0f (справочно, обычно ~4095 и ~0)\n", hi, lo);
  pinMode(PIN_ANALOG, INPUT);
  pinMode(PIN_DIGITAL, INPUT);
  endTest();
}


// Прогнать tick() count раз с паузой stepMs
static void tickFor(Async &a, int count, int stepMs = 2) {
  for (int i = 0; i < count; i++) { a.tick(); delay(stepMs); }
}

void testAsyncBasic() {
  beginTest("Async: delay, repeat, breakFn из колбэка, одинаковые задержки");
  Async a;
  int d1 = 0, d2 = 0, d3 = 0, r = 0, rp = 0;
  long lastLoop = 0;
  // Три задачи с одинаковой задержкой: в старой версии breakFn одной могла удалить другую
  a.delay(5, [&]() { d1++; });
  a.delay(5, [&]() { d2++; });
  a.delay(5, [&]() { d3++; });
  a.repeat(5, [&]() { r++; });
  a.repeat(5, [&](CallbackParams p) {
    rp++;
    lastLoop = p.loopCount;
    if (rp == 3) p.breakFn();     // отмена себя из колбэка
  });
  CHECK_EQ(a.size(), 5u);
  tickFor(a, 40);                 // ~80 мс
  CHECK_EQ(d1, 1);
  CHECK_EQ(d2, 1);
  CHECK_EQ(d3, 1);
  CHECK(r >= 5);
  CHECK_EQ(rp, 3);
  CHECK_EQ(lastLoop, 3L);
  CHECK_EQ(a.size(), 1u);         // остался только repeat(r)

  int q = 0;
  a.queue([&]() { q++; });
  a.tick();
  a.tick();
  CHECK_EQ(q, 1);
  endTest();
}

void testAsyncMutation() {
  beginTest("Async: clearAll(), добавление и отмена задач из колбэков");
  Async a;
  int x = 0, y = 0;
  a.delay(1, [&]() { x++; a.clearAll(); });
  a.delay(1, [&]() { y++; });      // в том же tick() после clearAll() — не выполняется
  delay(3);
  a.tick();
  CHECK_EQ(x, 1);
  CHECK_EQ(y, 0);
  CHECK_EQ(a.size(), 0u);
  tickFor(a, 3);
  CHECK_EQ(x, 1);

  // Задача, добавленная из колбэка, выполняется не в том же tick()
  int added = 0, sameTick = 0;
  a.repeat(1, [&](CallbackParams p) {
    if (p.loopCount == 1) {
      a.queue([&]() { added++; });
      sameTick = added;
    }
  });
  a.tick();
  CHECK_EQ(added, 0);
  tickFor(a, 5);
  CHECK_EQ(added, 1);
  CHECK_EQ(sameTick, 0);
  a.clearAll();

  // Отмена чужой задачи из колбэка: и той, что уже выполнилась в этом tick(), и следующей
  int z = 0, w = 0;
  Job jz = a.repeat(1, [&]() { z++; });
  a.delay(1, [&, jz]() mutable { jz.breakFn(); });   // jz уже выполнилась в этом tick()
  Job jw;
  a.delay(1, [&]() { jw.breakFn(); });               // jw ещё не выполнялась — и не выполнится
  jw = a.repeat(1, [&]() { w++; });
  delay(3);
  a.tick();
  tickFor(a, 5);
  CHECK_EQ(z, 1);
  CHECK_EQ(w, 0);
  CHECK_EQ(a.size(), 0u);

  // Удаление Async из колбэка
  Async *dyn = new Async();
  int afterDelete = 0;
  dyn->delay(1, [&]() { Async *d = dyn; dyn = nullptr; delete d; });   // после delete — ни одного обращения к захваченному
  dyn->delay(1, [&]() { afterDelete++; });
  delay(3);
  dyn->tick();
  CHECK(dyn == nullptr);
  CHECK_EQ(afterDelete, 0);
  endTest();
}

void testAsyncJob() {
  beginTest("Async: Job снаружи — run(), breakFn(), pause/resume, untilFn, whileFn");
  Async a;
  int n = 0;
  Job j = a.delay(1000, [&]() { n++; });
  j.run();                         // выполнить сейчас: колбэк и отмена задачи
  CHECK_EQ(n, 1);
  CHECK_EQ(a.size(), 0u);

  int k = 0;
  Job jk = a.repeat(1, [&]() { k++; });
  jk.pause();
  tickFor(a, 5);
  CHECK_EQ(k, 0);
  jk.resume();
  tickFor(a, 5);
  CHECK(k > 0);
  jk.breakFn();
  CHECK_EQ(a.size(), 0u);
  int kStop = k;
  tickFor(a, 3);
  CHECK_EQ(k, kStop);

  int u = 0;
  Job ju = a.repeat(1, [&]() { u++; });
  ju.untilFn([](CallbackParams p) { return p.loopCount > 3; });
  tickFor(a, 15);
  CHECK_EQ(u, 3);
  CHECK_EQ(a.size(), 0u);

  int wv = 0;
  Job jw = a.repeat(1, [&]() { wv++; });
  jw.whileFn([](CallbackParams p) { return p.loopCount <= 2; });
  tickFor(a, 15);
  CHECK_EQ(wv, 2);
  CHECK_EQ(a.size(), 0u);

  // Job пережил свой Async; Job по умолчанию
  Job stale;
  {
    Async b;
    stale = b.delay(10, []() {});
  }
  stale.breakFn();
  stale.pause();
  stale.resume();
  stale.untilFn([](CallbackParams) { return true; });
  Job empty;
  empty.run();
  empty.breakFn();
  CHECK(true);   // дошли сюда — без сбоев
  endTest();
}

// ---------------------------------------------------------------------------
// Межъядерные тесты
// ---------------------------------------------------------------------------

void testCoreLoop() {
  beginTest("CoreLoop: выполнение на нужном ядре, порядок, переполнение");
  std::atomic<int> core{-1};
  std::atomic<bool> current{false};
  std::atomic<TaskHandle_t> task{nullptr};
  core0Loop.post([&]() {
    core = xPortGetCoreID();
    current = core0Loop.isCurrent();
    task = currentTask();
  });
  CHECK_WAIT(task.load() != nullptr, 1000);
  CHECK_EQ(core.load(), 0);
  CHECK(current.load());
  CHECK(task.load() == core0Task);
  CHECK(!core0Loop.isCurrent());       // из задачи тестов — чужой цикл
  CHECK_EQ(core0Loop.core(), 0);
  CHECK_EQ(core1Loop.core(), 1);

  std::vector<int> order;
  std::atomic<bool> done{false};
  for (int i = 0; i < 20; i++) core1Loop.post([&order, i]() { order.push_back(i); });
  core1Loop.post([&]() { done = true; });
  CHECK_WAIT(done.load(), 1000);
  bool fifo = order.size() == 20;
  for (int i = 0; fifo && i < 20; i++) fifo = order[i] == i;
  CHECK(fifo);

  // Очередь на 4 задачи, без ожидания: 10 задач при занятом цикле → 6 потеряно
  LoopBlocker blocker;
  blocker.block(smallLoop);
  uint32_t droppedBefore = smallLoop.dropped();
  std::atomic<int> executed{0};
  int accepted = 0;
  for (int i = 0; i < 10; i++) accepted += smallLoop.post([&]() { executed++; }) ? 1 : 0;
  blocker.release();
  CHECK_WAIT(executed.load() == 4, 1000);
  vTaskDelay(pdMS_TO_TICKS(20));
  CHECK_EQ(accepted, 4);
  CHECK_EQ(executed.load(), 4);
  CHECK_EQ(smallLoop.dropped() - droppedBefore, 6u);
  endTest();
}

Subject<int> gHomed;   // глобальный Subject (не shared_ptr) с домашним циклом

void testHomedQueue() {
  beginTest("Домашний цикл, Delivery::Queue: next() с другого ядра");
  auto hs = Subject<int>::create(core0Loop);
  std::vector<int> got;
  std::atomic<int> cnt{0};
  std::atomic<bool> onHome{true};
  hs->subscribe([&](int v) {
    if (currentTask() != core0Task) onHome = false;
    got.push_back(v);
    cnt++;
  });
  uint32_t droppedBefore = core0Loop.dropped();
  for (int i = 1; i <= 50; i++) hs->next(i);
  CHECK_WAIT(cnt.load() == 50, 1000);
  bool inOrder = got.size() == 50;
  for (int i = 0; inOrder && i < 50; i++) inOrder = got[i] == i + 1;
  CHECK(inOrder);
  CHECK(onHome.load());
  CHECK_EQ(hs->get(), 50);
  CHECK_EQ(core0Loop.dropped() - droppedBefore, 0u);

  // update() с другого ядра
  hs->update();
  CHECK_WAIT(cnt.load() == 51, 1000);

  endTest();
}

void testHomedGlobal() {
  beginTest("Домашний цикл у глобального Subject (on()) и у Subject в shared_ptr");
  // Сначала такой же Subject в shared_ptr — для сравнения, если глобальный не сработает
  auto heap = Subject<int>::create();
  heap->on(core1Loop);
  std::atomic<int> hv{0};
  std::atomic<TaskHandle_t> ht{nullptr};
  auto hsub = heap->subscribe([&](int v) { ht = currentTask(); hv = v; });
  heap->next(41);
  CHECK_WAIT(hv.load() == 41, 1000);
  CHECK(ht.load() == core1Task);
  hsub->complete();

  // gHomed глобальный и помнит значение с прошлого прогона ('r'): повтор того же
  // значения был бы отсеян, поэтому значения зависят от номера прогона
  const int base = gRunNo * 100;
  gHomed.on(core1Loop);
  std::atomic<int> gv{0};
  std::atomic<TaskHandle_t> gt{nullptr};
  auto gsub = gHomed.subscribe([&](int v) { gt = currentTask(); gv = v; });
  gHomed.next(base + 42);
  CHECK_WAIT(gv.load() == base + 42, 1000);
  CHECK(gt.load() == core1Task);
  gHomed.next(base + 43);
  CHECK_WAIT(gv.load() == base + 43, 1000);
  gsub->complete();
  endTest();
}

void testHomedLatest() {
  beginTest("Домашний цикл, Delivery::Latest: только последнее значение");
  auto ls = Subject<int>::create(core0Loop, 100, Delivery::Latest);
  std::atomic<int> cnt{0}, last{0};
  ls->subscribe([&](int v) { cnt++; last = v; });

  LoopBlocker blocker;
  blocker.block(core0Loop);
  for (int i = 1; i <= 100; i++) ls->next(i);
  CHECK_EQ(core0Loop.pending(), 1u);   // в очереди одна задача на 100 значений
  blocker.release();
  CHECK_WAIT(last.load() == 100, 1000);
  vTaskDelay(pdMS_TO_TICKS(20));
  CHECK_EQ(cnt.load(), 1);
  CHECK_EQ(last.load(), 100);
  endTest();
}

void testSubscribeOnLoop() {
  beginTest("subscribe(cb, loop): колбэк в задаче выбранного цикла");
  Subject<int> s;   // без домашнего цикла: рассылка в задаче тестов
  std::atomic<TaskHandle_t> tDrv{nullptr}, t0{nullptr}, t1{nullptr};
  std::atomic<int> n0{0}, n1{0};
  s.subscribe([&](int) { tDrv = currentTask(); });
  s.subscribe([&](int) { t0 = currentTask(); n0++; }, core0Loop);
  s.subscribe([&](int, Subscription<int> *) { t1 = currentTask(); n1++; }, core1Loop);
  s.next(1);
  CHECK_WAIT(n0.load() == 1 && n1.load() == 1, 1000);
  CHECK(tDrv.load() == driverTask);
  CHECK(t0.load() == core0Task);
  CHECK(t1.load() == core1Task);

  // Latest для отдельной подписки
  Subject<int> s2;
  std::atomic<int> cnt{0}, last{0};
  s2.subscribe([&](int v) { cnt++; last = v; }, core1Loop, Delivery::Latest);
  LoopBlocker blocker;
  blocker.block(core1Loop);
  for (int i = 1; i <= 30; i++) s2.next(i);
  blocker.release();
  CHECK_WAIT(last.load() == 30, 1000);
  vTaskDelay(pdMS_TO_TICKS(20));
  CHECK_EQ(cnt.load(), 1);

  // BehaviorSubject: начальное значение приходит в цикл подписчика
  BehaviorSubject<int> b(9);
  std::atomic<int> bv{0};
  std::atomic<TaskHandle_t> bt{nullptr};
  b.subscribe([&](int v) { bt = currentTask(); bv = v; }, core0Loop);
  CHECK_WAIT(bv.load() == 9, 1000);
  CHECK(bt.load() == core0Task);
  endTest();
}

void testObserveOn() {
  beginTest("observeOn(loop): цепочка обрабатывается на другом ядре");
  auto src = Subject<int>::create();
  std::atomic<int> got{0};
  std::atomic<TaskHandle_t> filterTask{nullptr}, subTask{nullptr};
  src->observeOn(core1Loop)
     ->pipe({ [&](int v, auto) { filterTask = currentTask(); return v * 3; } })
     ->subscribe([&](int v) { subTask = currentTask(); got = v; });
  src->next(7);
  CHECK_WAIT(got.load() == 21, 1000);
  CHECK(filterTask.load() == core1Task);
  CHECK(subTask.load() == core1Task);
  endTest();
}

// Храним глобально. При повторном прогоне старый объект удаляется, а его таймер остаётся
// в core0Loop.async — проверяем, что такой таймер безопасно ничего не делает.
std::shared_ptr<Subject<bool>> gPinSubject;

void testFromPinHomed() {
  beginTest("fromPin() с домашним циклом: опрос пина на ядре 0");
  gPinSubject = Subject<bool>::create(core0Loop, 5);
  std::atomic<int> changes{0};
  std::atomic<bool> last{false}, onHome{true};
  gPinSubject->subscribe([&](bool v) {
    if (currentTask() != core0Task) onHome = false;
    last = v;
    changes++;
  });
  gPinSubject->fromPin(PIN_DIGITAL, INPUT_PULLUP);
  CHECK_WAIT(changes.load() >= 1 && last.load(), 1000);
  pinMode(PIN_DIGITAL, INPUT_PULLDOWN);
  CHECK_WAIT(!last.load(), 1000);
  CHECK(onHome.load());
  gPinSubject->complete();   // таймер продолжит тикать, но Subject больше ничего не рассылает
  vTaskDelay(pdMS_TO_TICKS(20));
  pinMode(PIN_DIGITAL, INPUT);
  endTest();
}

void testAction() {
  beginTest("Action: вызов функции на другом ядре с параметрами");
  Action<int, String> act(core1Loop);
  std::atomic<int> sum{0}, calls{0}, calls2{0};
  std::atomic<TaskHandle_t> th{nullptr};
  String lastS;
  act.subscribe([&](int a, String s) { sum += a; lastS = s; th = currentTask(); calls++; });
  auto h2 = act.subscribe([&](int, String) { calls2++; });

  // Вызов с ядра 0, одинаковые параметры дважды — оба доставляются
  core0Loop.post([&]() { act.call(5, "five"); act.call(5, "five"); });
  CHECK_WAIT(calls.load() == 2, 1000);
  CHECK_EQ(sum.load(), 10);
  CHECK(lastS == "five");
  CHECK(th.load() == core1Task);
  CHECK_EQ(calls2.load(), 2);

  h2->complete();
  CHECK(act.call(1, "x"));
  CHECK_WAIT(calls.load() == 3, 1000);
  vTaskDelay(pdMS_TO_TICKS(10));
  CHECK_EQ(calls2.load(), 2);

  Action<> ping(core0Loop);
  std::atomic<int> pings{0};
  ping.subscribe([&]() { pings++; });
  ping.call();
  CHECK(ping.callWait(pdMS_TO_TICKS(100)));
  CHECK_WAIT(pings.load() == 2, 1000);

  Action<int> local;   // без домашнего цикла — синхронно
  int lv = 0;
  local.subscribe([&](int v) { lv = v; });
  local.call(3);
  CHECK_EQ(lv, 3);
  endTest();
}


void testRemote() {
  beginTest("Remote: вызов на другом ядре с ответом (ниже 1 ожидаемое сообщение об ошибке)");
  Remote<int(int, int)> add(core1Loop);
  std::atomic<TaskHandle_t> ht{nullptr};
  add.handle([&](int a, int b) { ht = currentTask(); return a + b; });

  // Ответ в задаче core0Loop
  std::atomic<int> r0{0};
  std::atomic<TaskHandle_t> rt0{nullptr};
  CHECK(add.call(2, 3, [&](int v) { rt0 = currentTask(); r0 = v; }, core0Loop));
  CHECK_WAIT(r0.load() == 5, 1000);
  CHECK(ht.load() == core1Task);
  CHECK(rt0.load() == core0Task);

  // Ответ в домашнем цикле
  std::atomic<int> r1{0};
  std::atomic<TaskHandle_t> rt1{nullptr};
  CHECK(add.call(10, 20, [&](int v) { rt1 = currentTask(); r1 = v; }));
  CHECK_WAIT(r1.load() == 30, 1000);
  CHECK(rt1.load() == core1Task);

  // Синхронно из задачи тестов
  auto sum = add.callSync(pdMS_TO_TICKS(500), 7, 8);
  CHECK(sum.has_value());
  if (sum) CHECK_EQ(*sum, 15);

  // Синхронно из домашнего цикла — выполняется сразу, без очереди
  std::atomic<int> inHome{0};
  core1Loop.post([&]() {
    auto v = add.callSync(pdMS_TO_TICKS(100), 1, 1);
    inHome = v ? *v : -1;
  });
  CHECK_WAIT(inHome.load() == 2, 1000);

  // Результат String, вызов из ядра 0 на ядро 1 и ответ обратно
  Remote<String(int)> name(core1Loop);
  name.handle([](int i) { return String("ch") + String(i); });
  std::atomic<bool> gotName{false};
  String nameValue;
  core0Loop.post([&]() {
    name.call(5, [&](String v) { nameValue = v; gotName = true; }, core0Loop);
  });
  CHECK_WAIT(gotName.load(), 1000);
  CHECK(nameValue == "ch5");

  // Без обработчика и с исключением — пустой результат, ответа нет
  Remote<int()> none(core1Loop);
  CHECK(!none.callSync(pdMS_TO_TICKS(200)).has_value());
  Remote<int()> bad(core1Loop);
  bad.handle([]() -> int { throw std::runtime_error("ожидаемая ошибка обработчика Remote"); });
  CHECK(!bad.callSync(pdMS_TO_TICKS(200)).has_value());

  // Таймаут: домашний цикл занят. Опоздавший вызов выполнится позже — без сбоев
  LoopBlocker blocker;
  blocker.block(core1Loop);
  uint32_t t0 = millis();
  auto late = add.callSync(pdMS_TO_TICKS(50), 1, 2);
  uint32_t spent = millis() - t0;
  CHECK(!late.has_value());
  CHECK(spent >= 40 && spent < 300);
  blocker.release();
  endTest();
}

void testStress() {
  beginTest("Нагрузка: два ядра одновременно (next, subscribe, complete, get)");
  auto st = Subject<int>::create(core0Loop);
  std::atomic<int> received{0};
  st->subscribe([&](int) { received++; });

  // Ядро 0 рассылает 2000 значений, задача тестов в это время подписывается/отписывается
  std::atomic<bool> done{false};
  core0Loop.post([&]() {
    for (int i = 1; i <= 2000; i++) {
      st->next(i);
      if (i % 200 == 0) vTaskDelay(1);
    }
    done = true;
  });
  int churn = 0;
  while (!done.load()) {
    auto sub = st->subscribe([](int) {});
    volatile int v = st->get();
    (void)v;
    sub->complete();
    churn++;
    taskYIELD();
  }
  CHECK_EQ(received.load(), 2000);
  CHECK(churn > 0);
  INFO("подписок/отписок за время рассылки: %d\n", churn);

  // Два отправителя в один Subject (Queue): чётные из задачи тестов (приоритет 1),
  // нечётные из core1Loop (приоритет 2), оба на ядре 1. Очередь переполняется; при
  // ожидании 10 мс задача с низким приоритетом может не дождаться места — освободившееся
  // место FreeRTOS отдаёт задаче с более высоким приоритетом. Поэтому здесь ждём без ограничения.
  auto mx = Subject<int>::create(core0Loop);
  std::atomic<int> cnt{0};
  mx->subscribe([&](int) { cnt++; });
  TickType_t oldTimeout = core0Loop.postTimeout();
  core0Loop.setPostTimeout(portMAX_DELAY);
  uint32_t droppedBefore = core0Loop.dropped();
  std::atomic<bool> sent1{false};
  core1Loop.post([&]() {
    for (int i = 0; i < 500; i++) mx->next(2 * i + 1);
    sent1 = true;
  });
  for (int i = 0; i < 500; i++) mx->next(2 * i);
  CHECK_WAIT(sent1.load(), 3000);
  CHECK_WAIT(cnt.load() == 1000, 3000);
  CHECK_EQ(cnt.load(), 1000);
  CHECK_EQ(core0Loop.dropped() - droppedBefore, 0u);
  core0Loop.setPostTimeout(oldTimeout);

  // То же с ожиданием 10 мс — потери возможны, только для информации
  auto mx2 = Subject<int>::create(core0Loop);
  std::atomic<int> cnt2{0};
  mx2->subscribe([&](int) { cnt2++; });
  uint32_t dropped2 = core0Loop.dropped();
  std::atomic<bool> sent2{false};
  core1Loop.post([&]() {
    for (int i = 0; i < 500; i++) mx2->next(2 * i + 1);
    sent2 = true;
  });
  for (int i = 0; i < 500; i++) mx2->next(2 * i);
  waitFor([&]() { return sent2.load(); }, 3000);
  waitFor([&]() { return cnt2.load() + (int)(core0Loop.dropped() - dropped2) >= 1000; }, 3000);
  INFO("с ожиданием 10 мс: доставлено %d из 1000, потеряно %lu (справочно)\n",
       cnt2.load(), (unsigned long)(core0Loop.dropped() - dropped2));
  endTest();
}

void testMemory() {
  beginTest("Память: нет утечек при межъядерной доставке");
  auto batch = [](int n) {
    auto s = Subject<int>::create(core0Loop);
    Action<int> act(core1Loop);
    std::atomic<int> a{0}, b{0};
    s->subscribe([&](int) { a++; });
    s->subscribe([&](int) { b++; }, core1Loop);
    act.subscribe([&](int) { b++; });
    for (int i = 1; i <= n; i++) { s->next(i); act.call(i); }
    bool ok = waitFor([&]() { return a.load() == n && b.load() == 2 * n; }, 3000);
    syncLoops();   // a, b, s и act живут до конца лямбды: дождаться всех задач
    return ok;
  };
  CHECK(batch(300));   // прогрев: первые выделения памяти (очереди, буферы) не считаем
  uint32_t before = ESP.getFreeHeap();
  bool allOk = true;
  for (int i = 0; i < 5; i++) allOk = batch(300) && allOk;   // 5 × 300 значений и вызовов
  CHECK(allOk);
  uint32_t after = ESP.getFreeHeap();
  int32_t diff = (int32_t)before - (int32_t)after;
  INFO("свободно до: %lu, после: %lu, разница: %ld байт\n", (unsigned long)before, (unsigned long)after, (long)diff);
  // Утечка даже 4 байт на сообщение дала бы больше 6 КБ (1500 значений + 1500 вызовов)
  CHECK(diff < 4096);
  endTest();
}

// ---------------------------------------------------------------------------

void runAll() {
  gPass = gFail = gTests = gTestsFailed = 0;
  gRunNo++;
  uint32_t start = millis();
  if (!gQuiet) {
    Serial.println("\n========== subjectLab: тесты subject.cpp ==========");
    Serial.printf("Чип: %s, ядер: %d, свободно памяти: %lu байт\n", ESP.getChipModel(), ESP.getChipCores(), (unsigned long)ESP.getFreeHeap());
  }

  testNext();
  testForceUpdate();
  testSubscriptions();
  testComplete();
  testPipe();
  testFilters();
  testAvgMedian();
  testForceUpdateFilter();
  testBehavior();
  testExceptions();
  testTypes();
  testFromPinAsync();
  testAsyncBasic();
  testAsyncMutation();
  testAsyncJob();
  testCoreLoop();
  testHomedQueue();
  testHomedGlobal();
  testHomedLatest();
  testSubscribeOnLoop();
  testObserveOn();
  testFromPinHomed();
  testAction();
  testRemote();
  testStress();
  testMemory();

  if (gQuiet) {
    Serial.printf("прогон %2d: тестов провалено %d, ошибок %d, %lu мс, свободно памяти %lu\n",
                  gRunNo, gTestsFailed, gFail, millis() - start, (unsigned long)ESP.getFreeHeap());
    return;
  }
  Serial.println("\n==================================================");
  Serial.printf("Тестов: %d, провалено: %d | проверок: %d, ошибок: %d | %lu мс\n",
                gTests, gTestsFailed, gPass + gFail, gFail, millis() - start);
  Serial.println(gFail == 0 ? "ИТОГ: ВСЁ OK" : "ИТОГ: ЕСТЬ ОШИБКИ");
  Serial.println("Отправьте 'r' — запустить ещё раз, 'l' — 20 прогонов подряд (ловить редкие сбои).");
}

// Серийный прогон: печатаются только ошибки, в конце — какие тесты и сколько раз падали
void runSeries(int runs) {
  for (int i = 0; i <= MAX_TESTS; i++) gFailRuns[i] = 0;
  Serial.printf("\n========== серия из %d прогонов ==========\n", runs);
  int badRuns = 0;
  gQuiet = true;
  for (int r = 1; r <= runs; r++) {
    runAll();
    if (gFail) badRuns++;
  }
  gQuiet = false;
  Serial.println("------------------------------------------");
  if (badRuns == 0) {
    Serial.printf("ИТОГ СЕРИИ: все %d прогонов без ошибок\n", runs);
  } else {
    Serial.printf("ИТОГ СЕРИИ: прогонов с ошибками %d из %d\n", badRuns, runs);
    for (int t = 1; t <= gTests && t <= MAX_TESTS; t++)
      if (gFailRuns[t]) Serial.printf("  [%2d] %s — упал %d раз\n", t, gNames[t], gFailRuns[t]);
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500);   // успеть открыть монитор порта
  driverTask = currentTask();

  xTaskCreatePinnedToCore(coreLoopTask, "core0Loop", 6144, &core0Loop, 2, &core0Task, 0);
  xTaskCreatePinnedToCore(coreLoopTask, "core1Loop", 6144, &core1Loop, 2, &core1Task, 1);
  xTaskCreatePinnedToCore(coreLoopTask, "smallLoop", 4096, &smallLoop, 2, &smallTask, 0);
  waitFor([]() { return core0Loop.task() && core1Loop.task() && smallLoop.task(); });

  runAll();
}

void loop() {
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'r' || c == 'R') runAll();
    if (c == 'l' || c == 'L') runSeries(20);
  }
  delay(10);
}
