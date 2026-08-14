# Ментальная модель работы конвейера компилятора

`ssk` - skif-script source file.

Что бы вычислить какой то листинг кода на языке очень важно определить контекст.

Вот контекст:

```ssk
type int {.builtin.}; // declare type 'int' exists. defined by implementation internals
infix +{.precedence=20, associativity=left.}; // declare infix '+' operator properties

infix +(lhs: int, rhs: int): int {.builtin.}; // infix '+' overload for (int, int)
```

Рассматриваемый листинг

```ssk
var a: int = 0;     // resolve 'int' type, construct value on stack, bind to 'a' symbol, eval initializer expr, assign to binded lvalue.
var b: int = a + 5; // resolve 'int' type, construct value on stack, bind to 'b' symbol, eval initializer expr, assign to binded lvalue.
var c: int = if(b > 0, a, -1); // resolve 'int' type, construct value on stack, bind to 'c' symbol, eval initializer expr, assign to binded lvalue.
```

## Pipeline

```
function compile(source, ctx):
  cst   = parse_cst(source)
  ast   = lower(cst)
  declaration_pass(ast, ctx.scope)
  resolve_pass(ast, ctx)
  verify_pass(ast, ctx)
  ir    = build_ir(ast, ctx)
  value = eval(ir, ctx.env)
```

Каждую стадию ниже разбираем на нашем листинге.

---

### Parse CST

Токенизация + lossless дерево (trivia, позиции, комментарии). Skip — не интересно.

### Lowering to AST

CST → семантическое дерево: выкидываем trivia, склеиваем токены в узлы.

```
function lower(cst) -> ast:
  for n in walk(cst):
    match (n)
    of VarDecl(name, type, init):
      emit DataObjDef(name, type, lower(init))
    of InfixExpr(op, l, r):
      emit Infix(op, lower(l), lower(r))
    of CallExpr(name, args):
      emit Call(name, [lower(a) for a in args])
    of Ident(name):   emit Identifier(name)
    of Num(v):        emit Literal_Num(v)
```

AST для нашего листинга (схематично):

```
DataObjDef(a, int, Literal(0))
DataObjDef(b, int, Infix(+, Identifier(a), Literal(5)))
DataObjDef(c, int, Call(if, [Infix(>, Identifier(b), Literal(0)),
                             Identifier(a), Literal(-1)]))
```

`Identifier` — это **любое** использование имени (переменная ли, функция ли, тип ли). Кто именно — выясняется позже. AST структурный, не семантический: `int`/`a`/`+`/`if` — пока просто строки.

### Declaration_Pass

Обходит «spine» AST — только топ-левел объявления, **не спускается** в выражения и тела функций. Регистрирует имена в scope.

```
function declaration_pass(ast, scope):
  for n in ast.top_level:            # spine, не walk
    match (n)
    of DataObjDef(name, type_expr, _):        # initializer игнорируем
      tid = resolve_type(scope, type_expr)
      if !tid: error("unknown type", type_expr)
      scope.declare(name, Data_Object, tid)
    of FuncDef(name, proto, body):            # body не заходим
      pid = register_callable(proto)
      scope.declare(name, Functional, pid)
```

После прохода:

```
scope: { int:Type, +:Functional, a:int, b:int, c:int }
```

Ключевое: `a` / `b` / `c` зарегистрированы **до** того, как мы смотрим на их инициализаторы. Поэтому когда в Resolve_Pass мы разбираем `b = a + 5` или `c = if(b > 0, a, -1)`, `Identifier(a)` / `Identifier(b)` гарантированно резолвятся. Это развязывает руки и для ссылок «вперёд» (если бы они были), и для инструментов, которым достаточно знать inventory имён без полного check.

### Resolve_Pass

Полный обход AST снизу-вверх. Вычисляет атрибуты каждого узла: тип, категория (lvalue/rvalue), выбранный overload. Не выдаёт ошибок кроме «не найдено».

```
function resolve_pass(ast, ctx):
  for n in walk(ast) bottom-up:
    match (n)
    of Literal_Num(_):              set_type(n, int)
    of Identifier(name):
      r = scope.lookup(name)
      if !r: error("undefined", name)
      set_type(n, r.type); set_def(n, r)
    of Infix(op, l, r):
      ov = resolve_overload(op, type(l), type(r))
      if !ov: error("no overload", op, types)
      set_type(n, ov.ret); set_def(n, ov)
    of Call("if", [c, t, e]):
      set_type(n, common_type(type(t), type(e)))
    of DataObjDef(_, t, init):      set_type(n, t)
```

После: `a`/`b`/`c`/`0`/`5`/`-1` → int, `b > 0` → bool, `if(...)` → int. Каждый `Infix`/`Call` знает свой overload.

### Verify_Pass

Читает атрибуты, которые насобирал Resolve. Только проверки, ничего не вычисляет.

```
function verify_pass(ast, ctx):
  for n in walk(ast):
    match (n)
    of DataObjDef(_, type, init):
      if !coercible(type(init), type): error("init not coercible", type, type(init))
    of Assign(lhs, rhs):
      if category(lhs) != Lvalue:   error("assign to rvalue")
      if is_const(type(lhs)):       error("assign to const")
    of Identifier(name):
      if is_uninitialized(name):    warn("use before init", name)
```

На нашем листинге: всё чисто, никаких warn'ов. Каждый `Identifier` ссылается на уже объявленное имя (`a`, `b`), инициализаторы совместимы по типу.

### Build_IR

С lowering'ом в IR — отдельный ортогональный мир. `build_ir` единственный мост из checked AST; сам IR ничего не знает про AST.

```
function build_ir(ast, ctx) -> Module:
  m = new Module()
  for n in walk(ast):
    match (n)
    of DataObjDef(name, _, init):
      rid = emit_expr(m, init)       # рекурсивно, возвращает VReg
      emit(m, Store(name, rid))
    # emit_expr:
    of Literal(v):        return emit(m, Const(v))
    of Identifier(name):  return emit(m, Load(name))
    of Infix(op, l, r):
      rl = emit_expr(m, l); rr = emit_expr(m, r)
      return emit(m, BinOp(op, rl, rr))
    of Call("if", [c, t, e]):
      rc = emit_expr(m, c); rt = emit_expr(m, t); re = emit_expr(m, e)
      return emit(m, Select(rc, rt, re))
```

IR для нашего листинга (один блок — тернарный `if` чистый, ветвиться незачем):

```
%r0 = const 0;                    store a = %r0
%r1 = load a;  %r2 = const 5;     %r3 = bin + %r1 %r2;   store b = %r3
%r4 = load b;  %r5 = const 0;     %r6 = bin > %r4 %r5
%r7 = load a;  %r8 = const -1;    %r9 = select %r6 %r7 %r8;   store c = %r9
```

`Store`/`Load` — точки мутаций. Reaching-defs dataflow (для dataflow-graph consumer'а) ищет: для каждого `Load` какой `Store` до него доехал.

### Eval

Register-machine интерпретатор поверх IR. Тот же `Value`, что и tree-walking над AST.

```
function eval(module, env) -> Value:
  for inst in module.insts:           # линейно, без рекурсии
    match (inst)
    of Const(v, dst):         regs[dst] = v
    of Load(name, dst):       regs[dst] = env[name]
    of Store(name, src):      env[name] = regs[src]
    of BinOp(op, dst, l, r):  regs[dst] = lookup_impl(op)(regs[l], regs[r])
    of Select(dst, c, t, e):  regs[dst] = regs[c] ? regs[t] : regs[e]
```

Для нашего листинга: `a=0, b=5, c=0` (т.к. `b > 0` истинно — `b` равно 5 — берём ветку `then`, т.е. `a`, которое равно 0).