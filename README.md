# UD

**UD** is a small, fast scripting language with a bytecode compiler and a single
virtual machine at its core. It blends the feel of Lua (blocks closed by `end`,
no curly braces), the optional typing of C++, and the readability of Python.

```ud
int function entry()
    cout("Hello from UD!")
    return 0
end
```

- **Fast.** Source compiles straight to bytecode and runs on a stack VM that uses
  computed-goto dispatch on GCC/Clang. No tree-walking.
- **Batteries included.** Arrays, dictionaries, sets, structs, lambdas with
  higher-order methods (`map`/`filter`/`reduce`/`sort`), `try`/`catch`/`throw`,
  `require` for splitting code across files, and built-in `math`, `random`,
  `regex`, file, and string libraries.
- **One VM, every way to run.** Interpret a script, compile it to a portable
  `.ldx` bytecode file, run a `.ldx`, or build a **standalone `.ldx`** that runs
  on its own with no `ud` installed — all through the exact same VM.
- **Tiny.** The whole `ud` binary is ~180 KB.
- **Friendly failures.** Every error is numbered with a one-line plain-language
  reason. UD never shows you a raw C crash or a segfault.

---

## Building

You need a C11 compiler (GCC or Clang). There are no dependencies.

**With the build script (recommended):**

```sh
./build.sh        # Linux / macOS / MSYS2 on Windows
build.bat         # Windows cmd.exe
```

**With make:**

```sh
make              # optimized build -> ./ud (or ud.exe on Windows)
make examples     # build + run every example
```

**By hand:**

```sh
gcc -std=c11 -O2 -ffunction-sections -fdata-sections \
    -fno-asynchronous-unwind-tables -fno-unwind-tables \
    -Isrc -o ud src/*.c -s -Wl,--gc-sections
```

To put `ud` on your PATH everywhere (and associate `.ud` files + install the
VS Code extension), use the bootstrap installer described in **Installing** below.

---

## Running

Everything shares one compiler and one VM:

| Command | What it does |
|---|---|
| `ud file.ud` | compile in memory and run |
| `ud build file.ud [-o out.ldx]` | compile to a **standalone** `.ldx` that runs on its own |
| `ud build file.ud --thin` | compile to a portable `.ldx` (needs `ud run`) |
| `ud run file.ldx` | run a compiled `.ldx` (thin or standalone) |
| `ud --version` | print the version |
| `ud --help` | show usage |

```sh
ud examples/fizzbuzz.ud
ud build examples/fib.ud -o fib.ldx   # standalone: fib.ldx runs by itself
ud run fib.ldx                        # ...and is still a normal .ldx
```

A `.ldx` holds only bytecode, constants, and type tags — never your source, so it
**cannot be turned back into readable UD or C**.

### Standalone vs. thin `.ldx`

`ud build` produces a **standalone** `.ldx` by default: the portable bytecode with
a full copy of the `ud` runtime fused in front of it. The result is *also* a native
executable, so it runs on a machine with no `ud` installed — double-click it, or
rename it to `.exe` and run it — while **still being a valid `.ldx`** that `ud run`
accepts. (On Linux/macOS `chmod +x` is set for you; the payload is placed after the
executable, which the OS ignores.)

Pass `--thin` when you'd rather have the small, cross-platform payload on its own.
A thin `.ldx` is a handful of bytes and runs on any platform, but only through
`ud run`. A standalone `.ldx` is bigger (it carries the ~180 KB runtime) and is
tied to the platform it was built on.

| | standalone (default) | `--thin` |
|---|---|---|
| Runs on its own | yes | no — needs `ud run` |
| `ud run` works | yes | yes |
| Cross-platform | no (native to the build OS) | yes |
| Size | payload + ~180 KB runtime | just the payload |

---

## The language

### The entry point (required)

Every UD program must define its starting point, exactly like `main()` in C:

```ud
int function entry()
    return 0
end
```

If it is missing you get **UD Error 1**. Whatever `entry()` returns becomes the
process exit code, so `return 0` means success.

The top level of a file may only contain **function** and **struct** definitions.
All runnable code lives inside functions.

### Comments

```ud
-- this is a comment (primary)
// this is also a comment (alias)
```

### Types

UD has four value types you can name: `int`, `float`, `bool`, `string`, plus
dynamic `array`s, `dict`s, `set`s, `struct`s, first-class `function`s, and `nil`.

### Functions

```ud
-- typed return + typed parameters
int function add(int a, int b)
    return a + b
end

-- untyped function, untyped parameters
function greet(name)
    cout("Hi, " .. name)
end
```

A leading type (`int`/`float`/`bool`/`string`) gives the return type. Parameters
may be individually typed (`int a`) or left untyped (`a`). Typed values are
coerced at the boundary: an `int` parameter given `8.5` truncates, a `float`
parameter given `8` promotes to `8.0`.

### Variables

```ud
int count = 0            -- typed local (validates + coerces the value)
string name = "UD"
message = "hi"           -- untyped local (first assignment declares it)
count += 1               -- compound assignment
```

Locals are block-scoped. A typed declaration coerces its initializer to the
declared type; a bare `name = value` just declares a local of whatever type the
value already is.

### Operators

| Group | Operators |
|---|---|
| Arithmetic | `+`  `-`  `*`  `/`  `%`  `**` (power) |
| String | `..` (concatenation — turns any value into text) |
| Comparison | `==`  `!=`  `<`  `>`  `<=`  `>=` |
| Logical | `and`  `or`  `not`  (aliases: `&&`  `\|\|`  `!`) |
| Bitwise | `&`  `\|`  `^`  `~`  `<<`  `>>` |
| Conditional | `cond ? then : else` (ternary) |
| Assignment | `=`  `+=`  `-=`  `*=`  `/=`  `%=` |

`+` is for numbers only; use `..` to join text. `int / int` gives an `int`
(truncated) — use floats for real division. `**` is exponentiation.

The ternary `?:` is an expression, so it slots anywhere a value is expected:

```ud
label = n % 2 == 0 ? "even" : "odd"
cout("the max is " .. (a > b ? a : b))
```

### Control flow

```ud
if x > 0 then
    cout("positive")
elseif x < 0 then
    cout("negative")
else
    cout("zero")
end

while i < 10 do
    i += 1
end

for i = 0, 9 do          -- 0 to 9 inclusive
    cout(i)
end

for i = 0, 10, 2 do      -- with a step
    cout(i)
end

for item in myArray do   -- iterate a collection
    cout(item)
end
```

`break` and `continue` work inside every loop.

### while...unless (UD's signature loop)

`while ... unless` is a `while` loop with a second exit condition checked **at the
bottom of each pass**. The body runs first, then the `unless` condition is tested;
the moment it becomes true, the loop exits immediately.

```ud
int n = 0
while true do
    cout("n = " .. n)
    n += 1
unless n >= 3 end
```

This prints `n = 0`, `n = 1`, `n = 2` and then stops — because after the pass
where `n` becomes `3`, the `unless n >= 3` at the bottom is true, so the loop ends
without running the body again. Think of it as "keep looping... unless this."

### Arrays

```ud
nums = [10, 20, 30]
nums.append(40)          -- also: push
last = nums.pop()
cout(nums[0])            -- indexing
cout(nums[-1])           -- negative indexes count from the end
cout(nums[1:3])          -- slicing, Python-style [start:stop)
cout(len(nums))          -- length
```

Array methods: `append` / `push`, `pop`, `length` / `len`, `contains`, `index` /
`find`, `join`.

### Strings

```ud
string s = "  Hello, World  "
cout(s.trim())                 -- also: strip
cout(s.upper())
cout(s.lower())
cout(s.replace("World", "UD"))
cout(s.split(","))             -- -> array
cout(s.find("World"))          -- index, or -1
cout(len(s))
```

String methods: `upper`, `lower`, `trim` / `strip`, `replace`, `split`,
`find` / `index`, `contains`, `length` / `len`, `join`.

### Dictionaries

A dict maps keys to values. Write one as `{ key: value, ... }`; index it with
`[]`; assigning to a new key inserts it.

```ud
ages = { "alice": 30, "bob": 25 }
ages["carol"] = 28             -- insert or update
cout(ages["alice"])            -- read
cout(ages.get("dave", 0))      -- read with a default (no error if absent)
cout(ages.has("bob"))          -- membership test
ages.remove("bob")
for name in ages.keys() do     -- keys()/values() return arrays
    cout(name .. " -> " .. ages[name])
end
```

Dict methods: `get` (optional default), `set`, `has` / `contains`, `remove` /
`delete`, `keys`, `values`, `length` / `len`. An empty `{}` is a dict.

### Sets

A set stores unique values. Write one as `{ value, ... }` — duplicates collapse.

```ud
colors = { "red", "green", "red" }   -- two elements
colors.add("blue")
cout(colors.has("green"))
cout(colors.length())
cout(colors.values().sort().join(", "))
```

Set methods: `add`, `has` / `contains`, `remove` / `delete`, `values` / `items`,
`length` / `len`. (`{}` is an empty dict; a set literal always has at least one
element, so build an empty set by adding to one.)

### Structs

```ud
struct Point
    float x
    float y
end

int function entry()
    p = Point(3.0, 4.0)        -- construct by calling it
    cout(p.x)                  -- read a field
    p.x = 6.0                  -- fields are mutable
    return 0
end
```

Fields are typed and coerced on assignment.

### Lambdas and higher-order methods

A **lambda** is an anonymous `function(...) ... end` used as a value. Lambdas are
first-class: store them in a variable, pass them as arguments, return them, call
them. (They do not capture surrounding locals — pass what they need as arguments.)

```ud
double = function(x) return x * 2 end
cout(double(21))                       -- 42

apply = function(f, x) return f(x) end
cout(apply(double, 5))                 -- 10
```

Arrays carry higher-order methods that take a lambda and re-enter the VM for each
element:

```ud
nums = [1, 2, 3, 4, 5]
nums.map(function(x) return x * x end)          -- [1, 4, 9, 16, 25]
nums.filter(function(x) return x % 2 == 1 end)  -- [1, 3, 5]
nums.reduce(function(a, b) return a + b end, 0) -- 15  (optional seed)
nums.sort(function(a, b) return a > b end)      -- [5, 4, 3, 2, 1]
nums.foreach(function(x) cout(x) end)           -- also: each
```

`map`, `filter`, and `sort` return **new** arrays (the original is untouched);
`sort` with no argument orders numbers or strings naturally, or takes a comparator
that returns whether the first item should come first (a bool) or an ordering
number (`<0`, `0`, `>0`). `reduce`'s seed is optional — without it, the first
element starts the accumulation.

### Input and output

```ud
cout("hello")                  -- print (adds a newline; any number of args)
cout("x =", x)                 -- multiple args are space-separated

string name = cin("Name? ")    -- read a line; the prompt is optional
```

`cin` validates against the **declared type** of the variable receiving it:

| Declaration | Accepts | Rejects |
|---|---|---|
| `string s = cin(...)` | anything | — |
| `int n = cin(...)` | whole numbers | words, decimals like `8.5` (**UD Error 10**) |
| `float f = cin(...)` | `8` or `8.5` (promotes ints) | non-numbers (**UD Error 11**) |
| `bool b = cin(...)` | `true`/`false`/`1`/`0` | anything else (**UD Error 12**) |

A bare `cin(...)` with no declared type yields a string.

There are also four conversion functions: `int(x)`, `float(x)`, `bool(x)`,
`string(x)`, plus `len(x)` and `type(x)`.

### `format` and `sleep`

`format(template, ...)` fills `{}` placeholders in order (write `{{` / `}}` for
literal braces); `sleep(seconds)` pauses (fractions are fine):

```ud
cout(format("{} of {} ({}%)", 3, 4, 75))   -- "3 of 4 (75%)"
sleep(0.5)                                  -- half a second
```

### Files

Four builtins cover text files, all relative to the working directory:

```ud
write_file("out.txt", "first\n")     -- create/overwrite
append_file("out.txt", "second\n")   -- add to the end
if file_exists("out.txt") then
    cout(read_file("out.txt"))       -- whole file as a string
end
```

---

## Standard library

Three modules are always available — no import needed.

### `math`

Constants `math.pi`, `math.e`, `math.tau`, `math.inf`, and functions `sqrt`,
`cbrt`, `abs`, `sign`, `floor`, `ceil`, `round`, `trunc`, `exp`, `log`, `log10`,
`log2`, `pow`, `hypot`, `gcd`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`,
`atan2`, plus variadic `min` / `max`.

```ud
cout(math.sqrt(2))          -- 1.4142135623731
cout(math.max(3, 9, 2))     -- 9
cout(math.gcd(48, 36))      -- 12
```

### `random`

| Call | Result |
|---|---|
| `random.integer(lo, hi)` | whole number in `[lo, hi]` (inclusive) |
| `random.range(lo, hi)` | whole number in `[lo, hi)` (excludes `hi`) |
| `random.float()` | float in `[0, 1)` |
| `random.bool()` | `true` or `false` |
| `random.choice(array)` | a random element |
| `random.shuffle(array)` | shuffles the array in place, returns it |
| `random.seed(n)` | seed the generator for reproducible runs |

```ud
random.seed(42)
cout(random.integer(1, 6))              -- a dice roll
cout(random.choice(["a", "b", "c"]))
```

### `regex`

| Call | Result |
|---|---|
| `regex.test(pattern, s)` | `true` if the pattern matches anywhere |
| `regex.match(pattern, s)` | the first matching substring, or `nil` |
| `regex.find_all(pattern, s)` | an array of every match |
| `regex.replace(pattern, s, with)` | `s` with every match replaced |

```ud
cout(regex.test("^[0-9]+$", "2024"))            -- true
cout(regex.find_all("[a-z]+", "ab_cd").join(",")) -- "ab,cd"
cout(regex.replace("[0-9]", "id=42", "*"))      -- "id=**"
```

---

## Error handling

Wrap risky code in `try` / `catch`. A runtime error (like division by zero) is
delivered to the nearest `catch` as a message; `throw` raises your own error with
any value, and `catch (name)` binds it — the parentheses are required to name it.

```ud
try
    x = 10 / 0
catch (e)
    cout("caught: " .. e)      -- "caught: you divided by zero"
end

try
    throw 404                  -- throw any value; its type is preserved
catch (code)
    cout(code * 2)             -- 808 — still an int
end
```

A thrown value keeps its type, so an `int` you `throw` is still a number in the
handler. `try`/`catch` blocks nest, and a `catch` may re-`throw` to an outer one.
An uncaught `throw` that reaches the top prints as **UD Error 22** and stops the
program.

---

## Modules

`require("file.ud")` splices another file's top-level definitions into the current
program **at compile time**. Paths resolve relative to the requiring file, repeats
are deduped, and cycles are broken automatically, so a built `.ldx` stays
self-contained — the modules travel inside it.

```ud
-- geometry.ud  (a module: no entry(), just definitions)
const PI = 3.14159
function circle_area(r) return PI * r * r end
```

```ud
-- main.ud
require("geometry.ud")

int function entry()
    cout(circle_area(2))       -- 12.56636
    return 0
end
```

`require` may appear only at the top level of a file, and a required module should
not define `entry()` (that belongs to the program you run).

---

## Error numbers

Every failure funnels through one channel and prints:

```
UD Error <N>: <short description>
  Why: <one-line reason>
  at line <L>
```

| # | Meaning |
|---|---|
| 1 | No entry point (`int function entry()` missing) |
| 2 | Syntax error |
| 3 | Use of a variable that was never assigned |
| 4 | Call to a function/global that doesn't exist |
| 5 | Operation applied to the wrong type |
| 6 | Wrong number of arguments |
| 7 | Division or modulo by zero |
| 8 | Array/string index out of bounds |
| 9 | Indexing something that isn't an array or string |
| 10 | Typed `cin` expected an integer |
| 11 | Typed `cin` expected a number |
| 12 | Typed `cin` expected a boolean |
| 13 | Unknown struct field |
| 14 | Unknown struct type / bad construction |
| 15 | Could not read/write a file |
| 16 | `.ldx` file missing, corrupt, or wrong version |
| 17 | Call depth exceeded (stack overflow) |
| 18 | Malformed slice bounds |
| 19 | Unknown method/attribute on a value |
| 20 | Tried to call a non-function value |
| 21 | Invalid explicit type conversion |
| 22 | Uncaught error — a `throw` reached the top of the program |
| 99 | Internal error (should never happen) |

---

## Examples

Everything in [`examples/`](examples/) runs out of the box:

| File | Shows |
|---|---|
| `hello.ud` | the minimal program |
| `fizzbuzz.ud` | for-range, if/elseif/else, `%` |
| `operators.ud` | arithmetic, power, bitwise, comparisons |
| `strings.ud` | the string method toolkit |
| `arrays.ud` | append, index, slice, iterate, pop |
| `structs.ud` | defining and using records |
| `loops.ud` | while, `while...unless`, for, break, continue |
| `fib.ud` | recursion with typed functions |
| `input.ud` | typed `cin` |
| `evenorodd.ud` | typed function + `cin`, `%` |
| `collections.ud` | dictionaries and sets |
| `functional.ud` | lambdas, `map`/`filter`/`reduce`/`sort`, ternary |
| `errors.ud` | `try` / `catch` / `throw` |
| `stdlib.ud` | `math`, `random`, `regex`, `format`, files |
| `modules.ud` | `require` (imports `geometry.ud`) |

```sh
ud examples/loops.ud
```

---

## Installing

The bootstrap installer builds `ud`, puts it on your PATH, gives `.ud` and `.ldx`
files their icons and associations (double-click a `.ud` to run it, a `.ldx` to
run the compiled program), and installs the VS Code extension:

```sh
# Linux / macOS / MSYS2
./install.sh

# Windows PowerShell
powershell -ExecutionPolicy Bypass -File install.ps1
```

Run `ud --version` afterward to confirm the install.

---

## Project layout

```
src/            the language: lexer, parser, compiler, VM, serializer, CLI
examples/       runnable .ud programs
editor/vscode/  the VS Code extension (syntax highlighting)
assets/         the .ud / .ldx icons (and the script that draws them)
build.sh · build.bat · Makefile   three ways to build the same binary
install.sh · install.ps1          bootstrap installers
```

## How it fits together

```
source.ud ─► lexer ─► parser ─► compiler ─► bytecode ─┬─► VM ─► output
                                                       │
                                          serialize ◄──┘
                                              │
                                    ┌─────────┴───────────┐
                                thin .ldx           standalone .ldx
                                (payload)      (ud runtime + payload)
                                    │                     │
                                 ud run              runs on its own
                                    └────────► VM ◄────────┘
```

One lexer, one parser, one compiler, one VM. `build` just stops after the
compiler and writes the bytecode to disk; `run` reads it straight back into the
same VM. A standalone `.ldx` carries a copy of that VM with it.

## License

MIT.
