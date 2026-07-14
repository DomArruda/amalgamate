# Amalgamate

A single-file C++ source amalgamator. It recursively inlines `#include` directives to produce a self-contained translation unit, while leaving standard library headers untouched.

Originally based on Vinnie Falco's Amalgamate, rewritten for C++17 with `std::filesystem`, content-hash deduplication, and cleaner output.

---

## Features

- **Recursive inlining** — Transitive `#include "..."` directives are flattened into a single file
- **Standard library detection** — `<iostream>`, `<vector>`, `<string.h>`, etc. are left as-is
- **Include guard stripping** — `#ifndef` / `#define` / `#endif` guard blocks are removed from inlined headers to reduce noise
- **Content-hash deduplication** — Identical files reached via different paths are inlined only once
- **Recursion limit** — Hard cap at 100 levels to prevent runaway expansion
- **Clean markers** — Inlined sections are marked with compact `// === filename ===` comments, indented two spaces per level of nesting
- **Cross-platform** — Uses `std::filesystem`, works on Linux, macOS, and Windows (WSL/MSYS2)

---

## Build

Requires a C++17 compiler and a standard library with `std::filesystem` support.

```bash
# GCC
g++ -std=c++17 -O2 -pthread -o amalgamate amalgamate.cpp

# Clang
clang++ -std=c++17 -O2 -pthread -o amalgamate amalgamate.cpp
```

---

## Usage

```bash
./amalgamate [options] <inputFile> <outputFile>
```

### Options

| Option | Description |
|---|---|
| `-s` | Inline system headers (angle brackets). Disabled by default. |
| `-t` | Convert leading spaces to tabs |
| `-v` | Verbose output |
| `-w <wildcards>` | Comma-separated list of file patterns to inline (default: `*.cpp;*.c;*.hpp;*.h`) |
| `-f <file>` | Force reinclusion of a specific file or macro |
| `-p <file>` | Prevent reinclusion of a specific file or macro |
| `-d <name>=<file>` | Define a macro-to-file mapping for macro-based `#include` lines |
| `-i <dir>` | Additional search directory for resolving `#include` paths |
| `-h, --help` | Print usage and exit |

---

## Example

Given a project:

```
src/
├── main.cpp
├── add.cpp
├── add.hpp
└── adder.hpp
```

**main.cpp**

```cpp
#include "add.cpp"
#include <iostream>
#include <vector>
#include <string.h>

int main()
{
    Adder a{10, 2};
    std::cout << "Hello World: " << add(10, 2) << "\n";
    std::cout << "Hello World: " << add(a) << "\n";
}
```

**add.cpp**

```cpp
#include "add.hpp"

int add(int x, int y)
{
    return x + y;
}

int add(Adder adder)
{
    return adder.x + adder.y;
}
```

**add.hpp**

```cpp
#include "adder.hpp"

#ifndef ADD
#define ADD

int add(int x, int y);
int add(Adder adder);

#endif
```

**adder.hpp**

```cpp
#ifndef ADDER
#define ADDER

struct Adder
{
    int x{};
    int y{};

    Adder(int _x, int _y): x(_x), y(_y){}
    ~Adder() = default;
};

#endif
```

Run:

```bash
./amalgamate src/main.cpp out.cpp
```

**out.cpp**

```cpp
// === add.cpp ===
  // === add.hpp ===
    // === adder.hpp ===
#define ADDER

struct Adder
{
    int x{};
    int y{};

    Adder(int _x, int _y): x(_x), y(_y){}
    ~Adder() = default;

};
    // === end adder.hpp ===
#ifndef ADD
#define ADD

int add(int x, int y);
int add(Adder adder);

#endif

  // === end add.hpp ===
int add(int x, int y)
{
    return x + y;
}

int add(Adder adder)
{
    return adder.x + adder.y;

}
// === end add.cpp ===
#include <iostream>
#include <vector>
#include <string.h>

int main()
{
    Adder a{10,2};
    std::cout << "Hello World: "  << add(10,2) << "\n";
    std::cout << "Hello World: "  << add(a) << "\n";

}
```

`<iostream>`, `<vector>`, and `<string.h>` remain as `#include` directives since they're standard library headers. Marker indentation reflects inclusion depth: `add.cpp` is top-level, `add.hpp` is one level in, `adder.hpp` is two.

Notice the guard handling differs between the two headers. `adder.hpp`'s guard wraps its entire file, so it's fully stripped. `add.hpp`'s guard is preceded by its own `#include "adder.hpp"` line, so the guard no longer brackets the whole file and is left intact — see [Limitations](#limitations).

---

## How It Works

1. Parse the template file line-by-line, looking for `#include` directives.
2. Resolve quoted includes against the source file's directory and any `-i` search paths.
3. Filter out standard library headers and angle-bracket includes (unless `-s` is passed).
4. Inline the resolved file, stripping `#pragma once` and include guard blocks first.
5. Track inlined files by canonical absolute path and by content hash to avoid duplicates.
6. Repeat recursively up to a depth of 100.

---

## Limitations

- **Naive parser** — The tool does a simple line-by-line scan. It does not fully understand C++ grammar, so `#include` lines inside string literals or comments may be incorrectly processed.
- **Guard stripping is positional** — Include guards are only recognized when the `#ifndef` / `#define` / `#endif` block wraps the entire file. If any other directive (such as another `#include`) precedes the guard, it's left intact rather than stripped.
- **No preprocessor** — Conditional compilation (`#ifdef`) is not evaluated. All `#include` lines visible in the source are processed regardless of surrounding `#if` blocks.
- **Template required** — You must specify a single entry-point file. It does not auto-discover files from a directory.
- **Macro includes** — Only simple macro-to-string mappings via `-d` are supported.

---

## License

Original code Copyright (c) 2012 Vinnie Falco.
Modifications Copyright (c) 2026 Dominic Arruda.
