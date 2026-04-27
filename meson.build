project('ncc', 'c',
  version : '0.1.0',
  meson_version : '>= 1.1',
  default_options : ['c_std=gnu23', 'warning_level=3', 'b_lundef=true'])

cc = meson.get_compiler('c')

if not cc.compiles('int main(void) { return 0; }', args: ['-std=c23'],
                    name: 'C23 support check')
  error('Compiler does not support -std=c23. Use clang 22+ (/usr/local/bin/clang).')
endif

add_project_arguments(
  cc.get_supported_arguments(
    '-Wno-gnu-statement-expression-from-macro-expansion',
  ),
  language : 'c',
)

# Check for glibc (needed on Linux)
glibc_check = '''
#include <stdio.h>
#ifndef __GLIBC__
#error NO GLIBC
#endif
'''
using_glibc = cc.compiles(glibc_check, name : 'glibc check')

# Default compiler path for NCC_DEFAULT_CC
default_compiler_path = get_option('cc_path')
if default_compiler_path == ''
  if host_machine.system() == 'windows'
    default_compiler_path = 'clang'
  else
    default_compiler_path = cc.cmd_array()[0]
  endif
endif

c_args = [
  '-DNCC_DEFAULT_CC="' + default_compiler_path + '"',
]

if using_glibc
  c_args += ['-D_GNU_SOURCE']
endif

if host_machine.cpu_family() == 'x86_64' and cc.get_id() == 'clang'
  c_args += ['-Wno-atomic-alignment']
endif

rstr_styled = get_option('rstr_template_styled')
if rstr_styled != ''
  c_args += ['-DNCC_RSTR_TEMPLATE_STYLED="' + rstr_styled + '"']
endif
rstr_plain = get_option('rstr_template_plain')
if rstr_plain != ''
  c_args += ['-DNCC_RSTR_TEMPLATE_PLAIN="' + rstr_plain + '"']
endif
vargs_type_opt = get_option('vargs_type')
if vargs_type_opt != ''
  c_args += ['-DNCC_VARGS_TYPE="' + vargs_type_opt + '"']
endif
once_prefix_opt = get_option('once_prefix')
if once_prefix_opt != ''
  c_args += ['-DNCC_ONCE_PREFIX="' + once_prefix_opt + '"']
endif
rstr_string_type_opt = get_option('rstr_string_type')
if rstr_string_type_opt != ''
  c_args += ['-DNCC_RSTR_STRING_TYPE="' + rstr_string_type_opt + '"']
endif

# Memory debug tracking
mem_debug_enabled = get_option('mem_debug')
if mem_debug_enabled
  c_args += ['-DNCC_MEM_DEBUG']
endif

# Clang source-based code coverage
coverage_enabled = get_option('coverage')
coverage_args = []
coverage_link_args = []
if coverage_enabled
  if cc.get_id() != 'clang'
    error('Coverage requires Clang compiler')
  endif
  coverage_args = ['-fprofile-instr-generate', '-fcoverage-mapping']
  coverage_link_args = ['-fprofile-instr-generate']
endif

inc = include_directories('include')

# ── Library sources ──────────────────────────────────────────────────────────

src_lib = files(
  'src/lib/string.c',
  'src/lib/alloc.c',
)

src_parse = files(
  'src/parse/pwz.c',
  'src/parse/grammar.c',
  'src/parse/parse_tree.c',
  'src/parse/parse_forest.c',
  'src/parse/unicode_class.c',
  'src/parse/token.c',
  'src/parse/bnf.c',
  'src/parse/tree_dump.c',
  'src/parse/typedef_walk.c',
  'src/parse/symtab.c',
  'src/parse/c_tokenizer.c',
  'src/parse/emit.c',
  'src/parse/commander.c',
)

src_util = files(
  'src/util/platform.c',
  'src/util/sha256.c',
  'src/util/type_normalize.c',
)

src_xform = files(
  'src/xform/transform.c',
  'src/xform/xform_helpers.c',
  'src/xform/xform_bang.c',
  'src/xform/xform_constexpr.c',
  'src/xform/xform_constexpr_paste.c',
  'src/xform/xform_generic_struct.c',
  'src/xform/xform_kargs_vargs.c',
  'src/xform/xform_once.c',
  'src/xform/xform_option.c',
  'src/xform/xform_rstr.c',
  'src/xform/xform_typeid.c',
  'src/xform/xform_typestr.c',
  'src/xform/xform_typehash.c',
  'src/xform/xform_template.c',
)

src_scanner = files(
  'src/scanner/token_stream.c',
  'src/scanner/scanner.c',
  'src/scanner/scan_builtins.c',
)

# ── User-facing runtime library ───────────────────────────────────────────────
#
# Provides a small rich string API for code compiled by ncc.  Other
# runtime support is header-only via ncc_runtime.h.


libncc = static_library('ncc',
  src_lib,
  include_directories : inc,
  c_args : c_args + coverage_args,
  link_args : coverage_link_args,
)

libncc_dep = declare_dependency(
  link_with : libncc,
  include_directories : inc,
)

# ── Internal static library (compiler internals — not user-facing) ────────────

libncc_internal = static_library('ncc_internal',
  src_parse + src_xform + src_scanner + src_util,
  include_directories : inc,
  c_args : c_args + coverage_args,
  link_args : coverage_link_args,
  link_with : libncc,
)

libncc_internal_dep = declare_dependency(
  link_with : [libncc_internal, libncc],
  include_directories : inc,
)

# ── NCC executable ───────────────────────────────────────────────────────────

ncc_exe = executable('ncc',
  'src/ncc.c',
  c_args : c_args + coverage_args + ['--embed-dir=' + meson.current_source_dir()],
  link_args : coverage_link_args,
  dependencies : libncc_internal_dep,
  install : true,
)

# ── Smoke tests ──────────────────────────────────────────────────────────────

test_runner = find_program('test/run_test.sh')
test_dir    = meson.current_source_dir() / 'test'

# Library-level tests (link against libncc directly)
test_pwz = executable('test_pwz',
  'test/test_pwz.c',
  c_args : c_args + coverage_args,
  link_args : coverage_link_args,
  dependencies : libncc_internal_dep,
)
test('pwz_smoke', test_pwz, suite : 'ncc')

test_bnf = executable('test_bnf',
  'test/test_bnf.c',
  c_args : c_args + coverage_args,
  link_args : coverage_link_args,
  dependencies : libncc_internal_dep,
)
test('bnf_c_parse', test_bnf, suite : 'ncc')

test_xform_template = executable('test_xform_template',
  'test/test_xform_template.c',
  c_args : c_args + coverage_args,
  link_args : coverage_link_args,
  dependencies : libncc_internal_dep,
)
test('xform_template', test_xform_template, suite : 'ncc')

test_c_tokenizer = executable('test_c_tokenizer',
  'test/test_c_tokenizer.c',
  c_args : c_args + coverage_args,
  link_args : coverage_link_args,
  dependencies : libncc_internal_dep,
)
test('c_tokenizer', test_c_tokenizer, suite : 'ncc')

test_process_run = executable('test_process_run',
  'test/test_process_run.c',
  c_args : c_args + coverage_args,
  link_args : coverage_link_args,
  dependencies : libncc_internal_dep,
)
test('process_run', test_process_run, suite : 'ncc', timeout : 10)

test_fake_binary_compiler = executable('test_fake_binary_compiler',
  'test/fake_binary_compiler.c',
  c_args : c_args + coverage_args,
  link_args : coverage_link_args,
)

test_fake_verbose_compiler = executable('test_fake_verbose_compiler',
  'test/fake_verbose_compiler.c',
  c_args : c_args + coverage_args,
  link_args : coverage_link_args,
)

test_fake_stdin_close_compiler = executable('test_fake_stdin_close_compiler',
  'test/fake_stdin_close_compiler.c',
  c_args : c_args + coverage_args,
  link_args : coverage_link_args,
)

# Default ncc type settings for tests.  When the ncc binary is built with
# overridden type names (e.g. n00b_vargs_t for the n00b project), the test
# files still use the default ncc_* names.  Pass CLI overrides so the
# transforms emit the names the test files expect.
ncc_include_dir = meson.current_source_dir() / 'include'

ncc_test_flags = [
  '--ncc-vargs-type=ncc_vargs_t',
  '--ncc-once-prefix=__ncc_',
  '--ncc-rstr-string-type=ncc_string_t*',
  '--ncc-rstr-template-styled=({$0 static ncc_string_t $1={.u8_bytes=$2,.data=$3,.codepoints=$4,.styling=$5};&$1;})',
  '--ncc-rstr-template-plain=({static ncc_string_t $0={.u8_bytes=$1,.data=$2,.codepoints=$3,.styling=((void*)0)};&$0;})',
  '-I' + ncc_include_dir,
]

# Compiler-level tests (run through ncc executable)
ncc_compile_tests = {
  'bang':          ['compile_run',  test_dir / 'test_bang.c'],
  'constexpr':    ['compile_run',  test_dir / 'test_constexpr.c'],
  'kargs':        ['compile_run',  test_dir / 'test_kargs.c'],
  'kargs_vargs':  ['compile_run',  test_dir / 'test_kargs_vargs.c'],
  'rstr':         ['compile_run',  test_dir / 'test_rstr.c'],
  'vargs':        ['compile_run',  test_dir / 'test_vargs.c'],
  'once':         ['compile_run',  test_dir / 'test_once.c'],
  'option':       ['compile_run',  test_dir / 'test_option.c'],
  'generic_struct': ['compile_run', test_dir / 'test_generic_struct.c'],
  'gs_result':      ['compile_run', test_dir / 'test_gs_result.c'],
}

foreach name, params : ncc_compile_tests
  test('ncc_' + name, test_runner,
    args : [ncc_exe, params[0], params[1]] + ncc_test_flags,
    suite : 'ncc',
    depends : ncc_exe,
  )
endforeach

if host_machine.system() != 'windows' and cc.has_header('pthread.h')
  test('ncc_constexpr_pthread', test_runner,
    args : [ncc_exe, 'compile_run', test_dir / 'test_constexpr_pthread.c']
           + ncc_test_flags,
    suite : 'ncc',
    depends : ncc_exe,
  )
endif

# rstr_ansi test links against libncc (ncc_string_to_ansi lives there).
test('ncc_rstr_ansi', test_runner,
  args : [ncc_exe, 'compile_run', test_dir / 'test_rstr_ansi.c']
         + ncc_test_flags + [libncc.full_path()],
  suite : 'ncc',
  depends : [ncc_exe, libncc],
)

test('ncc_verbose_binary_output', test_runner,
  args : [ncc_exe, 'verbose_binary_output', test_dir / 'test_bang.c'],
  env : {'NCC_COMPILER' : test_fake_binary_compiler.full_path()},
  suite : 'ncc',
  depends : [ncc_exe, test_fake_binary_compiler],
)

test('ncc_cc_fallback_binary_output', test_runner,
  args : [ncc_exe, 'cc_fallback_binary_output', test_dir / 'test_bang.c'],
  env : {'NCC_TEST_CC' : test_fake_binary_compiler.full_path()},
  suite : 'ncc',
  depends : [ncc_exe, test_fake_binary_compiler],
)

test('ncc_cc_self_guard', test_runner,
  args : [ncc_exe, 'cc_self_guard', test_dir / 'test_bang.c']
         + ncc_test_flags,
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_verbose_high_output', test_runner,
  args : [ncc_exe, 'verbose_high_output', test_dir / 'test_bang.c']
         + ncc_test_flags,
  env : {'NCC_COMPILER' : test_fake_verbose_compiler.full_path()},
  suite : 'ncc',
  depends : [ncc_exe, test_fake_verbose_compiler],
  timeout : 10,
)

test('ncc_compiler_stdin_close', test_runner,
  args : [ncc_exe, 'compiler_stdin_close', test_dir / 'test_bang.c']
         + ncc_test_flags,
  env : {'NCC_COMPILER' : test_fake_stdin_close_compiler.full_path()},
  suite : 'ncc',
  depends : [ncc_exe, test_fake_stdin_close_compiler],
  timeout : 10,
)

test('ncc_depfile', test_runner,
  args : [ncc_exe, 'depfile', test_dir / 'test_depfile.c'] + ncc_test_flags,
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_depfile_md', test_runner,
  args : [ncc_exe, 'depfile_md', test_dir / 'test_depfile.c']
         + ncc_test_flags,
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_depfile_default', test_runner,
  args : [ncc_exe, 'depfile_default', test_dir / 'test_depfile.c']
         + ncc_test_flags,
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_depfile_mt', test_runner,
  args : [ncc_exe, 'depfile_mt', test_dir / 'test_depfile.c']
         + ncc_test_flags,
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_depfile_mq', test_runner,
  args : [ncc_exe, 'depfile_mq', test_dir / 'test_depfile.c']
         + ncc_test_flags,
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_depfile_error', test_runner,
  args : [ncc_exe, 'depfile_expect_error', test_dir / 'test_depfile.c']
         + ncc_test_flags,
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_constexpr_runtime_error', test_runner,
  args : [ncc_exe, 'constexpr_runtime_error',
          test_dir / 'err_constexpr_runtime_fail.c'] + ncc_test_flags,
  suite : 'ncc',
  depends : ncc_exe,
)

# Error tests (expect non-zero exit)
ncc_error_tests = {
  'err_bang_at_file_scope':       test_dir / 'err_bang_at_file_scope.c',
  'err_bang_missing_parens':      test_dir / 'err_bang_missing_parens.c',
  'err_bang_void_fn':             test_dir / 'err_bang_void_fn.c',
  'err_constexpr_bad_expr':      test_dir / 'err_constexpr_bad_expr.c',
  'err_constexpr_min_one_arg':   test_dir / 'err_constexpr_min_one_arg.c',
  'err_constexpr_paste_bad_frag': test_dir / 'err_constexpr_paste_bad_frag.c',
  'err_constexpr_wrong_args':    test_dir / 'err_constexpr_wrong_args.c',
  'err_missing_semicolon':       test_dir / 'err_missing_semicolon.c',
  'err_once_non_function':       test_dir / 'err_once_non_function.c',
  'err_rstr_bad_tag':            test_dir / 'err_rstr_bad_tag.c',
  'err_typeid_no_args':          test_dir / 'err_typeid_no_args.c',
}

foreach name, src : ncc_error_tests
  test('ncc_' + name, test_runner,
    args : [ncc_exe, 'expect_error', src],
    suite : 'ncc',
    depends : ncc_exe,
  )
endforeach
