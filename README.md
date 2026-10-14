# subjectLab

Реактивные потоки значений, таймеры и межъядерные вызовы для ESP32 (Arduino, PlatformIO).

- `Subject<T>`: источник значений с подписками, фильтрами (`pipe`), опросом пинов (`fromPin`)
- `BehaviorSubject<T>`: то же, новый подписчик сразу получает текущее значение
- `SubFilters<T>`: `median`, `avg`, `matchTo`, `filter`, `forceUpdate`
- `CoreLoop`: очередь задач ядра; `next()` и подписки безопасны с любого ядра
- `Action<Args...>`: вызов функции на другом ядре с параметрами
- `Remote<R(Args...)>`: вызов функции на другом ядре с ответом (асинхронно или с ожиданием)
- `Async`: таймеры `delay` / `repeat`

Полное описание и примеры: [docs/API.md](docs/API.md).

## Подключение к проекту

В `platformio.ini` проекта:

```ini
lib_deps =
    https://github.com/moorween/subjectLab.git#v1.1.0
```

`#v1.1.0` фиксирует версию: проект не изменится, пока вы сами не поменяете тег.
Без тега будет браться последний коммит ветки `main` (удобно, пока библиотека
активно меняется, но сборка может измениться без вашего ведома).

В коде:

```cpp
#include <Subject.h>   // подключает также CoreLoop.h и Async.h

CoreLoop core1Loop;
Action<int> blink(core1Loop);
Remote<float(int)> readAdc(core1Loop);

void core1Task(void*) { for (;;) core1Loop.run(pdMS_TO_TICKS(5)); }

void setup() {
  Serial.begin(115200);
  xTaskCreatePinnedToCore(core1Task, "core1", 4096, nullptr, 2, nullptr, 1);
  blink.subscribe([](int n) { Serial.printf("blink %d на ядре %d\n", n, xPortGetCoreID()); });
  readAdc.handle([](int pin) { return analogRead(pin) * 3.3f / 4095; });
}

void loop() {
  static int n = 0;
  blink.call(n++);                                        // выполнится на ядре 1
  auto v = readAdc.callSync(pdMS_TO_TICKS(100), 34);      // ждём ответ с ядра 1
  if (v) Serial.printf("ADC34 = %.2f В\n", *v);
  delay(1000);
}
```

Требования: ESP32, Arduino-ESP32 3.x (pioarduino), C++17, исключения включены (по умолчанию).

## Самотест на плате

Корень репозитория — одновременно проект PlatformIO, который собирает
`examples/selftest` на ESP32 и проверяет все функции библиотеки (26 тестов).

1. Открыть папку репозитория в VS Code с PlatformIO.
2. Ничего не подключать. **GPIO27 и GPIO33 должны быть свободны**: тест `fromPin`
   переключает их внутренние подтяжки.
3. Upload and Monitor (115200). В конце будет `ИТОГ: ВСЁ OK` или список проваленных
   проверок с номерами строк.

Команды в мониторе порта: `r` — прогнать ещё раз, `l` — серия из 20 прогонов (печатаются
только ошибки, в конце — какие тесты и сколько раз падали). Ожидаемые сообщения об
ошибках: два в тесте «Исключения» и одно в тесте `Remote` — так и должно быть.

Если не дождались события, печатается диагностика: состояние задач всех циклов, длина
очередей, сколько задач выполнено и потеряно, время ответа на пробную задачу, и пришло ли
событие с опозданием.

| № | Тест |
|---|---|
| 1–4 | `next`, отсев повторов, `update`, `forceUpdate`, обе формы `subscribe`, `Subscription::complete`, `Subject::complete` (в т.ч. из колбэка) |
| 5–8 | `pipe` (цепочки, ветвление, вложенность), `rejectNext`, `matchTo`, `filter`, `avg`, `median`, `SubFilters::forceUpdate` |
| 9–11 | `BehaviorSubject`, исключения в подписчиках и фильтрах, типы `String` и структура |
| 12 | `fromPin` через `Async`: `digitalRead` (GPIO27), `analogRead` (GPIO33) |
| 13 | `Async`: `delay`, `repeat`, `queue`, отмена из колбэка, несколько задач с одинаковой задержкой |
| 14 | `Async`: `clearAll()`, добавление и отмена своих и чужих задач из колбэков, удаление `Async` из колбэка |
| 15 | `Async`: `Job` снаружи — `run()`, `breakFn()`, `pause`/`resume`, `untilFn`, `whileFn`, `Job` после удаления `Async` |
| 16 | `CoreLoop`: выполнение на нужном ядре, порядок FIFO, переполнение очереди |
| 17 | Домашний цикл, `Delivery::Queue`: 50 значений с другого ядра, `update()` |
| 18 | Домашний цикл у глобального Subject (`on()`) и у Subject в `shared_ptr` |
| 19 | `Delivery::Latest`: 100 значений при занятом цикле → одна задача |
| 20–21 | `subscribe(cb, loop)`, `observeOn(loop)`, `BehaviorSubject` с циклом |
| 22 | `fromPin` с опросом на ядре 0; при повторном прогоне — удаление объекта с работающим таймером |
| 23 | `Action`: вызов с ядра 0 на ядре 1, несколько параметров, отписка, `Action<>` |
| 24 | `Remote`: ответ в выбранный цикл и в домашний, `callSync` из чужой задачи и из домашнего цикла, `String`, нет обработчика, исключение, таймаут |
| 25 | Нагрузка: 2000 значений на ядре 0 при одновременных подписках/отписках; два отправителя разного приоритета |
| 26 | Память: нет утечек на 3000 межъядерных сообщениях |

## Выпуск новой версии

1. Внести изменения, прогнать самотест на плате.
2. Поднять `version` в `library.json` (например, `1.2.0`).
3. Закоммитить и поставить тег:

   ```bash
   git commit -am "Описание изменений"
   git tag v1.2.0
   git push && git push --tags
   ```

4. В проектах поменять тег в `lib_deps`.

## Структура

```
library.json          описание библиотеки для PlatformIO
src/Subject.h         Subject, BehaviorSubject, SubFilters, Action, Remote
src/CoreLoop.h        CoreLoop, Delivery
src/Async.h, .cpp     таймеры
docs/API.md           описание API
examples/selftest/    самотест (собирается из корня репозитория)
platformio.ini        только для самотеста
```
