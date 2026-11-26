# GooberBASIC

A small, fast, game-friendly BASIC in the spirit of **BlitzBasic** and **BlitzMax**.
Write `graphics 640, 480`, draw some shapes, `flip`, and you have a game loop.

```basic
graphics(640, 480)
while not key_hit(KEY_ESCAPE)
    cls()
    set_color(255, 200, 0)
    draw_circle(mouse_x(), mouse_y(), 20)
    flip()
wend
```

- Written in C11. Compiles your program to bytecode and runs it on a stack VM.
- Statically typed (`int`, `float`, `string`, user types, arrays), with Blitz-style type
  suffixes (`%` `#` `$`) and BlitzMax-style annotations (`x:float`).
- Built-in 2D graphics, keyboard/mouse input, images, TTF text and sound (SDL2).

## Building

Needs a C compiler, `make`, `pkg-config` and the SDL2 development packages
(`libsdl2-dev libsdl2-image-dev libsdl2-ttf-dev libsdl2-mixer-dev` on Debian/Ubuntu).

```bash
make
```

```bash
./goober examples/goobercatch.gb
```

Other options: `goober --check file.gb` (compile only) and `goober --disasm file.gb` (show bytecode).
Run the test suite with `make test`.

Source files use the `.gb` extension.

## Editor support

[editors/vscode](editors/vscode) is a Visual Studio Code extension with syntax highlighting,
auto-indent and snippets for `.gb` files. To try it:

```bash
code --extensionDevelopmentPath="$PWD/editors/vscode" examples
```

See its [README](editors/vscode/README.md) for installing it and running programs from VS Code.

## Examples

| File | What it shows |
|---|---|
| [examples/hello.gb](examples/hello.gb) | `print`, `input`, `for`, `select` |
| [examples/types.gb](examples/types.gb) | `strict` mode, types, lists + `eachin`, arrays, functions |
| [examples/bounce.gb](examples/bounce.gb) | Graphics, mouse, a list of objects as a particle system |
| [examples/goobercatch.gb](examples/goobercatch.gb) | A small complete game |

---

# Language reference

## Basics

- Keywords and names are **case-insensitive** (`print`, `Print` and `PRINT` are the same),
  but underscores count: `draw_rect` and `drawrect` are different names.
- Line ends are only whitespace, as in Lua: a statement ends where its grammar does, so
  one can run over several lines and several can share a line (`x = 1 y = 2`). A `:`
  (or `;`) between statements is allowed but never needed.
- Every call takes parentheses, even with no arguments: `cls()`, `flip()`.
- Comments start with `'` and run to the end of the line. Block comments go between
  `rem` and `endrem`.
- `include "other.gb"` pastes another file in place (each file is included only once).
  Everything is shared between files unless it's declared `local`; see [Files](#files).

## Naming style

GooberBASIC code is written in lowercase, with `snake_case` to separate words:

| What | Style | Examples |
|---|---|---|
| Keywords | lowercase, joined words | `if` `endif` `function` `endfunction` `eachin` |
| Variables, fields, functions | `snake_case` | `spawn_timer`, `reset_game()` |
| Types | `t_` + `snake_case` | `t_enemy`, `t_power_up` |
| Constants | `UPPER_SNAKE_CASE` | `MAX_ENEMIES`, `KEY_ESCAPE` |
| Built-in commands | `snake_case` | `draw_rect`, `key_hit`, `add_last` |
| Calls | always with parentheses | `cls()`, `set_color(255, 0, 0)`, `reset_game()` |

Every built-in name follows this style, so old Blitz spellings like `DrawRect` or
`TList` are not recognized. The compiler suggests the new name:
`Unknown command or function 'DrawRect'. Built-in names are snake_case: did you mean draw_rect?`

## Types and variables

| Type | Suffix | Annotation | Default |
|---|---|---|---|
| Integer (signed 32-bit) | `%` | `:int` | `0` |
| Float (32-bit) | `#` | `:float` | `0.0` |
| Double (64-bit float) | | `:double` | `0.0` |
| Byte (unsigned 8-bit) | | `:byte` | `0` |
| Short (unsigned 16-bit) | | `:short` | `0` |
| Long (unsigned 64-bit) | | `:long` | `0` |
| Sized integers | | `:uint8` `:sint8` `:uint16` `:sint16` `:uint32` `:sint32` `:uint64` `:sint64` | `0` |
| String | `$` | `:string` | `""` |
| User type | | `:t_name` | `null` |
| Array | | `:int[]`, `:float[,]`, ... | `null` |
| List | | `:list<int>`, `:list<t_enemy>`, ... (`:int<>` for short) | `null` |

```basic
local count = 0            ' int by default
local speed# = 2.5         ' suffix style
local name:string = "Bob"  ' annotation style
global score:int           ' global is optional: no local means global
const MAX_ENEMIES = 50, TITLE$ = "Goober"
p:t_player = new t_player  ' declare-on-first-use: a global
```

- **A name is global unless it has `local` in front of it.** Without `strict`, using a
  variable you never declared creates it as a global (typed by its suffix, otherwise
  `int`), even inside a function. So does a `for` counter written without `local`, and
  `dim`. Write `local` (or `for local`) to keep a variable to its function.
- Put `strict` at the top of a file to require `local`/`global` declarations.
- Put `superstrict` there instead to also require a type on everything you
  declare, so nothing quietly becomes an `int`. It does all that `strict` does, and:
  - `local`, `global`, `dim`, `field`, function parameters and `for local` need a
    type, written as `:type` or a suffix (`local speed:float = 1.5`, `local n$`).
  - A function with no return type returns nothing: `return` alone ends it,
    `return value` is an error, and so is using its call as a value. Write
    `function f:int()` for a function that gives a value back.
  - `const` and `for local x = eachin list` may still leave the type out: they
    take the type of the value or the list, they don't default to `int`.
  - It covers the code after the `superstrict` line, so put it first.

```basic
superstrict
local speed:float = 1.5
function hurt(e:t_enemy, amount:int)   ' returns nothing
    e.hp :- amount
endfunction
function alive:int(e:t_enemy)
    return e.hp > 0
endfunction
```
- `local` variables belong to the function they're in (or to the main program).
- A top-level `local` belongs to its file: the functions written in that file see it,
  wherever they stand (like a closure sees the variables around it), but another file
  that includes it, or that it includes, can't. The main program itself sees it only
  from its line on. Every other variable is a global, shared by every function
  and every file.
- A `local` hides a global of the same name inside its function (or file).
- Watch loop counters in functions: `for i = 1 to 10` uses the global `i`, so a function
  that loops with `i` and is called from another `i` loop changes the caller's counter.
  Write `for local i = 1 to 10` in functions.
- Values convert automatically on assignment: between any two number types, and
  numbers ↔ `string`. `float` → integer truncates toward zero. Every type name
  also converts explicitly: `int()`, `byte()`, `sint64()`, `double()`, ..., plus `str()`.

### Number types

`byte`/`uint8`, `short`/`uint16`, `long`/`uint64`, `uint32` and the `sint` types are
fixed-width integers; `sint32` is the same type as `int`. `float` is 32-bit and `double`
is 64-bit.

```basic
local b:byte = 250
b :+ 10                    ' stored values wrap: b is now 4
print(b + 250)             ' 254: maths happens at full width, wrapping only on store
local big:long = 18446744073709551615
local f:float = 0.1        ' 32-bit: 16777216 + 1 is still 16777216
local d:double = 0.1       ' 64-bit
```

- Arithmetic uses the wider operand: 8/16/32-bit integers compute as `int`, `uint32`
  and `sint64` as 64-bit signed, `long`/`uint64` as 64-bit unsigned, and anything with
  a float as a float. Storing into a narrower type wraps (integers) or rounds (`float`).
- `float` with `float` (or with an integer) gives a `float`. Any `double` makes the
  result a `double`. A literal or untyped `const` like `0.5` or `pi` takes the other
  side's type, so `f * 0.5` stays a `float` and `f = 0.1` is true for `f:float = 0.1`.
- Integer literals that don't fit an `int` are 64-bit (`5000000000`). Hex/binary
  literals of up to 32 bits are `int` bit patterns (`$FFFFFFFF` is `-1`, as in Blitz).
  Longer ones are 64-bit.
- A `for` loop over a narrow type checks the end before wrapping, so
  `for b:byte = 250 to 0 step -50` runs 6 times and then stops.
- `abs`, `sgn`, `min` and `max` work on every number type.
- Built-in constants: `true` (1), `false` (0), `pi`, `KEY_*`, `MOUSE_*`.

## Operators (highest precedence first)

| Operators | Notes |
|---|---|
| `^` | power (always `float`) |
| unary `-` `+` `~` | `~` is bitwise NOT |
| `*` `/` `\` `mod` `shl` `shr` `sar` | `int / int` stays an `int` (`7 / 2 = 3`); `\` is integer division for floats too (`7.5 \ 2 = 3`); dividing by 0 clamps to the type's biggest value (smallest if negative, `0 / 0 = 0`); `mod 0` is an error |
| `+` `-` | `+` joins strings; a number joined to a string becomes text |
| `&` `\|` `~` | bitwise AND, OR, XOR |
| `=` `<>` `<` `>` `<=` `>=` | give 1 or 0; strings compare alphabetically |
| `not` | |
| `and` | short-circuit |
| `or` | short-circuit |

Compound assignment: `x :+ 1`, `x :- 1`, `x :* 2`, `x :/ 2`.
Number literals: `255`, `3.14`, `1e6`, `$FF` (hex), `%1010` (binary).
String escapes: `~n` newline, `~t` tab, `~q` quote, `~~` tilde.

## Control flow

```basic
if x > 10 then print("big") endif                         ' one line still needs endif
if x > 10 then print("big") else print("small") endif

if x > 10
    print("big")
elseif x > 5
    print("medium")
else
    print("small")
endif

while lives > 0 ... wend                   ' or endwhile
repeat ... until done                      ' or ... forever
for i = 1 to 10 step 2 ... next            ' step must be a constant; next takes no name
for local i:int = 10 to 0 step -1 ... next

select key
    case 1, 2 : print("one or two")
    case 3    : print("three")
    default   : print("something else")
endselect
```

`exit` leaves the innermost loop, `continue` skips to its next round.
`stop()` ends the program cleanly from anywhere, even inside a function; `return` in the
main program (outside any function) does too. `error(message$)` stops it with an error.

Blocks close with one word: `endif`, `endfunction`, `endtype`, `endselect`, `endwhile`
(or `wend`), `enddata`, `endrem`. `end` is reserved but is not a word on its own, so
`end if` is an error that says to write `endif`.

Every `if` ends with `endif`, and `then` is optional. `elseif` is one word: `else if` is
an `else` whose block starts a new `if`, which needs an `endif` of its own. A `return`
with no value has to
come last in its block (as in Lua), since anything after it that can be a value is
taken as the value to return.

## Functions

```basic
function hypot:float(a:float, b:float)
    return sqr(a * a + b * b)
endfunction

function greet$(name$, greeting$ = "Hello")   ' default argument
    return greeting$ + ", " + name$
endfunction

print(greet("Goober"))
draw_badge(10, 20)     ' a call on its own line still needs its parentheses
```

- A function with no declared return type returns an `int` (0 if nothing is returned).
- Calls always take parentheses, even on a line of their own and with no arguments:
  `set_color(255, 0, 0)`, `cls()`. BlitzBasic's form without them (`set_color 255, 0, 0`)
  is an error, because a function's name on its own is the function itself
  (see [Function values and closures](#function-values-and-closures)).
- Functions can be called before (above) the place they're defined.
- Arrays and objects are passed by reference; numbers and strings by value.
- Every file can call a function. Write `local function` to keep it to the file it's
  written in (see [Files](#files)).

## Function values and closures

Functions are values too: store them in variables, fields, arrays and lists, pass
them to other functions and return them.

```basic
strict
function double_it:int(x:int)
    return x * 2
endfunction

local f:int(int) = double_it              ' a named function, without parentheses
print(f(21))                              ' 42

local log:(string) = function(msg:string) ' a function written where it's used
    print("> " + msg)
endfunction
log("hello")
```

- A function type is the return type, then the parameter types in parentheses:
  `int(int, string)`. One that returns nothing leaves the return type out: `(string)`.
  A function returning a function reads left to right: `int()(int)` takes an `int` and
  gives back an `int()`.
- `function:int(x:int) ... endfunction` makes a function value. It has no name and can go
  anywhere an expression can. Without a return type (`function(x:int)`) it returns nothing.
  Its parameters can't have default values.
- A function variable starts as `null`; compare with `f = null`. Calling a null function
  is a runtime error.
- Lists and arrays of functions work as usual: `local handlers:list<(int)> = new list<(int)>`,
  `local steps:int(int)[4]`. Fields too: `field on_hit:(t_enemy)`.

**Closures.** A function value can use the local variables of the function it's written
in (or of the main program), and keeps them alive after that function has returned:

```basic
function make_counter:int()(stepsize:int)
    local count:int = 0
    return function:int()
        count = count + stepsize          ' count and stepsize live on
        return count
    endfunction
endfunction

local a:int() = make_counter(1)
local b:int() = make_counter(10)
print(a()) : print(a()) : print(b())      ' 1, 2, 10
```

- It's the variable that is shared, not a copy of its value: if two function values use
  the same local, or the outer function changes it later, they all see the change.
- A local belongs to the whole function, so function values made in a loop share the
  loop variable (after `for local i:int = 1 to 3`, they all see `i` as 4). To keep each
  pass's value, pass it to a function that makes the function value.
- Named functions are still declared at the top level, and still can't see the main
  program's locals; only function values reach outside themselves.

## Files

`include "other.gb"` runs another file's top-level code in place. Everything a file
declares is shared with every other file, unless it's declared `local`:

| In a file | Who can use it |
|---|---|
| `local x` at the top level | that file only (its functions included) |
| `local function f()` | that file only |
| `x = 1`, `global x` | every file |
| `function f()`, `global function f()` | every file |
| `type`, `const` | every file |

```basic
' enemies.gb
local spawned:int = 0                    ' enemies.gb's own
local function pick_spot:int()           ' a helper for this file only
    return rnd(0, 100)
endfunction
function spawn_enemy()                   ' main.gb (and any other file) can call this
    print("enemy at " + str(pick_spot()))
endfunction
```

```basic
' main.gb
include "enemies.gb"
spawn_enemy()      ' fine: a shared function
pick_spot()        ' error: 'pick_spot' is a local function of enemies.gb, and only that file can call it
```

Two files may each have a `local function` (or top-level `local`) with the same name:
each one uses its own. A shared function's name can be used only once, by any file.
(`global` in front of a variable or function is still allowed; it changes nothing.)

## Types

Types are **plain data records**: fields only, no methods. Write a regular function
that takes the object as a parameter instead.

```basic
type t_enemy
    field x:float, y:float
    field hp:int
    field name$
endtype

e:t_enemy = new t_enemy
e.x = 100 : e.hp = 3

global enemies:list<t_enemy> = new list<t_enemy>
add_last(enemies, e)

for e:t_enemy = eachin enemies                ' visit every t_enemy in the list, in order
    if e.hp <= 0 then remove(enemies, e) endif      ' removing inside eachin is safe
next

clear(enemies)                                ' drop them all
```

### Object lifetime (garbage collection)

As in BlitzMax, **there is no `delete`**. An object stays alive as long as something
refers to it: a variable, a field, an array element, or a list. Once nothing
does, the garbage collector frees it automatically, including groups of objects
that only refer to each other. To get rid of an object, set its variable to `null`
or `remove` it from its list. Strings and arrays work the same way. Using a
`null` object is a runtime error ("Object does not exist (null)").

The BlitzBasic forms `delete`, `for ... = each t_type`, `after` and `before` were
removed: types no longer keep a hidden list of every instance. The compiler names the
replacement if you use one of them. (`first` and `last` now work on a list: see below.)

## Lists

A list holds values of **one type**, written inside `list<...>`: `list<int>`,
`list<float>`, `list<string>`, a list of one of your types such as `list<t_enemy>`, or a
list of functions such as `list<int(int)>`. Create one with `new`:

```basic
local q:list<int> = new list<int>
add_first(q, 10)
add_last(q, 20)
add_last(q, "some string")          ' compile error: cannot put a string into a list<int>

for local n:int = eachin q          ' visits 10, then 20
    print(n)
next
```

- Putting a value of the wrong type into a list is a **compile error**. The one
  exception is an `int` going into a `list<float>`, which converts it.
- The `eachin` loop variable must have the list's element type. Without `strict`, an
  undeclared loop variable gets that type automatically: `for n = eachin q`.
- Lists can't hold `null`, or other lists. A field or an array can hold a list:
  `field items:list<string>`, `local rows:list<int>[4]`.
- A list variable starts as `null`; give it a list with `new list<int>` before using it,
  or a list literal: `local q:list<int> = <10, 20, 30>` (see below).
- `int<>` is short for `list<int>`, in types and after `new`: `local q:int<> = new int<>`.
- A function can take or return a list: `function evens:list<int>(upto:int)`.
- The old way of writing list types, `int list` and `new t_enemy list`, is a compile error
  that shows the new spelling.

| Command | What it does |
|---|---|
| `add_first(list, value)` · `add_last(list, value)` | put a value at the start / the end |
| `insert(list, n, value)` | make `value` item `n` (items count from 1); `n` below 1 puts it first, past the end puts it last |
| `first(list)` · `last(list)` | the first / last item |
| `remove_first(list)` · `remove_last(list)` | take the first / last item out and return it |
| `remove(list, value)` | take out the first matching item; returns its position, or 0 if none matched |
| `contains(list, value)` | the position of the first matching item, or 0, so it works in an `if` |
| `count(list)` · `count(list, value)` | how many items there are / how many match `value` |
| `is_empty(list)` | 1 if there are none |
| `clear(list)` | take every item out |

`first`, `last`, `remove_first` and `remove_last` return the list's element type
(`first(enemies).hp` works) and are a runtime error on an empty list, so check
`is_empty` first when the list might be empty.

`remove`, `contains` and `count(list, value)` compare by value for `int`, `float` and
`string` lists, and by identity (the same object) for object lists. They search the list,
so they take longer on long lists. The other commands, `count(list)` included, take the
same time however long the list is, except `insert`, which walks to position `n`.

These names are only commands when you call them: a variable or function of your own
called `count`, `first` or `last` still works. The old names (`list_add_last`,
`count_list`, `clear_list` ...) are a compile error that names the new one.

Inside an `eachin` loop you may remove the current item, or any other one, and the loop
carries on correctly. Using `eachin` on a `null` list is a runtime error.

## Arrays

```basic
local scores:int[10]           ' indexes 0..9
dim grid:int[20, 15]           ' dim works too, and makes a global
grid[3, 4] = 1
print(len(scores))             ' 10
print(len(grid, 2))            ' size of dimension 2 -> 15
local names:string[]           ' an array variable with no array yet (null)
```

Arrays are zero-based and bounds-checked. They can hold any type, including objects.
`local a:int[n]` creates a fresh array each time it runs.

## Array and list literals

```basic
local q:int[] = [1, 2, 3, 4, 5]         ' a new one-dimensional array
local p:list<int> = <1, 2, 3, 4, 5>     ' a new list
local w:int<> = <1, 2, 3, 4, 5>         ' the same: int<> is list<int>
local level:int[] = [                   ' a literal may span lines
    1, 1, 1,
    1, 0, 1
]
```

- Each one makes a new array or list every time it runs; the values are any expressions.
- Where the type is known (a declaration, an assignment, an argument, a return value),
  the values convert to it as an assignment would: `local f:float[] = [1, 2]` holds floats,
  and lists follow the usual list rule (an int may go into a float list, not the reverse).
- Elsewhere the first value decides the type: `["a", "b"]` is a `string[]`, and
  `[1, 2.5]` is an error (write `1.0`, or give it a type).
- `[]` and `<>` are empty ones, where the type is known: `local e:int[] = []`.
- Inside `< >` a `>` comparison ends the list, so put comparisons in brackets: `<(a > b)>`.
- `=` and `<>` ask whether two arrays or lists are the same one, and a literal is always a
  new one, so `if q = <> then` would always be false: it's a compile error. Test for no
  items with `is_empty(q)` or `len(a) = 0`.
- Array literals are one-dimensional; arrays can't hold arrays, and lists can't hold
  `null`, lists or arrays. An array of lists is fine: `local g:int<>[] = [<1>, <2, 3>]`.

## Data blocks

`defdata NAME ... enddata` writes a table of values straight into
the code, like BASIC's `DATA`. Point at a block with `restore`, then take the values
out one at a time, in order, with `read_int()`, `read_float()` or `read_string()`:

```basic
restore(LEVEL_1)
local w:int = read_int(), h:int = read_int()
for local y:int = 0 to h - 1
    for local x:int = 0 to w - 1
        grid[x, y] = read_int()
    next
next

defdata LEVEL_1
    4, 2,                  ' width, height
    1, 1, 1, 1,
    1, 0, 0, 1
enddata

defdata ENEMIES
    "slime", 3, 0.5,       ' name, hp, speed
    "bat",   2, 1.25
enddata
```

- Items are separated by commas, across line ends too, so every row but the last ends
  with one (`1, 2` then `-1` on the next line would otherwise be `2 - 1`). Each
  item is a number, a string or a constant expression (`-1`, `SIZE * 2`, `KEY_SPACE`).
- A block can be written anywhere at the top level, above or below the code that reads it.
- Each `read_*` converts the item the way an assignment would: `read_int()` of `2.9`
  is `2`, `read_string()` of `7` is `"7"`.
- `NAME` is an `int` constant: the block's number, counting from 0 in the order the
  blocks are written (across included files too). So `restore(LEVEL_1 + n)` picks the
  n-th block after `LEVEL_1`.
- Reading past the end of a block carries on into the next one. Reading past the last
  item of the last block is a runtime error ("out of data").
- `restore()` with no block goes back to the very first item.
- `data_count(NAME)` is how many items a block holds.

---

# Command reference

Optional arguments are shown in `[brackets]`. String positions are 1-based.
Angles are in **degrees**.

### Console & program
`print([text$])` · `write(text$)` · `input$([prompt$])` · `stop()` · `error(message$)` · `assert(condition [, message$])`

`assert` stops the program with "assertion failed" (plus `message$`, if given)
when `condition` is 0. `message$` is built every time the line runs, even when
the assertion passes.

### Data blocks
`restore([block])` · `read_int()` · `read_float#()` · `read_string$()` · `data_count(block)` (see [Data blocks](#data-blocks))

### Time & random numbers
`millisecs()` · `delay(ms)` · `current_date$()` · `current_time$()` ·
`seed_rnd(seed)` · `rnd#([max#])` / `rnd#(min#, max#)` · `rand(max)` / `rand(min, max)` (inclusive)

### Math
`abs` `sgn` `min` `max` (keep `int` if all arguments are `int`) ·
`sin` `cos` `tan` `asin` `acos` `atan` `atan2(y, x)` · `sqr` `exp` `log` `log10` ·
`floor` `ceil` `round` · `distance(x1, y1, x2, y2)`

### Collision
Each returns 1 or 0.
`in_range(x, low, high)` (low <= x <= high) · `in_rect(x, y, rx, ry, rw, rh)` (point in rect) ·
`rect_in_rect(x1, y1, w1, h1, x2, y2, w2, h2)` (rects overlap) ·
`in_circle(x, y, cx, cy, r)` (point in circle) · `circle_in_circle(x1, y1, r1, x2, y2, r2)` (circles overlap)

A rect covers `x` up to but not including `x + w`, the pixels `draw_rect` fills, so rects
that only share an edge don't overlap. A point on a circle's edge is inside it, but two
circles that only touch don't overlap.

### Memory
`gc_collect()` (run the garbage collector now) · `gc_mem_alloced()` (bytes in use by live objects)

### Strings
`len` · `left$(s, n)` · `right$(s, n)` · `mid$(s, start [, count])` · `upper$` · `lower$` ·
`trim$` · `instr(s, find [, start])` (0 = not found) · `replace$(s, find, with)` ·
`chr$(code)` · `asc(s)` · `hex$(n)` · `bin$(n)` · `repeat_string$(s, n)` · `lset$(s, n)` · `rset$(s, n)` ·
`int(x)` · `float(x)` · `str$(x)`

### Graphics
`graphics(width, height [, fullscreen])` · `end_graphics()` · `app_title(title$)` ·
`graphics_width()` · `graphics_height()` · `app_terminate()` (window close requested) ·
`cls()` · `flip([vsync=1])` · `set_cls_color(r, g, b)` · `set_color(r, g, b)` · `set_alpha(a#)` (0..1) ·
`plot(x, y)` · `draw_line(x1, y1, x2, y2)` · `draw_rect(x, y, w, h [, filled=true])` ·
`draw_oval(x, y, w, h [, filled])` · `draw_circle(cx, cy, r [, filled])` ·
`save_screenshot(file$)`

If your program never calls `app_terminate()`, closing the window ends the program.

### Text
`draw_text(text$, x, y [, center_x, center_y])` · `text_width(text$)` · `text_height()` ·
`load_font(file$ [, size])` · `set_font(handle)` (0 = default font)

The default font is a monospace TTF from your system. Set the `GOOBER_FONT` environment variable to use a different one.

### Images
`load_image(file$)` (PNG/JPG/BMP..., returns 0 on failure) · `draw_image(img, x, y)` ·
`draw_image_ex(img, x, y [, angle#, scale_x#, scale_y#])` (negative scale flips) ·
`draw_image_rect(img, x, y, src_x, src_y, w, h)` (sprite sheets) ·
`mid_handle(img)` · `set_image_handle(img, x, y)` · `image_width(img)` · `image_height(img)` · `free_image(img)`

### Input
`key_down(key)` · `key_hit(key)` (presses since last call) · `wait_key()` · `flush_keys()` ·
`mouse_x()` · `mouse_y()` · `mouse_z()` (wheel) · `mouse_down(button)` · `mouse_hit(button)` ·
`hide_mouse()` · `show_mouse()` · `move_mouse(x, y)`

Keys: `KEY_A`..`KEY_Z`, `KEY_0`..`KEY_9`, `KEY_F1`..`KEY_F12`, `KEY_ESCAPE`, `KEY_SPACE`,
`KEY_ENTER`, `KEY_TAB`, `KEY_BACKSPACE`, `KEY_LEFT/RIGHT/UP/DOWN`, `KEY_LSHIFT`, `KEY_RSHIFT`,
`KEY_LCONTROL`, `KEY_RCONTROL`, `KEY_LALT`, `KEY_RALT`, `KEY_INSERT`, `KEY_DELETE`, `KEY_HOME`,
`KEY_END`, `KEY_PAGEUP`, `KEY_PAGEDOWN`, `KEY_MINUS`, `KEY_EQUALS`, `KEY_COMMA`, `KEY_PERIOD`, `KEY_SLASH`.
Mouse: `MOUSE_LEFT` (1), `MOUSE_RIGHT` (2), `MOUSE_MIDDLE` (3).

### Sound
`load_sound(file$)` (WAV/OGG...) · `play_sound(snd [, loops])` → channel · `stop_channel(ch)` ·
`channel_playing(ch)` · `sound_volume(snd, volume#)` · `free_sound(snd)` ·
`play_music(file$ [, loops=-1])` · `stop_music()`

### Network
One program **hosts** (player 0); up to 7 others **join** (players 1–7). They share
`NET_SLOTS` (256) numbers and can send each other text messages, over TCP or UDP.

`net_host(port [, mode])` → 1 if it worked · `net_join(address$, port [, mode])` → 1 if started ·
`net_status()` · `net_error()` · `net_close()` · `net_update()` ·
`net_id()` (0 host, 1–7 client, -1 none) · `net_is_host()` · `net_peers()` · `net_peer(id)` ·
`net_set(slot, value#)` · `net_get(slot)` → float ·
`net_send(text$ [, to])` · `net_messages()` · `net_recv()` → `""` if none · `net_from()`

Constants: `NET_TCP` (the default mode), `NET_UDP`; `net_status()` gives `NET_OFF`, `NET_CONNECTING`,
`NET_CONNECTED` or `NET_LOST`; `NET_ALL` (-1, `net_send`'s default), `NET_SLOTS` (256), `NET_MAX_PLAYERS` (8).

- The host owns the slots. Anyone may `net_set` one: your copy changes at once, and a client's change goes
  to the host, which passes it on to everyone. Only changed slots are sent.
- Messages (1–255 characters) arrive in order, exactly once, on both TCP and UDP. The host relays them.
- Nothing blocks. The sockets are read and written when `flip()`, `delay()`, `net_update()`, `net_status()`
  or `net_recv()` run, so a program without a window should call `delay()` in its loop.
- A connection that is silent for 5 seconds is dropped; joining gives up after 5 seconds. IPv4 only.

```
if not net_host(4040) then print("can't host: " + net_error()) return endif
repeat
    net_set(0, net_get(0) + 1)                   ' everyone sees the count go up
    while net_messages() > 0 : print("player " + net_from() + ": " + net_recv()) : wend
    delay(16)
forever
```

---

# How it works

```
source.gb   ──lexer──▶ tokens ──prescan──▶ symbol tables ──compiler──▶ bytecode ──▶ VM
                (include spliced here)   (types, functions, globals)      (typed ops)
```

| File | Role |
|---|---|
| `src/lexer.c` | Tokens; handles `rem`, `..`, `include` |
| `src/compiler.c` | Prescan, then single-pass recursive descent straight to bytecode, with type checking and constant folding (`W / 2 - 10` becomes one constant) |
| `src/vm.c` | Stack VM dispatch loop, runtime errors with call trace, `--disasm` |
| `src/objects.c` | Strings, instances, arrays, lists, mark-and-sweep GC |
| `src/builtins.c` | Console/string/math/time natives + native registry |
| `src/gfx.c` | **All** SDL code: window, drawing, text, images, input, sound |
| `src/net.c` | Networking: TCP and UDP sockets, the shared slots and messages (also linked into GooberEngine) |

Debugging aids: `GOOBER_GC_STRESS=1` runs the garbage collector on every allocation.
`GOOBER_NET_LOSS=30` drops 30% of the UDP packets the program sends, to test a bad connection.

## Roadmap ideas
- Port `src/gfx.c` to SDL3 (nothing else depends on SDL).
- `eachin` over arrays.
- File I/O (`read_file`, `write_line`, ...), `split` returning `string[]`.
- Render-to-image (`create_image`, `set_buffer`), pixel collision, animated image strips.
- `goto`/`gosub` and `data`/`read` for classic-BASIC compatibility.

## License

GooberBASIC is free software, under the [GNU General Public License, version 3](LICENSE), with the [GooberEngine Game Exception](GAME-EXCEPTION.txt), which it shares with GooberEngine.

- **The interpreter stays free.** If you share GooberBASIC or a modified version of it, you must do so under the GPLv3, with its source.
- **Your programs are yours.** A program isn't covered by the GPLv3 just because it's written in GooberBASIC or run by it. License it however you want. The programs in `examples/` can be copied into your own freely.
