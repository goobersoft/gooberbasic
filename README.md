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
- One statement per line, or several separated by `:` (or `;`).
- Comments start with `'`. Block comments go between `rem` and `endrem`.
- End a line with `..` to continue it on the next line.
- `include "other.gb"` pastes another file in place (each file is included only once).

## Naming style

GooberBASIC code is written in lowercase, with `snake_case` to separate words:

| What | Style | Examples |
|---|---|---|
| Keywords | lowercase, joined words | `if` `endif` `function` `endfunction` `eachin` |
| Variables, fields, functions | `snake_case` | `spawn_timer`, `reset_game()` |
| Types | `t_` + `snake_case` | `t_enemy`, `t_power_up` |
| Constants | `UPPER_SNAKE_CASE` | `MAX_ENEMIES`, `KEY_ESCAPE` |
| Built-in commands | `snake_case` | `draw_rect`, `key_hit`, `list_add_last` |
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
| List | | `:int list`, `:t_enemy list`, ... | `null` |

```basic
local count = 0            ' int by default
local speed# = 2.5         ' suffix style
local name:string = "Bob"  ' annotation style
global score:int           ' visible inside functions too
const MAX_ENEMIES = 50, TITLE$ = "Goober"
p:t_player = new t_player  ' declare-on-first-use, Blitz style
```

- Without `strict`, using a variable you never declared creates it as a local
  (typed by its suffix, otherwise `int`), like BlitzBasic.
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
  Main-program locals are **not** visible inside functions; use `global` for that.
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
| `*` `/` `mod` `shl` `shr` `sar` | `int / int` stays an `int` (`7 / 2 = 3`) |
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
if x > 10 then print("big")               ' single line
if x > 10 then print("big") else print("small")

if x > 10
    print("big")
elseif x > 5
    print("medium")
else
    print("small")
endif                                     ' or end if

while lives > 0 ... wend                   ' or endwhile
repeat ... until done                      ' or ... forever
for i = 1 to 10 step 2 ... next            ' step must be a constant
for local i:int = 10 to 0 step -1 ... next

select key
    case 1, 2 : print("one or two")
    case 3    : print("three")
    default   : print("something else")
endselect
```

`exit` leaves the innermost loop, `continue` skips to its next round.
`end` stops the program.

## Functions

```basic
function hypot:float(a:float, b:float)
    return sqr(a * a + b * b)
endfunction

function greet$(name$, greeting$ = "Hello")   ' default argument
    return greeting$ + ", " + name$
endfunction

print(greet("Goober"))
draw_badge(10, 20)     ' a call on its own line: parentheses recommended
```

- A function with no declared return type returns an `int` (0 if nothing is returned).
- Write calls with parentheses, even on a line of their own: `set_color(255, 0, 0)`.
  BlitzBasic's form without them (`set_color 255, 0, 0`) still works, but the examples don't use it.
- Functions can be called before (above) the place they're defined.
- Arrays and objects are passed by reference; numbers and strings by value.

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

global enemies:t_enemy list = new t_enemy list
list_add_last(enemies, e)

for e:t_enemy = eachin enemies                ' visit every t_enemy in the list, in order
    if e.hp <= 0 then list_remove(enemies, e) ' removing inside eachin is safe
next

clear_list(enemies)                           ' drop them all
```

### Object lifetime (garbage collection)

As in BlitzMax, **there is no `delete`**. An object stays alive as long as something
refers to it: a variable, a field, an array element, or a list. Once nothing
does, the garbage collector frees it automatically, including groups of objects
that only refer to each other. To get rid of an object, set its variable to `null`
or `list_remove` it from its list. Strings and arrays work the same way. Using a
`null` object is a runtime error ("Object does not exist (null)").

The BlitzBasic forms `delete`, `for ... = each t_type`, `first`, `last`, `after` and
`before` were removed: types no longer keep a hidden list of every instance. The
compiler names the replacement if you use one of them.

## Lists

A list holds values of **one type**, written before the word `list`: `int list`,
`float list`, `string list`, or a list of one of your types such as `t_enemy list`.
Create one with `new`:

```basic
local q:int list = new int list
list_add_first(q, 10)
list_add_last(q, 20)
list_add_last(q, "some string")     ' compile error: cannot put a string into an int list

for local n:int = eachin q          ' visits 10, then 20
    print(n)
next
```

- Putting a value of the wrong type into a list is a **compile error**. The one
  exception is an `int` going into a `float list`, which converts it.
- The `eachin` loop variable must have the list's element type. Without `strict`, an
  undeclared loop variable gets that type automatically: `for n = eachin q`.
- Lists can't hold `null`, or other lists. A field or an array can hold a list:
  `field items:string list`, `local rows:int list[4]`.
- A list variable starts as `null`; give it a list with `new int list` before using it.

`list_add_last(list, value)` · `list_add_first(list, value)` · `list_remove(list, value)` ·
`list_contains(list, value)` · `count_list(list)` · `list_is_empty(list)` · `clear_list(list)`

`list_remove` removes the first matching item. It and `list_contains` compare by value
for `int`, `float` and `string` lists, and by identity (the same object) for object lists.
Both search the list, so they take longer on long lists.

Inside an `eachin` loop you may remove the current item, or any other one, and the loop
carries on correctly. Using `eachin` on a `null` list is a runtime error.

## Arrays

```basic
local scores:int[10]           ' indexes 0..9
dim grid:int[20, 15]           ' dim works too; at top level it makes a global
grid[3, 4] = 1
print(len(scores))             ' 10
print(len(grid, 2))            ' size of dimension 2 -> 15
local names:string[]           ' an array variable with no array yet (null)
```

Arrays are zero-based and bounds-checked. They can hold any type, including objects.
`local a:int[n]` creates a fresh array each time it runs.

---

# Command reference

Optional arguments are shown in `[brackets]`. String positions are 1-based.
Angles are in **degrees**.

### Console & program
`print([text$])` · `write(text$)` · `input$([prompt$])` · `runtime_error(message$)` · `end`

### Time & random numbers
`millisecs()` · `delay(ms)` · `current_date$()` · `current_time$()` ·
`seed_rnd(seed)` · `rnd#([max#])` / `rnd#(min#, max#)` · `rand(max)` / `rand(min, max)` (inclusive)

### Math
`abs` `sgn` `min` `max` (keep `int` if all arguments are `int`) ·
`sin` `cos` `tan` `asin` `acos` `atan` `atan2(y, x)` · `sqr` `exp` `log` `log10` ·
`floor` `ceil` `round` · `distance(x1, y1, x2, y2)` ·
`rects_overlap(x1, y1, w1, h1, x2, y2, w2, h2)`

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

---

# How it works

```
source.gb   ──lexer──▶ tokens ──prescan──▶ symbol tables ──compiler──▶ bytecode ──▶ VM
                (include spliced here)   (types, functions, globals)      (typed ops)
```

| File | Role |
|---|---|
| `src/lexer.c` | Tokens; merges `end if`/`else if` into `endif`/`elseif`, handles `rem`, `..`, `include` |
| `src/compiler.c` | Prescan, then single-pass recursive descent straight to bytecode, with type checking and constant folding (`W / 2 - 10` becomes one constant) |
| `src/vm.c` | Stack VM dispatch loop, runtime errors with call trace, `--disasm` |
| `src/objects.c` | Strings, instances, arrays, lists, mark-and-sweep GC |
| `src/builtins.c` | Console/string/math/time natives + native registry |
| `src/gfx.c` | **All** SDL code: window, drawing, text, images, input, sound |

Debugging aids: `GOOBER_GC_STRESS=1` runs the garbage collector on every allocation.

## Roadmap ideas
- Port `src/gfx.c` to SDL3 (nothing else depends on SDL).
- `eachin` over arrays; `list_first`/`list_last` (they can return the element type now that lists are typed).
- File I/O (`read_file`, `write_line`, ...), `split` returning `string[]`.
- Render-to-image (`create_image`, `set_buffer`), pixel collision, animated image strips.
- `goto`/`gosub` and `data`/`read` for classic-BASIC compatibility.
