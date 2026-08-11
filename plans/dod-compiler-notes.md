# Заметки об архитектуре: codegen графа, cache-friendly layout, ECS

Сводка дискуссии о трёх пересекающихся темах:
1. **Кнопка Compile** — codegen графа cc-utils в статический C++ / binary.
2. **Cache-friendly дизайн компилятора** — как сделать AST/IR/table
   cache-friendly и data-oriented, а не "vtable-tree-of-shared-ptr".
3. **ECS (flecs) под капот** — стоит ли использовать generic ECS-движок
   вместо ручного SoA-layout.

По ходу обсуждения всплыла **аналогия с DWARF `.debug_info`** — плоский
preorder-encoded массив DIE с null-маркером конца siblings. Это очень
точное попадание и оно раскрывает несколько разных плоских layout'ов
деревьев, у каждого свои trade-off'ы. Отдельный раздел ниже.

Тон документа — разговорный, без сокращений аргументации. Это ссылочный
материал, к которому предполагается возвращаться, а не executive summary.

---

## Часть 1. Кнопка Compile: codegen графа в статический код

### 1.1. Постановка задачи

Визуальный граф в cc-utils сейчас **интерпретируется**: `runner::pull`
ходит по рёбрам, через vtable вызывает `activate`, протаскивает
`any_value`. Это отлично для интерактивной отладки, но имеет цену:

- vtable-dispatch на каждом activate (indirect branch, branch predictor)
- `any_value` = type-erased carrier с heap-аллокацией значения
- поиск `node_factory` по `type_id` через string-keyed map
- динамическая диспетчеризация рёбер

В production-компиляторе этого оверхеда **нет**, потому что pipeline
захардкожен в C++ напрямую. Кнопка Compile в нашем случае могла бы
**сгенерировать** эквивалентный статический код из визуального графа —
получить best of both worlds: визуальное редактирование + zero-cost
runtime.

Это ровно та идея, на которой построены Halide, TVM, XLA — "граф как
спецификация, codegen как специализация".

### 1.2. Что у нас уже есть для codegen

Архитектурно мы готовы на ~70%:

- **Топология DAG** есть: `graph::nodes() + edges()` → топосорт → эмиссия
  вызовов в правильном порядке.
- **Типизированные слоты**: каждый slot знает `type_descriptor_t`. Это
  маппится на C++-тип аргумента функции.
- **`node_factory::type_id`** — стабильный идентификатор, маппится на
  C++-имя функции или класса.
- **`property_schema()`** — декларативная схема свойств, маппится на
  `constexpr` параметры.
- **`<requires>` секция** — список плагинов-провайдеров, маппится на
  набор `#include` + флаги линковки.

### 1.3. Чего не хватает

Придётся ввести **второй контракт параллельно с `activate`**:

```cpp
class node {
  // Existing: runtime interpretation.
  virtual auto activate(std::span<const input_pair>,
                        std::span<output_pair>,
                        const activate_context&) -> activate_result = 0;

  // New: compile-time code emission.
  virtual auto codegen(codegen_ctx& ctx) -> codegen_result = 0;
};
```

Где `codegen()` возвращает не значение, а **C++ фрагмент** — выражение
или statement-list — который встраивается в генерируемый .cpp. Например
для `basic.text.from_file`:

```cpp
// activate() — интерпретация:
auto activate(...) -> activate_result override {
  std::ifstream in(path_);
  // ... read file, return content
}

// codegen() — эмиссия:
auto codegen(codegen_ctx& ctx) -> codegen_result override {
  // Возвращает C++ код, который делает то же самое, но со статическим
  // path (вставленным как string literal) и записывает результат в
  // переменную с известным именем, чтобы downstream-узлы могли её читать.
  return ctx.emit_statement(
    "std::string " + ctx.output_var("out") + " = "
    "[&] { std::ifstream _in(\"" + path_ + "\"); "
    "std::ostringstream _ss; _ss << _in.rdbuf(); return _ss.str(); }();"
  );
}
```

Параллельно нужно расширить `type_registry`:

```cpp
class type_registry {
  // existing: runtime descriptor
  virtual auto name_of(type_descriptor_t) -> std::string_view = 0;
  virtual auto descriptor_of_name(std::string_view) -> type_descriptor_t = 0;

  // new: C++ name for codegen
  virtual auto cpp_name_of(type_descriptor_t) -> std::string_view = 0;
};
```

Чтобы вместо `any_value` эмитить конкретный тип (`std::string`,
`std::filesystem::path`, `long`, ...).

### 1.4. Реалистичные фазы

| Фаза | Что делает | Effort | Win |
|------|------------|--------|-----|
| **1** | Эмитит .cpp с вызовом `activate()` каждого узла в topo-сорте. Компилируется отдельно. | ~1 день | Standalone executable без host UI. Plugin-loading + visual editor разделены. |
| **2** | Type-aware codegen: вместо `any_value` эмитит конкретные типы через `cpp_name_of(descriptor)`. Свойства как `constexpr`. | ~1 неделя | Убирает type erasure, ~5–10% speedup на cold start. |
| **3** | Plugin-as-static-library: вместо `dlopen` — прямой вызов функции. Codegen эмитит `if (type_id == "...") factory::create_static()` | ~1 неделя | Полностью статический бинарь, никаких .so. |
| **4** | Fusion соседних узлов + `std::simd` для batch-операций. | ~1–3 месяца | Реальный SIMD-выигрыш — но **только для подходящего домена**. |
| **5** | Halide-style schedules: пользователь в графе annotирует hints (`vectorize`, `tile`, `parallel`), codegen исследует пространство. | research project | TVM-tier результат. |

### 1.5. Где Compile даст реальный выигрыш

**Даст (даже на фазах 1–3):**
- Standalone-артефакт: можно дать .pipeline + .cpp, другому не нужен хост.
- Убрать dynamic dispatch для холодного старта конвейера.
- Plugin-as-static-library — полностью статический бинарь без .so.

**Не даст на текущем tl-компиляторе (фазы 4–5):**
- Граф короткий (4–6 узлов), каждый узел уже хорошо оптимизирован внутри.
- Между узлами нет fusion-opportunities: парсер и NASM-эмиттер принципиально
  разные операции, их в одну SIMD-функцию не свернуть.
- "Hot loops" живут **внутри** узлов (lexer loop, parser recursion, NASM
  encoding), не в рёбрах между ними.

**Даст на другом домене (где-нибудь фазы 4–5):**
- Image/audio/DSP pipeline: узлы = маленькие kernel'ы (convolve, threshold,
  mix), codegen их **fuses** в один проход с auto-vectorization.
- ML inference (TVM-style).
- Scientific simulations.

**TL;DR для Compile-кнопки:** фазы 1–3 делаем — это дешево и полезно.
Фаза 4+ имеет смысл только если сменить домен с source-to-source compiler
на что-то с batch-данными.

### 1.6. std::execution (C++26 senders/receivers) — ожидания vs реальность

**Ожидание**: "EBveйший static zero-cost conveyor by NVIDIA".

**Реальность**: `std::execution` (P2300) — это про **scheduler'ы и
parallelism**, не про SIMD. Он даёт:
- Lazy графы вычислений (как наш, но compile-time composability)
- Composable schedulers (thread pool, GPU, NIC)
- Type-erased pipelines через `any_sender_of`

Он **не** генерирует SSE/AVX. Для SIMD нужен отдельный механизм:
- `std::simd` (P2650, тоже C++26)
- явные intrinsics в эмитированном коде
- autovectorizer gcc/clang (работает для регулярных циклов)

"Static zero-cost conveyor" = **связка** `std::execution` (параллелизм
между узлами) + `std::simd` (внутри узлов с batch-данными). Не один
механизм.

---

## Часть 2. Cache-friendly дизайн компилятора

### 2.1. Почему классический компилятор анти-cache

Традиционный компилятор:
- AST = pointer-based дерево из полиморфных узлов
- Каждый узел — отдельная heap allocation (cold, ~64 байт + vtable)
- Каждый визитор = vtable dispatch через указатель
- Symbols = hash table per scope (chain-walking on collision)
- IR = граф с def-use chains (pointer-chasing)

Это **наследие Lisp/SML-эпохи**, когда "pointer = абстракция", а память
считалась бесплатной. На современных CPU (где L1-miss = 50 cycles, branch
mispredict = 15 cycles) это катастрофа:

| Операция | Hot path | Cost |
|----------|----------|------|
| `node->left->right->value` | 3 cache misses | ~150 cycles |
| `vtable call on Node*` | indirect branch | ~15 cycles (mispredicted) |
| `scope.lookup("foo")` | hash + string compare | ~30–80 cycles |
| `dynamic_cast<Node*>` | RTTI walk | ~50–200 cycles |

### 2.2. Главный трюк: arena + индексы + SoA

Заменить `Node*` на `NodeId = uint32_t`. Все узлы лежат в одном
`std::vector`. AST из указательного дерева становится **плоским
массивом структур**, а иногда — **массивом из плоских столбцов**
(Structure-of-Arrays):

```cpp
// Плохо (классический AoS):
struct Node {
  Kind kind;                  // 1 байт
  SourceLoc loc;              // 8 байт
  std::vector<Node*> kids;    // 24 байта + отдельная heap allocation
  TypeRef type;               // 4 байта
  Op op;                      // 4 байта (только для Expr)
  // + padding → 48+ байт, половина пустая для не-Expr
};
std::vector<std::unique_ptr<Node>> nodes;  // каждый new отдельно

// Хорошо (SoA):
struct NodeTable {
  std::vector<Kind>        kinds;   // 1 байт/узел, prefetcher счастлив
  std::vector<SourceLoc>   locs;    // холодные, только для ошибок
  std::vector<KidSpan>     kids;    // { offset, len } в отдельный пул
  std::vector<TypeRef>     types;   // только после typecheck
  std::vector<Op>          ops;     // только Expr
};
```

Когда проход "посчитать все BinaryExpr" — читаем только `kinds` (по 1
байту подряд). hardware prefetcher работает идеально, auto-vectorizer gcc
тоже может. На большом AST скорость проходов в 5–10× выше, чем по
vtable-tree.

### 2.3. Кто так делает в реальности

- **Zig** — Andrew Kelley подробно писал. Arena + indices везде,
  единственный из современных компиляторов такого класса.
- **rustc** — `IndexVec<NodeId, T>` везде. AST/HIR/ty/IR — всё плоские
  таблицы. Медленнее Zig, но это из-за borrow-checking, не layout.
- **Roslyn** (C#) — "green tree" immutable в arena + "red tree" lazy view.
  Symbol tables band-by-band.
- **Go compiler** — slice-based AST. Компилирует большие программы очень
  быстро.
- **LuaJIT** — Mike Pall писал про "compiler as data": байткод линейный,
  не древесный.
- **Clang** — `llvm::BumpPtrAllocator` для AST (не contiguous, но arena —
  без per-node free).
- **Crafting Interpreters** (Bob Nystrom), часть про clox — самый
  доступный источник для практика.

### 2.4. По каждой боли

**AST-деревья →** arena + NodeId. Дети — range в отдельном пуле
(`KidSpan { uint32_t offset, len }`). Ходить по дереву = итерация по
массиву с visited-маской.

**Графы →** Compressed Sparse Row (CSR). Это формат из научных вычислений:
один массив рёбер + offsets по узлам. Деф-юз цепочки в SSA ложатся
идеально. В LLVM так и сделано (с оговорками).

**Динамические аллокации →** arena per-compilation-unit. Один bump
allocator, в конце компиляции модуля — `reset()`. Никаких `delete`.
`std::shared_ptr` выкинуть полностью. `std::vector` внутри узлов —
заменить на `Span<T>` (указатель + длина, память в общем пуле).

**Обход деревьев →** вместо рекурсивного visitor через vtable, использовать
`std::visit` по `std::variant<...>`. Современный компилятор C++
разворачивает это в jump-table — дешевле vtable и инлайнится.

**Разрешение символов →** это самая глубокая проблема:
1. **String interning**: все идентификаторы → `StringId = uint32_t`.
   Сравнение = `int == int`. Пул строк — один большой arena, не
   освобождается.
2. **Keywords** через perfect hash (gperf в build time) — O(1) с нулевым
   branching.
3. **Scopes** как persistent hash-array-mapped-trie (HAMT) — O(log₆₄ N)
   lookup, immutable sharing.
4. **Малые scopes** (функция, блок) — линейный scan. Для ≤16 имён это
   **быстрее** hash table, потому что prefetched целиком в линию кэша.
   LuaJIT так делает.

**Таблицы/реестры O(N) / O(N log N) →** обычно означает, что что-то не
проиндексировано. Если `find_type_by_name` делает линейный поиск — нужно
построить reverse map один раз и работать по индексу. Build-once-lookup-many.

### 2.5. Что реально даёт наибольший эффект (для практики)

В порядке ROI:

1. **Arena allocator + освобождение по окончании компиляции** — неделя
   работы, обычно -40% к времени компиляции на больших файлах. Убивает
   `shared_ptr`, упрощает ownership.

2. **String interning для всех идентификаторов** — два дня, ещё -10–15%.
   Особенно помогает в type checker, где сравнивается много имён.

3. **Bytecode для hot path** — серьезная работа (недели), но даёт
   interpreter в 5–20× быстрее AST-walker. LuaJIT, V8, CPython-3.11+ —
   все здесь.

4. **SoA для самых частых таблиц** (AST-nodes, IR-instructions, types) —
   когда профайлер покажет, что конкретный проход горячий. Не делать
   "везде" — это лишний effort.

5. **`std::variant` вместо виртуальных функций** для маленьких закрытых
   иерархий (Op codes, Type kinds). vtable для больших открытых иерархий
   (plugin system) — оставляем, там полиморфизм по делу.

### 2.6. Честные предостережения

**LLVM и GCC так НЕ делают** в полной мере. Они предпочитают гибкость и
переиспользование проходов между языками. Их AST/IR — pointer-based с
vtable. Это медленно, но позволяет одному `LoopUnrollPass` работать для C,
C++, Fortran, Rust, Swift одновременно. Если вы делаете "универсальную
инфраструктуру" — pointer-based оправдано. Если "быстрый компилятор одного
языка" — arena+indices лучше.

**Преждевременная оптимизация здесь так же вредна, как везде.** Сначала
профиль. Обычно 80% времени в 2–3 проходах (часто typecheck + register
allocation + codegen). Их и оптимизировать под layout. Остальное —
оставить как есть.

**ABI/интеропабельность страдает.** Как только у вас плоские индексы вместо
указателей, сложнее делать плагины, которые travers'ят AST внешнему
инструменту. Поэтому LLVM `Value*` сохраняет указатели — это контракт с
consumers.

---

## Часть 3. AST layout и аналогия с DWARF `.debug_info`

### 3.1. Наблюдение пользователя

Моё описание оптимального AST layout'а ("flat array of structs with
KidSpan ranges") напомнило пользователю формат **DWARF `.debug_info`**.
И это **блестящее попадание** — мы столкнулись с той же фундаментальной
задачей (сериализовать дерево плоско), просто в разных контекстах.

### 3.2. Как устроен DWARF .debug_info

DWARF — формат отладочной информации. `.debug_info` секция содержит
**DIE** (Debugging Information Entries):

- Каждый DIE имеет **tag** (`DW_TAG_subprogram`, `DW_TAG_variable`,
  `DW_TAG_formal_parameter`, ...) и набор **attributes** (`DW_AT_name`,
  `DW_AT_type`, `DW_AT_location`, ...).
- DIE **плоско лежат в памяти/файле**, подряд.
- Дерево кодируется через **order + null-marker**:
  - parent DIE
  - его children — линейно
  - **null DIE** (entry с тегом 0) сигнализирует "children закончились"
  - следующий DIE — sibling родителя

Пример:
```
DW_TAG_subprogram          <- функция foo
  DW_AT_name = "foo"
  DW_TAG_formal_parameter  <- child: параметр
    DW_AT_name = "x"
  DW_TAG_variable          <- child: локальная переменная
    DW_AT_name = "tmp"
  NULL                     <- end of foo's children
DW_TAG_subprogram          <- функция bar (sibling of foo)
  DW_AT_name = "bar"
  ...
  NULL
```

### 3.3. Свойства этого layout'а

**Плюсы:**
- **Полностью плоский массив байтов** — идеально для file storage и
  mmap. Ноль указателей в самой сериализованной форме.
- **Preorder traversal** = последовательное чтение, prefetcher счастлив.
- **Никаких per-node child-count полей** или offset-таблиц — null-marker
  заменяет их.
- **Sibling iteration** = walk вперёд до null-DIE.
- **Поддерево** = contiguous range в массиве — можно memcpy целиком.

**Минусы:**
- **Random access к "третьему child"** = O(child-count) — нужно
  итерироваться.
- **Вставка child** = shift всех последующих DIE — O(N).
- **Удаление** = либо shift, либо tombstone.
- **Parent pointer** — отсутствует, при необходимости строится
  side-table.

### 3.4. Сравнение плоских layout'ов деревьев

Существует несколько способов плоско закодировать дерево. Сравним по
access-паттернам:

#### 3.4.1. Preorder + null-marker (DWARF-style)

```
[parent | child1 | child1.grandchild | NULL | child2 | NULL | NULL]
```

Каждый узел: `tag + attributes`. Null — отдельный 0-байт.

- **Iteration (preorder)**: O(N), максимально cache-friendly.
- **Sibling iteration**: O(siblings).
- **Random access child[i]**: O(i).
- **Insert/delete**: O(N) shift.
- **Storage overhead**: ~1 байт null-marker на узел.

**Где хорошо:** read-only сериализованные данные (DWARF, сетевые протоколы,
персистентное storage).

#### 3.4.2. KidSpan ranges (моё первоначальное описание)

```
nodes: [parent: { kids: {offset:1, len:2} }, child1: {...}, child2: {...}]
kids_pool: (нет, children уже в основном массиве по offset)
```

Каждый узел: `data + KidSpan`. KidSpan указывает на slice в общем массиве.

- **Iteration (preorder)**: O(N), так же cache-friendly.
- **Sibling iteration**: O(siblings), но известно заранее len.
- **Random access child[i]**: O(1) — `nodes[kids.offset + i]`.
- **Insert/delete**: O(N) shift.
- **Storage overhead**: 8 байт KidSpan на узел.

**Где хорошо:** in-memory representation для read-once-build-many компилятора.
Удобно для type-checker и IR passes, которые часто делают "дай мне
3-го ребёнка этого узла".

#### 3.4.3. Parent + first-child + next-sibling pointers (классика)

```
struct Node {
  NodeId parent;
  NodeId first_child;
  NodeId next_sibling;
  ... data ...
};
```

(Индексы вместо указателей — то же самое, но cache-friendly.)

- **Iteration (preorder)**: O(N), но с random-access pattern, менее
  cache-friendly.
- **Sibling iteration**: O(siblings).
- **Random access child[i]**: O(i).
- **Insert/delete**: O(1) pointer-rewiring!
- **Storage overhead**: 12 байт на узел.

**Где хорошо:** mutable trees (AST во время парсинга, IR во время
трансформаций). Свободно мутируется без shift'ов.

#### 3.4.4. CSR (Compressed Sparse Row)

Два массива: `offsets[N+1]` и `children[M]`, где M — общее число
parent-child рёбер.

```
offsets:    [0,    2,    5,    5,    8,    ...]  // node i имеет детей в children[offsets[i]..offsets[i+1])
children:   [1, 2, 3, 4, 5, 6, 7, ...]
```

- **Iteration (preorder)**: с дополнительной логикой.
- **Sibling iteration**: O(siblings), contiguous slice.
- **Random access child[i]**: O(1).
- **Insert/delete**: дорогая перестройка — но часто build-once.
- **Storage overhead**: 4 байта на узел + 4 байта на edge.

**Где хорошо:** графы больше чем деревья (def-use chains, CFG), когда
нужна batch-обработка всех детей многих узлов. Используется в LLVM IR.

#### 3.4.5. Когда какой применять

| Layout | Когда |
|--------|-------|
| **DWARF preorder + null** | Сериализация в файл/сеть. Read-only after build. |
| **KidSpan ranges** | AST in-memory, type-checking passes, "дай N-го ребёнка". |
| **Parent + first-child + next-sibling** | Mutable tree во время парсинга / transformation passes. |
| **CSR** | Graph-shaped IR (CFG, def-use). Batch-обработка. |

**Смешанный подход** часто лучший: один layout для build phase, другой для
post-build passes. LLVM так и делает: parser строит linked-structure,
далее MLIR pass-tunnel конвертирует в flat-SoA.

### 3.5. Гибрид: от DWARF к in-memory

Для read-heavy компилятора после парсинга:
1. Парсер строит tree с parent/child/sibling pointers (мутабельно).
2. После парсинга — **freeze**: обходим preorder, эмитим плоский массив в
   DWARF-style с KidSpan-ами (если нужны random-child-access) или
   без (если только preorder).
3. Все последующие passes работают с замороженным плоским layout'ом.

Это решает конфликт: парсер хочет мутабельность (vtable-tree), passes хотят
cache-friendliness (flat-SoA). Две фазы — два layout'а.

---

## Часть 4. ECS (flecs) vs custom SoA

### 4.1. Чем ECS привлекает для компилятора

У flecs (Sander Mertens) есть три фичи, которые compiler-writers хотят:

1. **Type-driven iteration**: "дай мне все `BinaryExpr`" → flecs query.
   Данные лежат contiguous (archetype-storage).
2. **Добавление компонентов на лету**: pass typecheck может
   `add<TypeInfo>()` к expression-entities без изменения типа узла. В
   классическом AST для этого нужны side-tables `Map<NodeId, TypeInfo>`,
   что менее элегантно.
3. **Кешированные запросы**: повторный проход "все loops в function X" —
   flecs переиспользует результат, инкрементально обновляет.
4. **Hierarchy + произвольные relationships** через `ChildOf` /
   `Relationship` — AST, scope-tree, def-use chains все ложатся.
5. **Snapshot/restore** из коробки — undo/redo в IDE, IR checkpoints
   между проходами.
6. **Module system** — можно паковать проходы как flecs modules.

На бумаге выглядит идеально. flecs реально мощный, зрелый, с decent
documentation.

### 4.2. Где flecs проигрывает кастомному SoA-стеку

#### 4.2.1. ECS identity ≠ node identity

В ECS entity — это просто `uint64_t`. Чтобы из узла получить его `left`-child:

- положить `Left(uint64_t)` компонент → лишний lookup в archetype-storage
- либо использовать relationship `flecs::entity(left_id).get<Left>()` →
  ещё больше indirection

Сравните:

```cpp
// Кастомный arena-tree:
nodes[i].left;                  // один cache-line, если layout правильный

// flecs:
world.entity(left_id).get<Left>()->node;  // 2-3 cache misses на каждый шаг
```

Для **однократного** traversal это +20–40% overhead. Для компилятора,
который ходит по AST 50 раз — складывается в замедление в разы.

#### 4.2.2. Iteration order

ECS традиционно **не гарантирует порядок** — компоненты хранятся
grouped-by-archetype, а внутри archetype в порядке добавления. Компилятору
часто нужен **source order** или **AST-preorder**. flecs даёт `cascade` и
`Ordered`, но это всегда явная работа, тогда как `std::vector<Node>`
упорядочен by design.

#### 4.2.3. Allocation patterns

При парсинге 1M-узлового AST создание 1M flecs-entities — это не
бесплатно. У flecs есть pool-allocators внутри, но bump-allocator arena
дешевле: `ptr += sizeof(Node)`, нулевая branching. Для batch-built
структур (AST, IR) arena-allocation всегда выигрывает.

#### 4.2.4. Boilerplate

Каждый вид AST-узла — отдельный component struct. Каждый вид
IR-instruction — отдельный. Это **тонна** кода. В tagged union
(`std::variant<Add, Sub, Mul, ...>`) это одна строка, и `std::visit`
разворачивается в jump-table — сравнимо по скорости с ECS-query, но без
indirection.

#### 4.2.5. Mutability по умолчанию

Компиляторы часто хотят "frozen IR между проходами" — invariant что в
этом pass мы только читаем. ECS по умолчанию изменяемый. Invariant'ы
навешиваются вручную.

#### 4.2.6. Debugging experience

Весь тулинг (gdb, perf, AST-printer, profilers) ожидает древовидные /
списочные структуры. ECS в debug — это плоские таблицы с entity-id,
отладка превращается в "что это за entity 4827109 и какие у него
компоненты".

### 4.3. Что используют в реальных компиляторах

**Никакой production-компилятор** (LLVM, GCC, rustc, Go, Swift, Roslyn,
Zig) **не использует** generic ECS. Все используют один и тот же паттерн,
который **похож на ECS, но hand-rolled**:

- `using NodeId = uint32_t;` — typed integer id
- `IndexVec<NodeId, NodeData> nodes;` — dense массив
- Несколько параллельных массивов для SoA там, где конкретный проход этого
  требует
- Component-like side-data `IndexVec<NodeId, TypeInfo> types;` — но
  создаются вручную, не через generic registry
- Arena allocator под всё

Этот паттерн **дает все плюсы ECS** (cache-friendly iteration, type-driven
dispatch, side-tables) **без ECS abstraction overhead**. Разница: вы сами
решаете layout под конкретные проходы, а не делегируете это flecs.

rustc так и называет — **"ECS-like without ECS"**. У них есть доклады об
этом.

### 4.4. Почему ECS всё-таки соблазняет

Потому что у ECS есть три фичи, которые compiler-writers реально хотят:

1. **Heterogeneous optional attributes** — узлы одного "типа" могут иметь
   разный набор свойств. ECS решает это естественно.
2. **Runtime extensibility** — плагины добавляют новые component kinds.
3. **Observer / reactive systems** — "когда добавили TypeInfo, запусти pass
   X". Очень удобно для пайплайна проходов.

Но в реальных компиляторах:
1. решается side-tables `DenseMap<NodeId, X>`
2. решается **не нужно** — проходы компилятора фиксированы на build time,
   plugin-system в компиляторе большая редкость
3. решается **явным pass pipeline** (LLVM `PassManager`, rustc `Queries`)

### 4.5. Когда flecs УМЭСЕН

- **Game engine scripting language** — где сам рантайм игры уже на ECS, и
  compiler живёт в той же семантике (Bevy's bevy_reflect, некоторые
  research-движки).
- **Simulation / visual scripting** — где "компилятор" близок к dataflow
  graph (это в каком-то смысле наш cc-utils).
- **Pedagogical проект** — где цена обучения ECS окупается пониманием.
- **Research compiler с очень heterogeneous IR** — где overhead ECS
  оправдан гибкостью.
- **Очень small compiler, где micro-perf не важна, но гибкость важна** —
  flecs экономит недели разработки.

### 4.6. cc-utils УЖЕ структурно ECS

Кстати, наш текущий дизайн — это уже ECS без слова "ECS":

| ECS | cc-utils |
|-----|----------|
| Entity | `node::instance_id()` |
| Component | `node::properties()` + `node::slots()` |
| Archetype | `node_factory::type_id()` (factory → archetype) |
| System | `node::activate()` |
| Component registration | `host_registry::register_node_factory` |
| Query "all X" | `host_registry::find_node_factory(X)` + `graph::nodes()` фильтрация |
| Relationship | `graph::edges` |
| Reactive observer | (пока нет, но `invalidate_view_cache` — это manual version) |

Добавление flecs под капот — это **переписывание одной ECS-абстракции в
другую**, без новой функциональности. Может быть быстрее на hot-loops
(потому что SoA в flecs), но наш рантайм **не hot-loops** — он
activate'ит цепочку из 4–6 узлов с реальной работой (nasm/ld) внутри.
Cache-friendliness графа здесь иррелевантен — bottleneck в nasm/ld, не в
нашем коде.

---

## Часть 5. Практические рекомендации

### 5.1. Для скриптового языка пользователя (existing codebase)

**Не мигрировать на flecs.** Не переписывать всё под SoA сразу. Вместо
этого — **последовательные инкрементальные улучшения**, в порядке ROI:

1. **Arena allocator + освобождение по окончании компиляции** (неделя).
   Убивает `shared_ptr`, упрощает ownership. Обычно -40% к времени
   компиляции на больших файлах.

2. **String interning для всех идентификаторов** (2 дня). Ещё -10–15%.
   Особенно помогает в type checker.

3. **Bytecode для hot path interpreter'а** (недели). Interpreter в 5–20×
   быстрее AST-walker. LuaJIT, V8, CPython-3.11+ — все здесь. Если сейчас
   интерпретатор ходит по AST — это **самый** большой win.

4. **SoA для конкретных таблиц**, которые профайлер показал горячими
   (каждый — отдельный этап). Не делать "везде" — это лишний effort.

5. **`std::variant` вместо vtable** для маленьких закрытых иерархий
   (Op codes, Type kinds).

Параллельно: использовать **DWARF-style layout** для serialization
(если есть persistence в скриптовом языке — байткод-файл, snapshot).

### 5.2. Для cc-utils

1. **Добавить Compile-кнопку фазы 1–3** (см. Часть 1). Это превращает
   cc-utils из визуального интерпретатора в визуальный компилятор
   генератора компиляторов — мета, но логично. Полезно для отладки
   (standalone binary без хоста), plugin-as-static-library (фаза 3).

2. **Не переписывать host под DOD.** Host — это editor, его данные
   холодные. Cache-friendly должен быть **сгенерированный код**, а он уже
   получится без всяких структур данных компилятора — чистый C++.

3. **Не внедрять flecs.** Текущий дизайн уже ECS-shaped, замена на flecs
   ничего не даст.

4. **Где DOD мог бы пригодиться внутри cc-utils:** если появятся passes
   по самому графу (например, validation, dependency analysis для
   node-coloring по типам, layout autotiling). Но это не hot path, effort
   не оправдан.

### 5.3. Для нового компилятора с нуля

Если бы начинали новый компилятор сегодня:

1. **Layout:** custom `IndexVec<Id, T>` + side-tables `IndexVec<Id, U>`,
   поверх arena allocator. Не ECS.

2. **Идентификаторы:** interned strings с `StringId = uint32_t`. Keywords
   через perfect hash.

3. **AST build phase:** parent/child/sibling indices (мутабельно).

4. **AST post-parse:** freeze в preorder-SoA для passes (DWARF-style или
   KidSpan).

5. **IR:** CSR для graph-structured (def-use, CFG).

6. **Bytecode** для interpreter, если есть runtime.

7. **Visitor:** `std::variant` + `std::visit` для закрытых иерархий,
   vtable для открытых (если есть plugin-extension points).

8. **Pass pipeline:** явный `PassManager`, не reactive observers.

9. **Tooling:** с самого начала — AST printer в S-expr, IR printer, perf
   integration. Иначе debug hell.

---

## Часть 6. Литература (по убыванию практичности)

1. **Bob Nystrom — Crafting Interpreters** (бесплатно онлайн). jlox → clox
   переход — ровно про то, как из AST-pointer-tree получить cache-friendly
   bytecode. Самый доступный источник.

2. **Mike Acton — Data-Oriented Design and C++** (CppCon 2014). Не про
   компиляторы, но про mindset. Constitutional talk про DOD.

3. **Andrew Kelley — Practical DOD** (записи talk'ов про Zig compiler).
   Конкретно про compiler-as-DOD, с примерами кода.

4. **Engineering a Compiler**, Cooper & Torczon — разделы про symbol
   table / IR representation. Стандартный учебник.

5. **Chandler Carruth — CppCon talks** про hypermodular C++ и как
   структура компилятора влияет на speed. Особенно его talk'и про
   modules и clustering.

6. **LuaJIT wiki** — заметки Mike Pall о design choices. Уникальный
   взгляд "снизу вверх": от hardware constraints к дизайну.

7. **Mike Pall — "A No-Frills Introduction to Lua 5.1 Internal"** — про
   то, как маленький интерпретатор устроен cache-friendly.

8. **Sander Mertens — flecs docs + issue threads.** Сам Sander честен про
   trade-off'ы ECS; из issue-тредов видно, какие задачи ложатся плохо.

9. **rustc guide** + `rustc_data_structures` crate в исходниках rustc.
   `IndexVec`, `Interner`, `FxHashMap` (faster hash для small keys) —
   можно использовать как референс или даже скопировать.

10. **DWARF Debugging Information Format specification** (DWARF5). Раздел
    про `.debug_info` — canonical reference для preorder+null-marker
    layout. Не обязательно читать целиком, но полезно увидеть.

11. **Andrew Kelley — "Battery Includes" talk** про Zig compiler
    architecture (есть на YouTube).

12. **Clang/LLVM source code** — `BumpPtrAllocator`, `SmallVector`,
    `StringRef`, `PointerUnion` — battle-tested building blocks, можно
    копировать идеи.

13. **entt (C++ ECS library)** — если всё-таки решите пробовать ECS.
    Можно использовать напрямую для SoA-style AST storage; benchmark'ите
    vs custom IndexVec.

14. **"SSA Book" (Rastello & Bouchez Tichadou)** — про SSA-form IR, layout
    def-use chains, dominator tree. Более academically, но фундаментально.

15. **Polyhedral compilation** (Polly в LLVM, Graphite в GCC, Pluto) —
    про то, как loop-nest превращается в integer sets + affine maps для
    идеальной cache-friendly оптимизации. За гранью базовых
    компиляторов, но интересно.

---

## Приложение А. Что не вошло в этот документ

Темы, которые обсуждались вскользь или не были затронуты, но релевантны:

- **Halide schedules** — algorithm/schedule separation как образец для
  аннотаций в графе cc-utils (если делать Compile фазы 5).
- **TVM AutoTVM / Ansor** — autotuning layout / tile sizes. Research-grade.
- **MLIR `ExecutionEngine`** — JIT-компиляция графа; отличается от нашего
  случая тем, что MLIR работает в одном процессе без визуального UI.
- **Chandler Carruth "Module Splitting"** — про modular AST design.
- **Tiny Compiler (chibicc)** — минималистичный C compiler от Rui Ueyama,
  8000 строк. Хороший reference для "как сделать маленький компилятор
  читаемо".

Эти темы можно развернуть в отдельные документы, если появится
необходимость.

---

## Приложение Б. Краткая шпаргалка (для быстрого возврата)

**Layout'ы деревьев:**

| Layout | Random child | Insert | Sibling iter | Storage overhead |
|--------|--------------|--------|--------------|------------------|
| DWARF preorder+null | O(i) | O(N) shift | O(siblings) | 1 байт/узел |
| KidSpan ranges | O(1) | O(N) shift | O(siblings) | 8 байт/узел |
| Parent+child+sibling ptrs | O(i) | O(1) rewire | O(siblings) | 12 байт/узел |
| CSR (graph-style) | O(1) | rebuild | O(siblings) contiguous | 4 б/уз + 4 б/edge |

**Компромиссы:**

- **Want cache-friendliness + build-once:** preorder SoA (DWARF / KidSpan).
- **Want mutability during build:** pointer/index-linked tree.
- **Want fast "all children of many parents":** CSR.
- **Want plug-in extensible:** vtable (медленно) или `std::variant` если
  sealed.

**Когда ECS (flecs) оправдан:**
- Game engine / simulation runtime.
- Pedagogical проект.
- Очень heterogeneous IR с plugin-extension.
- Когда micro-perf не важна, но flexibility нужна.

**Когда ECS не оправдан:**
- Production компилятор.
- Hot path с детерминированным layout.
- Когда ABI/interop с внешними тулами важен.
- Когда тулинг (gdb, perf) должно работать естественно.

**Для cc-utils конкретно:**
- Compile-кнопка фаз 1–3 — делаем, полезно.
- flecs — не надо, текущий дизайн уже ECS-structured.
- DOD внутри хоста — не нужен, bottleneck в nasm/ld.

---

*Документ создан как результат дискуссии в cc-utils. Возвращаться к
разделам 3.4 (сравнение layout'ов деревьев), 5.1 (ROI для практики) и 6
(литература).*
