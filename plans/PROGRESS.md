# Статус реализации

Концепция — см. `user-story.md`. Здесь — что сделано по шагам и что дальше.

## Архитектура (новая, v3 plugin API)

```
cc-core (abstract contract)
  ├─ any_value.hpp        any_value = aa::any_with<move, copy, type_info>
  ├─ node.hpp             slot, node, node_properties, failure, activate_result
  ├─ node_factory.hpp     node_factory + property_desc + property_kind
  ├─ registry.hpp         type_registry (register_value_type<T>, is_connectable)
  ├─ view.hpp             view_renderer, view_renderer_provider, view_context
  ├─ host.hpp             host_registry (агрегирует типы/фабрики/рендереры)
  └─ plugin_entry.hpp     extern C: cc_plugin_load + cc_plugin_register

cc-runtime (host-side concrete impl)
  ├─ host_registry.hpp    make_host_registry()
  ├─ plugin_loader.hpp    load_all() — dlopen + auto-discover cc-plugin-*.so
  ├─ graph.hpp            nodes + edges, remove_node/remove_edge
  ├─ runner.hpp           pull(node, slot) — demand-driven activate, cycle detect
  └─ map_properties.hpp   node_properties impl (строковый map)

cc-plugin-basic (example plugin)
  ├─ text.from_file       Source: out=text, prop path
  └─ view                 Debug tap: in=any (multi), prop name

cc-workbench (host UI)
  ├─ Автозагрузка плагинов через plugin_loader
  ├─ Canvas (imgui-node-editor + BlueprintNodeBuilder)
  │    ├─ Цветные заголовки по category, цветные пины по типу значения
  │    ├─ Schema-driven property editor (text/path/multiline/int/bool)
  │    ├─ Drag-to-link output→input, удаление узлов и связей
  │    └─ ПКМ → custom menu window (НЕ BeginPopup) — без Suspend/Resume
  ├─ View tab: dropdown именованных view-узлов + TextEditor (read-only, C++)
  └─ Logger: read-only TextEditor, selectable/copyable, sticky-bottom scroll

Старый cc-pipeit (v2 plugin API) оставлен как есть, плагины tl/tl-ir/x86_64
не мигрированы — workbench их скипает по версии API.
```

## Сделано

### Шаг 1A — Foundation
- cc-core с any_value (AnyAny), слот/узел/фабрика, реестр типов, host_registry
- cc-runtime с concrete impl'ами, plugin loader, graph, runner
- plugin_api_version=3, новый entry point `cc_plugin_register(host_registry&)`

### Шаг 1B — Базовый плагин
- `cc-plugin-basic` с узлами `text.from_file` и `view`
- Регистрирует тип `text` (std::string)
- Метаданные через `property_schema()` — UI читает схему, не хардкодит

### Шаг 1C — Workbench UI
- Автозагрузка: plugin_loader::load_all сканирует search dirs
- Pretty rendering: цветные header'ы по category, type-coloured pins с иконками
- View tab: dropdown именованных view-узлов, C++-подсветка через TextEditor
- Logger: read-only TextEditor с selectable/copyable текстом
- Удаление узлов и связей (Delete key), drag-to-link
- Schema-driven property editor (Browse button для path через ImFileDialog)

## Известные проблемы (в работе)

### Позиция context menu смещена
**Симптом**: при ПКМ меню появляется не под курсором, а со смещением
пропорционально расстоянию от центра канваса.

**Диагностика** (из логов):
- Клик в центре канваса: `MouseClickedPos = (728, 348)` — примерно под курсором
- Клик у левого края: `(-658, 376)` — отрицательное X
- Клик у правого края: `(2081, 342)` — большое X

**Причина**: внутри `ed::Begin/End`, `ImGui::GetIO().MouseClickedPos[]`
возвращает canvas-local координаты, а не абсолютные screen. imgui-node-editor
трансформирует io.MousePos для внутренней консистентности.

**План фикса**: брать позицию из другого источника — например,
`ImGui::GetCursorScreenPos()` родительского dockable window, или фиксить
через viewport-to-screen преобразование.

## Дальнейшие шаги (по приоритету)

1. **Фикс позиции context menu** (блокирует нормальный UX) — пробуем
   `ed::CanvasToScreen(canvas_local)` внутри `ShowBackgroundContextMenu`.
2. **Шаг 2A**: переписать `cc-plugin-tl` / `-tl-ir` / `-x86_64` под v3 node-API
   (frontend → ast → ir → bytes как узлы графа)
3. **Шаг 2B**: переписать `ccp` CLI драйвер — build graph из 4 узлов
   (source/text.constant → tl.frontend → tl.irgen → x86_64.backend),
   проверить `return 42;` → exit 42 end-to-end
4. **Рефакторинг UI в Command pattern** — после шага 2, когда видно реальные
   use cases. План: `std::variant<CreateNode, DeleteNode, CreateEdge,
   DeleteEdge, SetProperty>` + одна `apply()` функция. UI кладёт в очередь,
   main loop разгребает. Задел под undo/redo. UI-only действия (open menu,
   select view) — отдельный UI state, НЕ команды. EventBus пока не вводим —
   добавим если появится реальная потребность в реактивности (например,
   runner invalidation при мутации графа).
5. **Шаг 3**: сохранение/загрузка `.pipeline` файла (graph + view layout),
   on_save/on_load hooks (нужны json/xml/toml библиотеки)
6. **Шаг 4**: toolbar с контекстными действиями выбранного узла
   (IActionProvider). Например `Compile` для exec.out узла.
7. **Шаг 5**: алгоритмическая генерация цветов типов (hash → palette с
   contrast/uniqueness), замена хардкода `pin_color_for_type`
8. **Шаг 6**: PIE menu как альтернатива popup
9. **Шаг 7**: выкинуть `cc-pipeit` (старый v2 контракт) после полной миграции
