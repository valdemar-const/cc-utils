# Статус реализации

Концепция — см. `user-story.md`. Здесь — что сделано по шагам и что дальше.

## Архитектура (новая, v3 plugin API)

```
cc-core (abstract contract)
  ├─ any_value.hpp        any_value = aa::any_with<move, copy, type_info>
  ├─ node.hpp             slot, node, node_properties, failure, activate_result
  ├─ node_factory.hpp     node_factory + property_desc + property_kind + apply_defaults
  ├─ registry.hpp         type_registry (register_value_type<T>, is_connectable)
  ├─ view.hpp             view_renderer, view_renderer_provider, view_context
  ├─ host.hpp             host_registry — типы/фабрики/рендереры + provider bookkeeping
  └─ plugin_entry.hpp     extern C: cc_plugin_load + cc_plugin_register

cc-runtime (host-side concrete impl)
  ├─ host_registry.hpp    make_host_registry()
  ├─ plugin_loader.hpp    load_all() — dlopen + auto-discover cc-plugin-*.so,
  │                       проставляет provider(name) на фабриках через RAII guard
  ├─ graph.hpp            nodes + edges, remove_node/remove_edge
  ├─ runner.hpp           pull(node, slot) — demand-driven activate, cycle detect
  └─ map_properties.hpp   node_properties impl (строковый map)

cc-plugin-basic / -tl / -tl-ir / -x86_64 (узлы графа)
  ├─ Каждый плагин: factory::create() и factory::create_with_id(id)
  ├─ basic.text.from_file, basic.text.constant, basic.view, basic.exec
  ├─ tl.frontend, tl.irgen
  └─ x86_64.nasm_gen, x86_64.assemble

cc-workbench (host UI)
  ├─ Автозагрузка плагинов через plugin_loader
  ├─ Canvas (imgui-node-editor + BlueprintNodeBuilder)
  │    ├─ Цветные заголовки по category, цветные пины по типу значения
  │    ├─ Schema-driven property editor (text/path/multiline/int/bool)
  │    ├─ Drag-to-link output→input, drag-to-canvas palette
  │    └─ ПКМ → custom menu window (НЕ BeginPopup) — без Suspend/Resume
  ├─ Pipeline file (.pipeline, XML через pugixml) — New/Open/Save/Save as
  │    ├─ pipeline_xml.hpp/cpp: save_pipeline, load_pipeline
  │    ├─ Секция <requires> со списком плагинов-провайдеров
  │    └─ Загрузка проверяет requires и смягчает missing-plugin'ы как warnings
  ├─ View tab: dropdown именованных view-узлов + TextEditor (read-only, C++)
  └─ Logger: read-only TextEditor, selectable/copyable, sticky-bottom scroll

Старый cc-pipeit (v2 plugin API) удалён; плагины tl/tl-ir/x86_64 мигрированы.
```

## Сделано

### Шаг 1A — Foundation
- cc-core с any_value (AnyAny), слот/узел/фабрика, реестр типов, host_registry
- cc-runtime с concrete impl'ами, plugin loader, graph, runner
- plugin_api_version=3, новый entry point `cc_plugin_register(host_registry&)`

### Шаг 1B — Базовый плагин
- `cc-plugin-basic` с узлами `text.from_file`, `text.constant`, `view`, `exec`
- Регистрирует типы `text`, `path`, `int`
- Метаданные через `property_schema()` — UI читает схему, не хардкодит

### Шаг 1C — Workbench UI
- Автозагрузка: plugin_loader::load_all сканирует search dirs
- Pretty rendering: цветные header'ы по category, type-coloured pins с иконками
- View tab: dropdown именованных view-узлов, C++-подсветка через TextEditor
- Logger: read-only TextEditor с selectable/copyable текстом
- Удаление узлов и связей (Delete key), drag-to-link
- Schema-driven property editor (Browse button для path через ImFileDialog)

### Шаг 2A — Миграция компиляторных плагинов
- `cc-plugin-tl` (frontend), `cc-plugin-tl-ir` (irgen), `cc-plugin-x86_64`
  (nasm_gen + assemble) под v3 node-API
- AST / IR view renderers
- `cc-pipeit` (v2 контракт) удалён вместе с `ccp` CLI драйвером

### Шаг 2B — End-to-end pipeline
- e2e-тест `cc_pipeline.return_42_end_to_end`: source → frontend → irgen →
  nasm → assemble → exec ⇒ exit code 42

### Реактивный View-tab pull
- Async pull через `std::future`; UI freeze-free
- Single-source input slots (graph::add_edge auto-replaces)
- 150ms debounce; clean invalidation на любой мутации графа
- Drag-to-canvas palette: drop связи на пустой канвас → узлы фильтруются по
  совместимому типу, новый узел авто-коннектится к dragged пину

### Шаг 3 — Сохранение / загрузка `.pipeline` ✅
- **Формат**: XML через `pugixml` (заявлен в CPM, уже зарегистрирован в
  `3rdparty.cmake` — `find_package(pugixml REQUIRED)` работает без правок
  `deps.toml`)
- **Модуль** `projects/bin/cc-workbench/src/pipeline_xml.{hpp,cpp}`:
  ```cpp
  save_pipeline(host, graph, positions, path) -> expected<void, string>;
  load_pipeline(host, graph, path)           -> expected<load_result, string>;
  ```
  load_result возвращает positions (для `ed::SetNodePosition`) + warnings:
  missing_plugins, unknown_node_types, skipped_edges
- **Схема XML**:
  ```xml
  <pipeline version="1">
    <requires>
      <plugin name="basic"/>
      <plugin name="tl"/>
    </requires>
    <nodes>
      <node type="..." id="...">
        <pos x="..." y="..."/>     <!-- опционально -->
        <properties>
          <property key="...">value</property>
        </properties>
      </node>
    </nodes>
    <edges>
      <edge src_node="..." src_slot="..." dst_node="..." dst_slot="..."/>
    </edges>
  </pipeline>
  ```
- **`<requires>`**: для каждого узла через `host.provider_of(type_id)` берём
  имя плагина-провайдера (см. ниже), пишем уникальный список. На load
  сравниваем с `host.loaded_plugins()` — отсутствующие плагины идут в warnings
- **Расширение cc-core API для provider-tracking**:
  - `host_registry::push_provider(name) / pop_provider()` (RAII guard в
    `plugin_loader::load_path` вокруг `cc_plugin_register`)
  - `host_registry::provider_of(type_id) const -> string_view`
  - `host_registry::loaded_plugins() const -> span<const string>`
  - Каждая фабрика получает provider при регистрации через текущий scope
- **Расширение node_factory API**:
  - `node_factory::create_with_id(instance_id)` — для load; создаёт узел с
    переданным id (иммутабельный instance_id сохраняется, рёбра не теряются)
  - `node_factory::apply_defaults(node)` — protected helper, применяет
    property_schema() defaults; и create(), и create_with_id() зовут его
- **Тесты** `tests/cc-pipeline_xml/`: 11 gtest-кейсов, включая end-to-end
  `save_clear_load_run_produces_42` (build → save → clear → load → pull
  exit_code == 42)

### Главное меню workbench

```
File
├─ New                Ctrl+N     → unsaved-check; clear graph + reset path/dirty
├─ Open...            Ctrl+O     → unsaved-check; ImFileDialog → load_pipeline
├ (separator)
├─ Save               Ctrl+S     → in-place если есть path; иначе Save as
├─ Save as...                    → ImFileDialog → save_pipeline + add .pipeline ext
├ (separator)
└─ Quit               Alt+F4     → unsaved-check

View
├─ Open tab
│  └─ Pipeline / View / Logger   →isVisible=true для закрытых окон
├ (separator)
├─ Reset layout                  →dockingParams.layoutReset = true
└─ Switch layout
   └─ default                    →то же, что Reset (задел под будущие профили)

Help
└─ About cc-workbench
```

### User-friendly фичи
- **Hotkeys**: Ctrl+N / Ctrl+O / Ctrl+S; игнорируются пока активен text input
- **Unsaved-changes modal**: Save / Don't Save / Cancel; gates New/Open/Quit
- **Window title**: `"basename + ( * если dirty) — cc-workbench"` обновляется
  каждый кадр через `appWindowParams.windowTitle`
- **Last directory** ImFileDialog запоминает последний каталог (seed = parent
  текущего pipeline_path)
- **Auto Zoom-to-Fit** после Open — пользователь сразу видит восстановленный граф
- **Error modal** при неудачной load (file missing / parse error / bad version)
- **Warnings modal** для мягких проблем (missing plugins / skipped nodes) —
  граф всё равно открывается
- **Auto .pipeline extension** при Save as если пользователь забыл суффикс
- **Pipeline-tab text** в docked окне:_basename(path) или "untitled.pipeline"

## Известные проблемы / out-of-scope

- **Position tracking на move**: drag узла в canvas НЕ проставляет dirty
  (imgui-node-editor не даёт явного callback). Позиции сохранятся только если
  пользователь жмёт Save после move — позиции попадают в XML из live editor
  state. Trade-off: позиции второстепенны, отдельный dirty-флаг для layout
  не заведён
- **Unsaved Save-as flow**: если path пустой и пользователь жмёт Save в
  confirm-modal, открывается Save-as; после его завершения исходное
  pending-action теряется (пользователь должен снова нажать New/Open). MVP
  trade-off
- **Multi-tab pipelines**: один Pipeline на запуск (см. user-story: "View
  табы соотнесутся с узлами одного графа"). Multi-document потребует
  рефактора AppState в vector<Graph>

## Дальнейшие шаги (по приоритету)

1. **Рефакторинг UI в Command pattern** — `std::variant<CreateNode, DeleteNode,
   CreateEdge, DeleteEdge, SetProperty>` + одна `apply()` функция. UI кладёт в
   очередь, main loop разгребает. Задел под undo/redo. UI-only действия (open
   menu, select view) — отдельный UI state, НЕ команды
2. **Шаг 4**: toolbar с контекстными действиями выбранного узла
   (IActionProvider). Например `Compile` для exec.out узла. Сейчас есть Run
   Pipeline (ищет x86_64.assemble), но это хардкод конкретной ноды
3. **Шаг 5**: алгоритмическая генерация цветов типов (hash → palette с
   contrast/uniqueness), замена хардкода `pin_color_for_type`
4. **Шаг 6**: PIE menu как альтернатива popup
5. **Save layout restore в .pipeline**: помимо <pos> добавить zoom/viewport
6. **Multi-document**: несколько Pipeline табов (каждый со своим графом и
   file path). Требует AppState → vector<PipelineDoc> + active-tab state

## Шаг 7 — Домены предметной области ✅ (plugin API v4, формат v2)

Конвенция — см. `plans/domains.md`. Реализовано:

- **cc-core**: `domain.hpp` (`domain_desc`), `register_domain/find_domain/
  domains/domain_closure/push_domain/pop_domain` на `host_registry`;
  `node_factory::domains()` (мульти-членство, самозаявляемое);
  `value_type_desc` (PascalCase name + short_name + inline_editor) в
  `type_registry` (+ `short_name_of/inline_editor_of/parse_value`);
  `node::slot_values()` для inline-значений пинов; `property_kind` вынесен в
  `cc/property_kind.hpp`; `plugin_api_version = 4`
- **cc-types-filesystem** (новый тип-пакет): `cc::fs::file_handle`,
  `cc::fs::file_attrs`, `stat_file` — typeinfo якорится в одной .so для
  cross-DSO any_cast
- **cc-runtime**: merge-регистрация доменов, BFS-замыкание deps (циклы
  безопасны), атрибуция типов через push_domain-scope; runner: 4-й аргумент
  `const type_registry*` — инъекция inline-значений (и в ensure_outputs, и в
  pull входного слота)
- **Плагины**: basic сеет 5 доменов (basic/types, filesystem, basic/text,
  basic/view, system/process) + 5 filesystem-узлов (path-let, get_file,
  get_or_create_file, file-инспектор, read_text); exec на входе File;
  tl/tl-ir в `compiler/lang/tl` (мульти-вендор); x86_64 в
  `compiler/backend/x86_64`, assemble выдаёт File-хендл артефакта;
  `basic.text.from_file` удалён
- **pipeline_xml v2**: атрибут `domain` + `<imports>` + `<values>`;
  root-домен обязателен (hard error если не зарегистрирован), import-миccинг
  — warning; v1 — legacy-режим (все фабрики видимы) + миграция через Save;
  save без root-домена — ошибка
- **Workbench**: диалог New Pipeline / Migrate (root + чекбоксы imports,
  показывает deps/provided_types); create-menu секциями по доменам
  (root → imports → transitive deps), legacy — плоско по категориям;
  drag-palette под тем же фильтром; подписи пинов `name:short`;
  inline-редакторы в теле узла с грейаутом при подключённом проводе;
  домен в тулбаре; File/FileAttrs view-рендереры; тики диалога —
  `vector<uint8_t>` (не `vector<bool>`)
- **Тесты**: 46/46 — closure/membership/provided_types, inline-инъекция,
  strict-vs-lenient материализация, v2 round-trip, v1-legacy, missing
  root/import, e2e `return_42` на 8-узельной цепочке
- **Прочее**: `gtest-1.18.0.zip` добавлен в CPM preload (`cpm add`),
  `cpm requires bump GTest` — уже 1.18.0; ассет `test.tl.pipeline`
  мигрирован на v2
