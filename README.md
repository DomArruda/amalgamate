Amalgamate
A single-file C++ source amalgamator. It recursively inlines #include directives to produce a self-contained translation unit, while leaving standard library headers untouched.
Originally based on Vinnie Falco's Amalgamate, rewritten for C++17 with std::filesystem, content-hash deduplication, and cleaner output.
Features
Recursive inlining — Transitive #include "..." directives are flattened into a single file
Standard library detection — <iostream>, <vector>, <string.h>, etc. are left as-is
Include guard stripping — #ifndef / #define / #endif guard blocks are removed from inlined headers to reduce noise
Content-hash deduplication — Identical files reached via different paths are inlined only once
Recursion limit — Hard cap at 100 levels to prevent runaway expansion
Clean markers — Inlined sections are marked with compact // === filename === comments
Cross-platform — Uses std::filesystem, works on Linux, macOS, and Windows (WSL/MSYS2)
Build
Requires a C++17 compiler and a standard library with std::filesystem support.
bash
# GCC
g++ -std=c++17 -O2 -pthread -o amalgamate amalgamate.cpp

# Clang
clang++ -std=c++17 -O2 -pthread -o amalgamate amalgamate.cpp
Usage
bash
./amalgamate [options] <inputFile> <outputFile>
Options
Table
Option	Description
-s	Inline system headers (angle brackets). Disabled by default.
-t	Convert leading spaces to tabs
-v	Verbose output
-w <wildcards>	Comma-separated list of file patterns to inline (default: *.cpp;*.c;*.hpp;*.h)
-f <file>	Force reinclusion of a specific file or macro
-p <file>	Prevent reinclusion of a specific file or macro
-d <name>=<file>	Define a macro-to-file mapping for macro-based #include lines
-i <dir>	Additional search directory for resolving #include paths
-h, --help	Print usage and exit
Example
Given a project:
plain
src/
├── main.cpp
├── math.hpp
└── math.cpp
main.cpp
cpp
#include "math.hpp"
#include <iostream>

int main() {
    std::cout << add(2, 3) << "\n";
}
math.hpp
cpp
#ifndef MATH_HPP
#define MATH_HPP
int add(int a, int b);
#endif
math.cpp
cpp
#include "math.hpp"
int add(int a, int b) { return a + b; }
Run:
bash
./amalgamate src/main.cpp out.cpp
out.cpp
cpp
// === math.hpp ===
int add(int a, int b);
// === end math.hpp ===
// === math.cpp ===
int add(int a, int b) { return a + b; }
// === end math.cpp ===
#include <iostream>

int main() {
    std::cout << add(2, 3) << "
";
}
Note that <iostream> remains an #include because it is a standard library header, and the #ifndef guard around math.hpp has been stripped.
How it works
Parse the template file line-by-line, looking for #include directives.
Resolve quoted includes against the source file's directory and any -i search paths.
Filter out standard library headers and angle-bracket includes (unless -s is passed).
Inline the resolved file, stripping #pragma once and include guard blocks first.
Track inlined files by canonical absolute path and by content hash to avoid duplicates.
Repeat recursively up to a depth of 100.
Limitations
Naive parser — The tool does a simple line-by-line scan. It does not fully understand C++ grammar, so #include lines inside string literals or comments may be incorrectly processed.
No preprocessor — Conditional compilation (#ifdef) is not evaluated. All #include lines visible in the source are processed regardless of surrounding #if blocks.
Template required — You must specify a single entry-point file. It does not auto-discover files from a directory.
Macro includes — Only simple macro-to-string mappings via -d are supported.
License
Original code Copyright (c) 2012 Vinnie Falco.
Modifications Copyright (c) 2026 Dominic Arruda.