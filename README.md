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
- **One VM, three ways to run.** Interpret a script, compile it to a portable
  `.ldx` bytecode file, or run a `.ldx` — all through the exact same VM.
- **Tiny.** The whole `ud` binary is ~120 KB.
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
    -o ud *.c -s -Wl,--gc-sections
```

To put `ud` on your PATH everywhere (and associate `.ud` files + install the
VS Code extension), use the bootstrap installer described in **Installing** below.

---

## Running

UD has three modes, all sharing one compiler and one VM:

| Command | What it does |
|---|---|
| `ud file.ud` | compile in memory and run |
| `ud build file.ud [-o out.ldx]` | compile to portable `.ldx` bytecode |
| `ud run file.ldx` | run a compiled `.ldx` |
| `ud --version` | print the version |
| `ud --help` | show usage |

```sh
ud examples/fizzbuzz.ud
ud build examples/fib.ud -o fib.ldx
ud run fib.ldx
```

A `.ldx` holds only bytecode, constants, and type tags — never your source. It is
a little-endian container that runs unchanged on any platform, and it **cannot be
turned back into readable UD or C**.

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
dynamic `array`s, `struct`s, and `nil`.

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
| Assignment | `=`  `+=`  `-=`  `*=`  `/=`  `%=` |

`+` is for numbers only; use `..` to join text. `int / int` gives an `int`
(truncated) — use floats for real division. `**` is exponentiation.

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

There are also three conversion functions: `int(x)`, `float(x)`, `bool(x)`,
`string(x)`, plus `len(x)` and `type(x)`.

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

```sh
ud examples/loops.ud
```

---

## Installing

The bootstrap installer builds `ud`, puts it on your PATH, associates `.ud`
files, and installs the VS Code extension:

```sh
# Linux / macOS / MSYS2
./install.sh

# Windows PowerShell
powershell -ExecutionPolicy Bypass -File install.ps1
```

Run `ud --version` afterward to confirm the install.

---

## How it fits together

```
source.ud ─► lexer ─► parser ─► compiler ─► bytecode ─┬─► VM ─► output
                                                       │
                                          serialize ◄──┘
                                              │
                                          file.ldx ─► VM ─► output
```

One lexer, one parser, one compiler, one VM. `build` just stops after the
compiler and writes the bytecode to disk; `run` reads it straight back into the
same VM.

## License

MIT.
