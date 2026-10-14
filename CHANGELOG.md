# Изменения

Формат: [Keep a Changelog](https://keepachangelog.com/ru/1.1.0/), версии — [SemVer](https://semver.org/lang/ru/).
В каждом разделе отдельно отмечено, что меняет поведение уже написанного кода.

## [Unreleased]

### Добавлено
- `AGENTS.md` / `CLAUDE.md` — правила для агентов, которые работают с библиотекой.
- `tools/check_docs.py` и GitHub Actions: сборка самотеста и проверка, что каждое
  публичное имя из `src/*.h` описано в `docs/API.md`.
- В `docs/API.md` описаны поля `Job` и `CallbackParams`, `Job::setActive`,
  `Job::attachUntilFn/attachWhileFn`, `Subscription::call/deliver`.

## [1.1.0] — 2026-10-14

### Добавлено
- `Remote<R(Args...)>` — вызов функции на другом ядре с ответом: `call(args..., reply, replyLoop)`,
  `call(args..., reply)`, `callSync(timeout, args...)` → `std::optional<R>`.
- `Async::queue()`, `Async::size()`, `Job::resume`.
- `CoreLoop::executed()`, `lastRun()`, `postTimeout()`.

### Исправлено
- `Async`: `breakFn` и `clearAll()` из колбэка во время `tick()` портили память;
  `breakFn` мог отменить чужую задачу с тем же ключом; `untilFn/whileFn/pause` у копии
  `Job` обращались к удалённому итератору.
- Таймер `fromPin` и отложенные задачи `Subject` больше не обращаются к удалённому объекту.
- Вызовы `Action`, оставшиеся в очереди после удаления объекта, ничего не делают.

### Меняет поведение
- `Async`: задачи выполняются в порядке добавления (раньше — по внутреннему ключу).
- `Async` нельзя копировать.
- `Job` по умолчанию: `run()` ничего не делает (раньше бросал исключение).

## [1.0.0] — 2026-10-14

Первая версия библиотеки: `Subject`, `BehaviorSubject`, `SubFilters`, `CoreLoop`,
`Action`, `Async`; самотест для ESP32.

### Меняет поведение (по сравнению с копией `subject.cpp` в gateKeeper)
- Первое значение доставляется всегда, даже если равно значению по умолчанию
  (например, первое `next(false)` у `Subject<bool>`).
- `BehaviorSubject(5)`, затем `next(0)` — значение доставляется.
