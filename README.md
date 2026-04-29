# ncc -- The non-C compiler

ncc is not a C compiler. It's a wrapper for existing C compilers,
making it easy to experiment with language extensions. Technically,
that makes it a preprocessor, but you still need your preprocessor...

Ncc works by first running your preprocessor in `-E` mode to get
pre-processed output, parsing, and then applying tree-based
transformations that you register. Finally, it sends the transformed
output back to your C compiler over stdin.

Most flags are proxied directly to your C compiler, unless they need
to be re-written or are specific to ncc. There's an unfortunate bit of
gymnastics to keep auto-dependency tracking sane.

NCC is written in C23 and embeds its grammar/templates with C23
`#embed`, using Clang's `--embed-dir` search path. Building ncc
therefore requires Clang 22.1.0 or newer; GCC 14 is not sufficient for
the compiler binary.

We provide a set of language extensions out of the box, focused mostly
on adding strong static typing capabilities through minimal language
changes. This includes bounds-checked string literals that support
rich text, and a type safe generics library.

But it's easy to add your own language extensions:

1. Specify the syntax you want in the bnf file provided.

2. Register a transform to fire either pre-order or post-order, on a
   particular non-terminal type.

3. Implement your transform function.

There's a templating capability at the C API level -- you can register
a template that is C code, except that it takes positional indicators
in the shell style (`$0`, `$1`, ...). At the same time, you name the
non-terminal in the grammar where the terminal should be parsed
from. We automatically do the text substitution (`$` params must be
the internal string type), and then parse starting in the grammar at
the specified non-terminal symbol to generate a parse subtree, so you
don't have to write pages of gross node building code, or write
scripts to generate pages of gross node building code.

## Getting started

Building requires Meson 1.1+, Ninja, and Clang 22.1.0 or newer. Put
the Clang 22.1.0+ `bin` directory ahead of older compiler
installations in `PATH` so Meson sees the Clang that supports both
`#embed` and `--embed-dir`.

```sh
LLVM_BIN=/path/to/clang-22.1.0-or-newer/bin
PATH="${LLVM_BIN}:${PATH}" CC=clang meson setup build
PATH="${LLVM_BIN}:${PATH}" meson compile -C build
PATH="${LLVM_BIN}:${PATH}" meson install -C build
```

You can then use `ncc` as if it were any other C compiler; generally
just `CC=ncc` and go.

Note that some of our language extensions need the supporting
runtime. Most of the type-safe data structures are header-only in
ncc_runtime.h.

For the rich string transformation, you'll need to link a small
runtime, libncc.a, which contains a small starter API.

You can redo the implementations of the pieces, change symbol names,
and so on via flags.

If you decide to do your own transforms, you'll need to register them;
for now, see the examples in `src/xform/`. I'm happy to incorporate
other good transforms into the default set if people want to share!

### Windows Cross-Build

`build-host` is the machine running Meson and `ncc` today. `target` is
the operating system for the produced compiler binary. For the Windows
flow below, the build host is Linux or macOS and the target is
`x86_64-w64-windows-gnu`. The local Windows cross-build uses the same
llvm-mingw Clang 22.1.0+ toolchain; keep its `bin` directory ahead of
older compiler installations in `PATH` for both setup and compile.
The cross file also expects `llvm-ar` and `llvm-strip` from that same
toolchain directory.

```sh
LLVM_MINGW=/path/to/llvm-mingw
PATH="${LLVM_MINGW}/bin:${PATH}" CC=clang meson setup build-win \
  --cross-file toolchains/windows-x86_64-clang.ini
PATH="${LLVM_MINGW}/bin:${PATH}" meson compile -C build-win
```

That produces `build-win/ncc.exe`. The cross file marks Windows
executables as non-runnable on the build host, so `meson test -C
build-win` is not part of the default validation loop. The supported
loop is:

```sh
LLVM_MINGW=/path/to/llvm-mingw
PATH="${LLVM_MINGW}/bin:${PATH}" CC=clang meson setup build-host
PATH="${LLVM_MINGW}/bin:${PATH}" meson compile -C build-host
PATH="${LLVM_MINGW}/bin:${PATH}" meson test -C build-host --print-errorlogs

PATH="${LLVM_MINGW}/bin:${PATH}" build-host/ncc --target=x86_64-w64-windows-gnu -o /tmp/ncc-win-bang.exe test/test_bang.c
PATH="${LLVM_MINGW}/bin:${PATH}" build-host/ncc --target=x86_64-w64-windows-gnu -o /tmp/ncc-win-option.exe test/test_option.c
PATH="${LLVM_MINGW}/bin:${PATH}" build-host/ncc --target=x86_64-w64-windows-gnu -o /tmp/ncc-win-constexpr.exe test/test_constexpr.c

scripts/package_windows_smoke.sh build-win /tmp/ncc-windows-smoke
```

Copy the resulting bundle to a Windows machine. From inside that
directory, run:

```powershell
$env:NCC_COMPILER='clang'; .\windows_smoke.ps1 -Ncc .\ncc.exe -Transcript .\windows-smoke-transcript.txt
```

If `clang.exe` is not on `PATH`, set `NCC_COMPILER` to its full path
instead. Cross-built Windows binaries default to invoking `clang` by
name unless you override that with `-Dcc_path=...`, `NCC_COMPILER`, or
`CC`. The smoke script enables `NCC_VERBOSE=1` by default unless you
override it and writes its transcript to
`windows-smoke-transcript.txt`; return that file after the run.

## NCC extensions

### Keyword Arguments (`_kargs`)

Our zero-cost keyword arguments provide named optional parameters,
with defaults, that are fully resolved statically. Currently, defaults
are mandatory. You are expected to specify parameter names and
defaults in a place that's visible to callers. The initializers for
variables must compile in the caller's context.

For instance, you can declare a `_kargs` function in a header like so:

```c
int create_widget(const char *name) _kargs {
    int   width  = 800;
    int   height = 600;
    char *title  = "Untitled";
};
```

Subsequent declarations can just declare `_kargs {}`, but if you provide any
names or defaults, they must completely match the previous declaration.

The definition site does look a bit unnatural right now. If naming the
keywords again for documentation you'd do:

```c
int create_widget(const char *name) _kargs {
    int   width  = 800;
    int   height = 600;
    char *title  = "Untitled";
}
{
  // Insert body here.
}
```
Calling this function with default values is simply:

```
create_widget("pane");
```

For supplying overrides, you use the same assignment syntax used in
compound literals:

```
create_widget("pane", .width = 1024, .title = "My App");
```

For function declarations and definitions, the transform involves
adding a parameter at the very end, of a pointer to a struct type that
is derived from the function name.

Currently, we insert code to unload the struct into local variables of
the same name. We could do better there, sure.

At the call site, we look up in the symbol table the parameter names
and stashed defaults. We then combine the defaults and provided
arguments (if any) into a compound literal of that struct type. We
don't bother deduplicating the defaults, because C23's struct
initialization semantics are well defined; we can always copy in all
the defaults, then the provided values. However, we do check to make
sure the caller doesn't specify the same argument multiple times. 

Note that, because we do add a shadow argument, we don't consider a
function with no fixed parameters to be lacking arguments. Therefore,
we don't do the work to accept `foo(void) _kargs {}`.

It's important to note that the struct is currently a stack temporary
passed by REFERENCE; that reference will not live beyond the life of
the called function.

#### Opaque keyword arguments

For generic or macro-dispatched APIs, you can use `_kargs: opaque` to
receive keyword arguments as `void *kargs`, without generating a
struct on the declaration side. For instance, if you wanted to have an
interface to alloc memory and call an initializer, proxying `_kargs`,
you could declare:

```c
void *my_new() _kargs: opaque;
```

The call site DOES need to know the initializer function to be able to
generate the struct. We have a transform to create a keyword argument
from the struct, called `kw_func()`:

```c
dict_t *d = my_new(kw_func(n00b_dict_init, .starting_buckets = 64));
```

The keyword proxy is definitely a lot more kludgy; if I were to
propose keyword parameters to the C standards committee, it'll
definitely be without this feature. I use it, but I don't think it's
necessary, or elegent enough.

### Variadic Parameters (`+`)

This is a full implementation of my macro-based prototype for modern
vargs for C. Modern in the sense that arguments are automatically
passed through a compound literal that includes an explicit type
field.

To provide compatability with traditional C-style `va_list` variant
argument lists, we use a '+' in the last explicit parameter slot to
denote vargs (by default, the type is `ncc_varargs_t *`).

The transformation rewrites the `+` to be the expected struct. There
are accessor macros (in `include/ncc_runtime.h`), but behind the
scenes, we simply declare a parameter named `vargs`.

If there are also keyword arguments, keyword arguments will be placed
after the vargs.

At any call site, we first process keyword arguments (which can
actually appear in any position). Then, we look at the remaining
positional arguments, and group anything beyond the number of fixed
declared arguments, and create a compound literal on the stack.

Unlike my macro prototype, this handles arguments of arbitrary
sizes. Large items (larger than `sizeof(void *)`) will be passed by
reference.

#### Type safety option

You can specify a type in front of the `+`. If you do, then we type
check the call site.

Note, though, that the accessor in the example API returns a void
*. So for integer types of 64-bits or less, this currently DOES need a
cast. And for larger types, you are responsible for the dereference,
even though at the call site it is transparent.

This is pure laziness-- for typed values, at least. We could
automatically deal with these scenarios at zero cost in typed
cases. For untyped cases, the implicit ref on the call side cannot
reliably be matched on the deference side at zero cost (i.e., without
always passing and checking info in the generated code).

I think that mismatch probably isn't acceptable. For that reason, when
/ if I propose this to the standards committee, I'll probably propose one of
two options:


1. Arguments larger than `sizeof(void *)` cannot be passed at all.
2. Adding a typed interface only; declare the type to be `void *` if
   you want to forego the safety.

Other options don't feel tenable to me. But I'm still taking feedback.

Here's a current example with the provided macros:
   
```c
long long int
sum(int +) {
    long long int total = 0;
    
    while (rest->cur_ix < rest->nargs) {
        total += (int)ncc_vargs_next();
    }
    
    return total;
}

sum(1, 2, 3, 4, 5);
```

### Option types

This provides a statically typed 'option' interface:


```
_option(char *)
my_get_env(const char *name)
{
   char *val = getenv(name);
   if (!val) {
       return _none(char *);
   }

   return _some(char *, val);
}
```
The transformation is handled differently depending on whether the
type the option encapsulates is a pointer type or not. For non-pointer
types we expand `_option(T)` to `struct { bool has_value; T value;
}`. For pointer types, we leave the pointer representation; a
`nullptr` tells us there is no value. 

**Constructors and accessors:**

| Syntax | Meaning |
|--------|---------|
| `_some(T, val)` | Construct an option with a value |
| `_none(T)` | Construct an empty option |
| `_is_some(x)` | Test if the option has a value |
| `_is_none(x)` | Test if the option is empty |
| `_unwrap(x)` | Extract the value |


### Type Reflection operators (`typeid`, et al)

Inspired by Martin Uecker's work on type safety on top of `_Generic`,
`typeof()` and GCC expression statements, I wanted to experiment with
general purposes way to get better type safety into the language, with
as minimal a number of languages changes as I could manage.

While Martin's work was great, there are a number of limitations I
thought was a usability problem. For instance, it heavily relies on
converting type names to ID parts via the preprocessor, meaning
everything basically needs to be explicitly type-def'd, even when
making heavy use of temporary structs. And the approach wasn't
satisfying for things like generics.

The set to handle all the things I wanted to handle is currently
larger than I wanted. And I do think I can pare it down a bit. But,
what ncc currently ships:

- `typeid()`, which produces single tokens from any positive number of
  arguments. If the argument is a string literal, that literal is used
  in the token name. Otherwise, it must be a type, in which case,
  after some normalization, we SHA-256 hash, and then encode a subset
  of the bits into an identifier. This is used to name temporary
  structs, so that they compare by name.

- `_generic_struct` -- unfortunately, C23's current type matching
  rules for tagged structs will really only let us compare struct
  definitions properly for two separate variables if there's an
  explicit declaration of the struct somewhere. So `_generic_struct`
  works the same as `struct`, except that it waives that rule. In ncc,
  that is implemented by automatically declaring the struct when we
  first see `_generic_struct` with a given tag in the compilation
  unit. In all cases, we rewrite `_generic_struct` to `struct`.
  
- `typehash()`, which I use for runtime type checking, where
  necessary. For example, there's a provided example of a statically
  typed tagged union (`ncc_variant_t()`). The tag is derived from the
  type via `typehash()`, which starts off with the same normalization
  and hashing as `typeid()`, but then extracts just 64 bits as an
  `unsigned int` for the tag.

- `constexpr_paste()` Unfortunately, there are times where our
   constructs don't seem to be quite enough to get us a tagged union,
   because we need to construct struct field names from their
   results. Unfortunately, since we run after the C preprocessor, we
   cannot use token pasting. This construct wouldn't be necessary if
   other constructs were part of the language.

- `typestr()` Much like for `constexpr_paste()`, this is something
   that'd typically be done w/ a macro, but I do turn types into
   strings a lot for debugging, and thought, since I was at it, might
   as well get a nicer interface.

- `constexpr_eval()` To get the equivolent of a typed Python tuple
   (anonymous fields available by index, but still statically typed),
   I could generate index info via macro, but not in a way that would
   be composable into an identifier. For instance, at definition time,
   you can use common techniques to "map" each field in the definition
   to a slot, but to get the index, it's severe torture to map it to a
   constant. It's easier to map it to an expression like `(1 + 1 + 1)`
   (though still quite ugly).

   For `constexpr_eval()`, I take a real hacky approach-- I compile a
   small program w/ standard headers, and allow you to provide
   additional headers at compile time if necessary. This is currently
   limited to producing `int` values. There's also a `constexpr_min()`
   and `constexpr_max()` that follow the same path.

   These three constructs are not critical for the strongly typed data
   structures I'm using. Even the Pythonic tuple is not a valid use
   case for this-- just use a struct.

   Still, compile-time execution that is less limiting than macros is
   a problem for C in my opinion, so I'm leaving these things around,
   and giving myself room to explore.

Some examples:

```c
int size = constexpr_eval(sizeof(struct my_big_struct));
int max  = constexpr_max(sizeof(A), sizeof(B), sizeof(C));
int min  = constexpr_min(4, 8);
int cmp  = constexpr_strcmp("abc", "def");
int len  = constexpr_strlen("hello world");
```

Each `constexpr_*` call compiles and runs a tiny C program that prints
the result, which is then substituted as an integer literal.

Use `--ncc-constexpr-include` to provide additional headers that the
helper program needs:

```sh
ncc --ncc-constexpr-include '<my_types.h>,"local.h"' -c file.c
```

For all of these type transformations, I have *not* done the work to
make them work with `typeof()` or `typeof_unqual()`. You basically
need to do a lot more work than I was willing to do for that to
work. Similarly, `auto` is not handled.

### Error Propagation (`!`)

Since I built myself a `Result` type (in the example code, the error
option is always an int), I needed some sugar to be able to automate
destructuring it like Rust's `?` operaator-- if the value is an error,
automatically propogate the error. Otherwise, automatically unpack it.

While a `?` can't be used for this purpose, as it would be ambiguous
w/ the ternary operator, I've found a postfix `!` operator is just as
clear.

### Single-Execution Functions (`once`)

One provides thread-safe functions that execute their body at most...
once.

```c
once void
init_subsystem(void) {
    // Runs exactly once, even from multiple threads.
    open_database();
    load_config();
}
```

For non-void functions, the return value is cached:

```c
once int get_cpu_count(void) {
    return sysconf(_SC_NPROCESSORS_ONLN);
}
```

Generated wrappers use compiler `__atomic` builtins for lock-free
synchronization.

This is only transformed / used during definitions. If we see `once`
as a keyword in a declaration, we currently siliently erase it (we
don't currently check because, again, laziness).

Outside of definitions and declarations, `once` is treated like an
identifier. Everything above isn't; they're all keywords.

### Rich String Literals (`r""`)

Pre-compiled string literals with inline styling markup. We provide a
default data structure and default code to pre-parse the literals, but
the formatting and rendering code is not currently provided.

```c
#include <ncc/string.h>

ncc_string_t *msg   = r"Hello {bold:world}!";
ncc_string_t *err   = r"<red>Error:</red> something broke";
ncc_string_t *plain = r"No markup, just a string";
```

The `r""` prefix triggers compile-time parsing of markup tags. The
transform emits a static compound literal with pre-computed styling
data, so there is zero runtime parsing cost.


## Customization

ncc is designed to be embedded into other projects as their C compiler.
All generated names and code templates are configurable, so the
extensions integrate cleanly with your project's type system.

### Meson Build Options

Pass these with `meson setup -Doption=value`:

| Option | Default | Purpose |
|--------|---------|---------|
| `cc_path` | (Meson compiler path, or `clang` for Windows cross-builds) | Path to the underlying C23 compiler |
| `vargs_type` | `ncc_vargs_t` | Struct type name for variadic parameters |
| `once_prefix` | `__ncc_` | Identifier prefix for `once` guard variables |
| `rstr_string_type` | `ncc_string_t*` | Type name used in `typehash()` for rich strings |
| `rstr_template_styled` | (built-in) | Code template for styled `r""` literals |
| `rstr_template_plain` | (built-in) | Code template for plain `r""` literals |
| `coverage` | `false` | Enable clang source-based code coverage |

These same options can be overridden per-invocation via CLI flags (see
below).

### CLI Flags

All ncc-specific flags use the `--ncc-` prefix and are stripped before
arguments reach clang.

| Flag | Purpose |
|------|---------|
| `--no-ncc` | Disable all transforms (passthrough mode) |
| `--ncc-vargs-type=TYPE` | Override vargs struct type name |
| `--ncc-once-prefix=PREFIX` | Override once-guard prefix |
| `--ncc-rstr-string-type=TYPE` | Override rstr string type for typehash |
| `--ncc-rstr-template-styled=TMPL` | Override styled rstr template |
| `--ncc-rstr-template-plain=TMPL` | Override plain rstr template |
| `--ncc-constexpr-include=HDRS` | Headers for constexpr eval programs |
| `--ncc-dump-tokens` | Dump token stream to stderr |
| `--ncc-dump-tree` | Dump parse tree to stderr |
| `--ncc-dump-tree-raw` | Dump parse tree with group nodes visible |
| `--ncc-dump-output` | Dump transformed C to stderr |
| `--ncc-help` | Show ncc help |

Priority order: CLI flag > meson build-time define > compiled default.

### Rich String Templates

The `r""` transform generates a static compound literal for each string.
The template controls the exact shape of that literal, which lets you
wrap strings in your project's object headers (e.g., for GC integration).

Templates use `$N` positional slots:

**Styled template slots:**
| Slot | Content |
|------|---------|
| `$0` | Style declaration block |
| `$1` | Variable name |
| `$2` | Byte count (`u8_bytes`) |
| `$3` | String data pointer |
| `$4` | Codepoint count |
| `$5` | Styling data pointer |

**Plain template slots:**
| Slot | Content |
|------|---------|
| `$0` | Variable name |
| `$1` | Byte count (`u8_bytes`) |
| `$2` | String data pointer |
| `$3` | Codepoint count |

**Default templates** (built into ncc):
```c
// Styled:
({$0 static ncc_string_t $1 = {
    .u8_bytes   = $2,
    .data       = $3,
    .codepoints = $4,
    .styling    = $5
}; &$1;})

// Plain:
({static ncc_string_t $0 = {
    .u8_bytes   = $1,
    .data       = $2,
    .codepoints = $3,
    .styling    = ((void*)0)
}; &$0;})
```

**Example: wrapping strings for a GC**:
```sh
meson setup build \
  -Drstr_string_type='my_string_t*' \
  -Dvargs_type='my_vargs_t' \
  -Donce_prefix='__my_' \
  '-Drstr_template_styled=({$0 static struct{gc_header_t hdr; my_string_t obj;} $6={.hdr={.magic=0xdeadbeef,.type=$5,.len=sizeof(my_string_t)},.obj={.u8_bytes=$2,.data=$3,.codepoints=$4,.styling=$5}};&$6.obj;})' \
  '-Drstr_template_plain=({static struct{gc_header_t hdr; my_string_t obj;} $5={.hdr={.magic=0xdeadbeef,.type=$4,.len=sizeof(my_string_t)},.obj={.u8_bytes=$1,.data=$2,.codepoints=$3,.styling=((void*)0)}};&$5.obj;})'
```

When overriding templates, additional `$N` slots beyond the defaults are
available for the wrapper struct's own fields (e.g., `$5`/`$6`/`$7` for
styled, `$4`/`$5` for plain).

## Environment Variables

| Variable | Purpose |
|----------|---------|
| `NCC_COMPILER` | Override the underlying compiler at runtime |
| `CC` | Fallback compiler (if `NCC_COMPILER` is unset) |
| `NCC_VERBOSE` | Enable verbose progress messages |
| `NCC_CONSTEXPR_HEADERS` | Default headers for `constexpr_*` evaluation |

## Architecture

Here's what happens in a nutshell:

1. **Preprocess** -- Run the source through `clang -E`
2. **Tokenize** -- Lex the preprocessed C with ncc's extended grammar
3. **Prescan** -- While we tokenize, we transform `r"..."` literals (by default, to `__ncc_rstr("...")`).
4. **Parse** -- Build a parse tree using the PWZ algorithm.
5. **Transform** -- Apply all registered transforms in walk-order. We continue walking in-place, without regard to whether nodes are synthetic or not.
6. **Emit** -- Emit the transformed tree back into standard C
7. **Compile** -- Pipe the result to clang

PWZ is "parsing with zippers", a more efficient variant of "parsing
with derivatives". It handles any context-free grammar, whether or not
there is ambiguity.

We have a grammar abstraction that PWZ operates on, that can hold any
context-free grammar.

We construct a grammar object manually for BNF parsing, and then use
that to generate the grammar we use to then parse C files.

## The parsing engine

The PWZ algorithm does come with a bit of overhead vs a hand-crafted
parser, especially given C's ambiguity.  The memoization we do is not
memory-optimized. The approach is a bit slower than just a direct
compile, but it gives us the flexibilty to quickly iterate on
extensions.

The grammar (`c_ncc.bnf`) contains the grammar extensions mentioned
above. But it also contains many clang and GCC extensions-- I added
everything I ran across when trying to get real programs to parse
system headers with those compilers on Linux and Mac. So it does cover
GCC statement expressions and Apple block pointers.

It does *not* include things I didn't experience in the process. There
is not currently nested function support, nor will there be support
for most Microsoft extensions.

The engine is a general-purpose parser, capable of parsing any context
free programming language. You'll get a parse tree on successful
parse, or if the language allows ambiguity, you may get multiple parse
trees.

The accepted syntax would be more commonly recognized as EBNF, as we
do accept `?`, `+` and `*` operators, that work on either a single
item (terminal or non-terminal) or grouped items, via parentheses.

We've been using the engine internally for a couple of years
now. Originally, it was based on the Earley algorithm, which makes it
easier to provide good error handling, without having to add error
productions (which can be problematic in grammars with nullable rules,
which people naturally tend to write).

With full C, every file's parse includes parsing every header
transitively included. That's let to a couple of cases where Earley's
performance was not acceptable enough for general purpose use, even
after exhausting all the common optimizations.

I would like to expressly call out John Aycock-- maybe 18 months ago, I
did reach out about to him about his 2001 paper, **Directly-Executable
Earley Parsing**. He managed to dust off implementations of the two
algorithms in the paper, DEEP and SHALLOW.

In the time since he sent them, I've converted them to 64-bit code,
and gotten them running. DEEP remains impractical due to code
size. But, SHALLOW seems to work particularly well in practice on
64-bit architectures.

But, I still need to do a lot of work to integrate it with our
framework, only a little of which is part of NCC. Internally, we
currently use PWZ, and if a parse fails, we go to Earley to re-parse,
in order to make it easier to do good error analysis (potentially
adding error productions for only a subset of the grammar, applied
only at the point where a top-level item doesn't complete).

I think there's a good chance when I finally get around to fully
integrating John's work, it'll be very performant, and we'll go back
to Earley only.

Generally, I prefer Earley and PWZ both to PEGs, because the fact that
they produce parse forests gives people tools to understand ambiguity
when developing or working with a grammar. And if you have to write a
parser for a language with inherant ambiguities, like C, it's far, far
easier to do with such an algorithm, as you can see here.

It took very little time to extract the full grammar from the C
standard, and get a parser working-- capturing common extensions we
ran into properly in the grammar was more work (and still not hard at
all; just a matter of figuring out where in the grammar extensions
actually sit, in practice).

Sure, you still need to handle some of those ambiguities to select the
right parse tree to do a full compile. But that isn't always
important. For instance, here, we do need to track typedefs, but we
don't really need to care which tree our parser selects when there's
an if-else ambiguity-- modulo our transformations, we are simply going
to emit the same C code we received (and our transformations wouldn't
affect ambiguous sites).

Also, my opinion of PEG is colored by the fact that most PEG notations
are hard to read and write, optimized for compactness over
communication.

I did implement a declarative version of the C parser using the
packrat algorithm (recursive descent with memoization, which is
typically how PEG is implemented). The typedef ambiguity aside,
tweaking the ordering of rules in the grammar was quite a lot of trial
and error.

While the Packrat approach yields a nice upper bound worse case, our
experiences so far, using all three algorithms to parse C, is that PWZ
is currently better all around. Without further optimization, Earley
wasn't quite good enough. Packrat was better than Earley on many
inputs, but on some inputs with MANY tokens, especially ones with
long, sequential tables. For instance, in a Unicode implementation,
there are typically tables with nearly 50,000 elements; the extra
overhead of the memos dwarfs the savings; in such files there are 0
cache hits, and lots of wasted work. 

Perhaps I could optimize my packrat implementation more, just as I've
done on my Earley implementation. However, PWZ has performed
reasonably well since I implemented it, without any real thought to
optimization as of yet.
