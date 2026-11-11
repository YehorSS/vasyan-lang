# VasyanLang

A small programming language with Ukrainian keywords, written from scratch in C++17:
lexer → parser (AST) → bytecode compiler → bytecode validator → stack-based VM.

Мова програмування з українськими ключовими словами, написана з нуля на C++17.

```
го факторіал(н)
    якшо н <= 1
        верни 1
    кінець
    верни н * факторіал(н - 1)
кінець

хай оцінки = {"матем": 5, "фізика": 4}
кожен i у 0..10
    кажи факторіал(i)
кінець
```

## Features

- Functions, recursion, `if / else if / else`, `while`, `for` over ranges
- Lists and dictionaries, compound assignment (`+=`, `-=`, …), UTF-8 strings indexed by character
- Runtime errors carry the source line
- **Bytecode validator:** every function is checked before it runs — no unknown opcodes,
  no jumps into the middle of an instruction, no stack underflow, consistent stack depth on every path
- **Resource limits:** instruction count, time, call depth, stack and string size
- **Embeddable:** the host adds its own native functions; a script reaches nothing else

## Build

```
cmake -S . -B build
cmake --build build --config Release
```

## Run

```
vasyan exec examples/demo.vy
vasyan exec examples/data.vy
vasyan selftest
```

## Layout

| Path | What |
| --- | --- |
| `src/lexer.cpp`, `src/parser.cpp` | source → tokens → AST |
| `src/compiler.cpp` | AST → bytecode |
| `src/validator.cpp` | static checks of bytecode |
| `src/vm.cpp`, `src/value.cpp` | interpreter and values |
| `src/module.cpp` | module format (serialize / load) |
| `examples/` | sample programs |
