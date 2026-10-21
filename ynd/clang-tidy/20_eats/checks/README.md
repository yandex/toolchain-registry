# Проверки clang-tidy для кодстайла Еды

Исходники проверок `eats-*`, которые пакет `ynd/clang-tidy/20_eats` собирает в
отдельный `clangTidyEatsModule`. Устройство пакета описано в
[`toolchains/docs/clang_tidy.md`](../../../../docs/clang_tidy.md).

| Файл | Назначение |
|---|---|
| `tidy_module_eats.cpp` | регистрирует имена проверок |
| `tests/test_<имя>.cpp` | примеры нарушений и чистого кода с разметкой |
| `test_checks.json` | маппинг «тест-файл → проверка» |
| `test_framework.py` | прогоняет clang-tidy на тест-файлах и сверяет предупреждения с разметкой |
| `test_catalog.py` | тест каталога: у каждой проверки есть тест-файл, маппинг не устарел |
| `test_test_framework.py` | тесты самого фреймворка |
| `CMakeLists.txt` | сборка проверок плагином к системному clang-tidy 20 |

## Как добавить проверку

1. Добавь `<имя>_check.cpp` и `<имя>_check.h`. Имя проверки начинается с `eats-`.
2. Зарегистрируй проверку в `tidy_module_eats.cpp`.
3. Добавь исходники в `eats_check_srcs` в `../unwrap/ix.sh`.
4. Напиши `tests/test_<имя>.cpp`: каждое нарушение и каждый похожий, но чистый
   случай, включая подавление через `NOLINT`.
5. Добавь в `test_checks.json` строку `"test_<имя>.cpp": "eats-<имя>"`. Одной
   проверке можно сопоставить несколько тест-файлов.
6. Прогони тесты каталога и проверок (ниже).

Разметка — комментарий на строке, куда указывает предупреждение:

```cpp
} catch (...) { // EXPECT-WARNING: пустой обработчик
} catch (...) { // EXPECT-NO-WARNING: обработчик пробрасывает исключение
f(a, b);        // EXPECT-WARNING-COUNT: 2: два независимых нарушения
```

- `EXPECT-WARNING[: причина]` — ровно одно предупреждение проверки.
- `EXPECT-WARNING-COUNT: N[: причина]` — ровно N предупреждений, N > 0.
- `EXPECT-NO-WARNING[: причина]` — ни одного. Строка без разметки тоже не должна
  получать предупреждений; `EXPECT-NO-WARNING` нужен, чтобы явно показать
  похожий чистый случай.

После маркера нужен конец строки или `:`, поэтому упоминание маркера в тексте
комментария разметкой не считается. Разметка в локальных заголовках, которые
тест подключает через `#include "..."`, тоже учитывается.

## Как запустить тесты локально

Команды выполняются из корня Аркадии.

Тест каталога и тесты фреймворка не требуют clang-tidy:

```bash
python3 -m unittest discover -s toolchains/ynd/clang-tidy/20_eats/checks -p 'test_*.py'
```

Тесты проверок с плагином к системному clang-tidy 20:

```bash
cmake -S toolchains/ynd/clang-tidy/20_eats/checks -B /tmp/eats-tidy-plugin
cmake --build /tmp/eats-tidy-plugin
clang-tidy-20 --load=/tmp/eats-tidy-plugin/libEatsTidyPlugin.so \
    --checks='-*,eats-*' --list-checks
python3 toolchains/ynd/clang-tidy/20_eats/checks/test_framework.py \
    --plugin /tmp/eats-tidy-plugin/libEatsTidyPlugin.so
```

Если LLVM 20 установлен не в `/usr/lib/llvm-20`, передай cmake `-DLLVM_DIR` и
`-DClang_DIR`.

Тесты проверок на собранном бинаре пакета `20_eats`, где проверки вкомпилированы:

```bash
python3 toolchains/ynd/clang-tidy/20_eats/checks/test_framework.py \
    --clang-tidy <путь к clang-tidy из 20_eats>
```

Отдельные файлы передаются позиционно, например
`test_framework.py --plugin ... toolchains/ynd/clang-tidy/20_eats/checks/tests/test_empty_catch_all.cpp`;
файл должен быть в `test_checks.json`. Код выхода ненулевой, если упал хотя бы один тест.

## Ограничения

- Стандарт тест-файлов — `-std=c++20`, другой задаётся через `--std`.
  Проверки работают только для C++.
- Стандартные заголовки берутся из окружения clang-tidy. Системный
  clang-tidy-20 находит их сам; бинарю из ix-пакета вне `ya` их может не
  хватить, тогда передай пути через `--extra-arg=-isystem<каталог>`.
- В системных заголовках clang-tidy предупреждений не выдаёт. Код, который в
  проверке должен считаться системным, подключай из локального каталога через
  `--extra-arg=-isystem...`, а не из настоящих заголовков.
- Фреймворк запускает clang-tidy с пустым `--config`, поэтому `.clang-tidy`
  из каталогов выше на прогон не влияет.
- Compile database не используется, только `-std` и `--extra-arg`, поэтому
  тест-файл должен компилироваться сам по себе. Ошибка
  компиляции роняет тест: AST неполный, и отсутствие предупреждений ничего
  не доказывает.
- `--load` работает только с системным clang-tidy 20: ix-сборка идёт с
  `CLANG_PLUGIN_SUPPORT=FALSE`.
- В autocheck эти тесты не запускаются, их прогоняют локально перед PR.
