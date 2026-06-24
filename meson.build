project('ncc', 'c',
  version : '0.1.0',
  meson_version : '>= 1.1',
  default_options : ['c_std=gnu23', 'warning_level=3', 'b_lundef=true'])

cc = meson.get_compiler('c')

# ncc embeds its grammar assets via C23 #embed + Clang --embed-dir. Rather than
# gate on a clang VERSION number (which under-counts compilers that already have
# the feature), require clang and let the functional #embed compile check below
# be the authoritative capability gate.
if cc.get_id() != 'clang'
  error('ncc requires Clang (it embeds assets with C23 #embed and Clang --embed-dir).')
endif

embed_check = '''
static const unsigned char ncc_embed_check[] = {
#embed "c_ncc.bnf"
};
int main(void) { return ncc_embed_check[0] == 0; }
'''
if not cc.compiles(embed_check,
                   args: ['-std=c23', '--embed-dir=' + meson.current_source_dir()],
                   name: 'C23 #embed support check')
  error('Compiler does not support C23 #embed with --embed-dir. Use Clang 22.1.0+ and put its bin directory ahead of older compilers in PATH.')
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
rstr_static_ref_styled = get_option('rstr_static_ref_template_styled')
if rstr_static_ref_styled != ''
  c_args += ['-DNCC_RSTR_STATIC_REF_TEMPLATE_STYLED="' + rstr_static_ref_styled + '"']
endif
rstr_static_ref_plain = get_option('rstr_static_ref_template_plain')
if rstr_static_ref_plain != ''
  c_args += ['-DNCC_RSTR_STATIC_REF_TEMPLATE_PLAIN="' + rstr_static_ref_plain + '"']
endif
rstr_static_ref_expr_styled = get_option('rstr_static_ref_expr_styled')
if rstr_static_ref_expr_styled != ''
  c_args += ['-DNCC_RSTR_STATIC_REF_EXPR_STYLED="' + rstr_static_ref_expr_styled + '"']
endif
rstr_static_ref_expr_plain = get_option('rstr_static_ref_expr_plain')
if rstr_static_ref_expr_plain != ''
  c_args += ['-DNCC_RSTR_STATIC_REF_EXPR_PLAIN="' + rstr_static_ref_expr_plain + '"']
endif
array_literal_data_template = get_option('array_literal_data_template')
if array_literal_data_template != ''
  c_args += ['-DNCC_ARRAY_LITERAL_DATA_TEMPLATE="' + array_literal_data_template + '"']
endif
array_literal_data_expr = get_option('array_literal_data_expr')
if array_literal_data_expr != ''
  c_args += ['-DNCC_ARRAY_LITERAL_DATA_EXPR="' + array_literal_data_expr + '"']
endif
static_object_entry_attr = get_option('static_object_entry_attr')
if static_object_entry_attr != ''
  c_args += ['-DNCC_STATIC_OBJECT_ENTRY_ATTR="' + static_object_entry_attr + '"']
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
rstr_text_style_type_opt = get_option('rstr_text_style_type')
if rstr_text_style_type_opt != ''
  c_args += ['-DNCC_RSTR_TEXT_STYLE_TYPE="' + rstr_text_style_type_opt + '"']
endif
rstr_style_record_type_opt = get_option('rstr_style_record_type')
if rstr_style_record_type_opt != ''
  c_args += ['-DNCC_RSTR_STYLE_RECORD_TYPE="' + rstr_style_record_type_opt + '"']
endif

# Memory debug tracking
mem_debug_enabled = get_option('mem_debug')
if mem_debug_enabled
  c_args += ['-DNCC_MEM_DEBUG']
endif

# Build-time default for n00b GC stack-map emission. The upstream ncc default
# remains off; n00b can opt its compiler build into stack maps explicitly.
gc_stack_maps_default = get_option('gc_stack_maps')
if gc_stack_maps_default
  c_args += ['-DNCC_GC_STACK_MAPS_DEFAULT']
endif

# Build-time default for ncc auto-registration of TU-scope pointer globals as
# libn00b GC roots. The upstream ncc default remains off; n00b opts in via
# its own meson build, and the migration of manual n00b_gc_register_root
# call sites lands in the same diff (D-008, D-006).
auto_gc_roots_default = get_option('auto_gc_roots')
if auto_gc_roots_default
  c_args += ['-DNCC_AUTO_GC_ROOTS_DEFAULT']
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
  'src/parse/symbol_populate.c',
  'src/parse/type_infer.c',
  'src/parse/c_tokenizer.c',
  'src/parse/comptime_check.c',
  'src/parse/comptime_guard.c',
  'src/parse/comptime_meta.c',
  'src/parse/comptime_image_emit.c',
  'src/parse/static_init_degrade.c',
  'src/parse/comptime_build.c',
  'src/parse/emit.c',
  'src/parse/commander.c',
  'src/parse/crt_entry.c',
)

src_util = files(
  'src/util/platform.c',
  'src/util/sha256.c',
  'src/util/type_normalize.c',
)

src_xform = files(
  'src/xform/transform.c',
  'src/xform/xform_array_literal.c',
  'src/xform/xform_helpers.c',
  'src/xform/xform_bang.c',
  'src/xform/xform_constexpr.c',
  'src/xform/xform_constexpr_paste.c',
  'src/xform/xform_contracts.c',
  'src/xform/xform_defer.c',
  'src/xform/xform_try.c',
  'src/xform/xform_nullable.c',
  'src/xform/nullability.c',
  'src/xform/nodiscard.c',
  'src/xform/union_deprecation.c',
  'src/xform/xform_generic_struct.c',
  'src/xform/xform_gc_globals.c',
  'src/xform/xform_gc_stack_maps.c',
  'src/xform/xform_kargs_vargs.c',
  'src/xform/xform_once.c',
  'src/xform/xform_option.c',
  'src/xform/xform_rpc.c',
  'src/xform/xform_rstr.c',
  'src/xform/xform_static_init.c',
  'src/xform/xform_static_image.c',
  'src/xform/xform_static_object.c',
  'src/xform/xform_type_layout.c',
  'src/xform/xform_typeid.c',
  'src/xform/xform_typestr.c',
  'src/xform/xform_typehash.c',
  'src/xform/xform_template.c',
  'src/xform/xform_gc_typemap.c',
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

test_array = executable('test_array',
  'test/test_array.c',
  c_args : c_args + coverage_args,
  link_args : coverage_link_args,
  dependencies : libncc_dep,
)
test('array_layout', test_array, suite : 'ncc')

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

test_crt_entry = executable('test_crt_entry',
  'test/test_crt_entry.c',
  c_args : c_args + coverage_args,
  link_args : coverage_link_args,
  dependencies : libncc_internal_dep,
)
test('crt_entry_emit', test_crt_entry, suite : 'ncc', timeout : 10)

test_comptime_meta = executable('test_comptime_meta',
  'test/test_comptime_meta.c',
  c_args : c_args + coverage_args,
  link_args : coverage_link_args,
  dependencies : libncc_internal_dep,
)
test('comptime_meta_emit', test_comptime_meta, suite : 'ncc', timeout : 10)

test_comptime_meta_io = executable('test_comptime_meta_io',
  'test/test_comptime_meta_io.c',
  c_args : c_args + coverage_args,
  link_args : coverage_link_args,
  dependencies : libncc_internal_dep,
)

test_comptime_build = executable('test_comptime_build',
  'test/test_comptime_build.c',
  c_args : c_args + coverage_args,
  link_args : coverage_link_args,
  dependencies : libncc_internal_dep,
)

test_comptime_symtab = executable('test_comptime_symtab',
  'test/test_comptime_symtab.c',
  c_args : c_args + coverage_args,
  link_args : coverage_link_args,
  dependencies : libncc_internal_dep,
)
test('comptime_symtab_flag', test_comptime_symtab, suite : 'ncc', timeout : 10)

test_comptime_check = executable('test_comptime_check',
  'test/test_comptime_check.c',
  c_args : c_args + coverage_args,
  link_args : coverage_link_args,
  dependencies : libncc_internal_dep,
)
test('comptime_readonly_check', test_comptime_check, suite : 'ncc', timeout : 10)

test_comptime_guard = executable('test_comptime_guard',
  'test/test_comptime_guard.c',
  c_args : c_args + coverage_args,
  link_args : coverage_link_args,
  dependencies : libncc_internal_dep,
)
test('comptime_guard_check', test_comptime_guard, suite : 'ncc', timeout : 10)

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
  '--ncc-rstr-static-ref-template-styled=$0 static ncc_string_t $1={.u8_bytes=$2,.data=$3,.codepoints=$4,.styling=$5};',
  '--ncc-rstr-static-ref-template-plain=static ncc_string_t $0={.u8_bytes=$1,.data=$2,.codepoints=$3,.styling=((void*)0)};',
  '--ncc-rstr-static-ref-expr-styled=&$1',
  '--ncc-rstr-static-ref-expr-plain=&$0',
  '--ncc-array-literal-data-template=static $0 $1[]={$3};',
  '--ncc-array-literal-data-expr=$1',
  '-I' + ncc_include_dir,
]

test('comptime_meta_object_io', test_comptime_meta_io,
  args : [ncc_exe.full_path(),
          test_dir / 'test_comptime_meta_main.c',
          test_dir / 'test_comptime_meta_no_main.c',
          test_dir / 'test_comptime_var.c']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps',
                             '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
  timeout : 20,
)

test('comptime_build_driver', test_comptime_build,
  args : [ncc_exe.full_path()] + ncc_test_flags
         + ['--ncc-no-gc-stack-maps', '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
  timeout : 30,
)

ncc_static_descriptor_flags = [
  '--ncc-static-object-entry-attr=__attribute__((used))',
  '--ncc-rstr-template-styled=({$0 static ncc_string_t $1={.u8_bytes=$2,.data=$3,.codepoints=$4,.styling=$5};$16 static const n00b_static_object_desc_t $8={.start=(const void*)&$1,.len=(uint64_t)sizeof($1),.tinfo=$6,.scan_kind=$12,.scan_cb=$13,.scan_user=$14,.object_id=$10,.file=__FILE__,.identity=$17,.flags=$11};static const n00b_static_object_desc_t * const $9 $15=&$8;&$1;})',
  '--ncc-rstr-template-plain=({static ncc_string_t $0={.u8_bytes=$1,.data=$2,.codepoints=$3,.styling=((void*)0)};$14 static const n00b_static_object_desc_t $6={.start=(const void*)&$0,.len=(uint64_t)sizeof($0),.tinfo=$4,.scan_kind=$10,.scan_cb=$11,.scan_user=$12,.object_id=$8,.file=__FILE__,.identity=$15,.flags=$9};static const n00b_static_object_desc_t * const $7 $13=&$6;&$0;})',
  '--ncc-rstr-static-ref-template-styled=$0 static ncc_string_t $1={.u8_bytes=$2,.data=$3,.codepoints=$4,.styling=$5};$16 static const n00b_static_object_desc_t $8={.start=(const void*)&$1,.len=(uint64_t)sizeof($1),.tinfo=$6,.scan_kind=$12,.scan_cb=$13,.scan_user=$14,.object_id=$10,.file=__FILE__,.identity=$17,.flags=$11};static const n00b_static_object_desc_t * const $9 $15=&$8;',
  '--ncc-rstr-static-ref-template-plain=static ncc_string_t $0={.u8_bytes=$1,.data=$2,.codepoints=$3,.styling=((void*)0)};$14 static const n00b_static_object_desc_t $6={.start=(const void*)&$0,.len=(uint64_t)sizeof($0),.tinfo=$4,.scan_kind=$10,.scan_cb=$11,.scan_user=$12,.object_id=$8,.file=__FILE__,.identity=$15,.flags=$9};static const n00b_static_object_desc_t * const $7 $13=&$6;',
  '--ncc-array-literal-data-template=$13 static $0 $1[]={$3};$20 static const n00b_static_object_desc_t $15={.start=(const void*)$1,.len=(uint64_t)sizeof($1),.tinfo=$4,.scan_kind=$6,.scan_cb=$7,.scan_user=$8,.object_id=$17,.file=__FILE__,.identity=$21,.flags=$18};static const n00b_static_object_desc_t * const $16 $19=&$15;',
]

# Compiler-level tests (run through ncc executable)
ncc_compile_tests = {
  'bang':          ['compile_run',  test_dir / 'test_bang.c'],
  'typehash_expr': ['compile_run',  test_dir / 'test_typehash_expr.c'],
  'typehash_scoped': ['compile_run',  test_dir / 'test_typehash_scoped.c'],
  'typehash_autotag': ['compile_run',  test_dir / 'test_typehash_autotag.c'],
  'array_literal': ['compile_run',  test_dir / 'test_array_literal.c'],
  'constexpr':    ['compile_run',  test_dir / 'test_constexpr.c'],
  'kargs':        ['compile_run',  test_dir / 'test_kargs.c'],
  'kargs_vargs':  ['compile_run',  test_dir / 'test_kargs_vargs.c'],
  'kargs_vargs_default_omitted': ['compile_run',
                                  test_dir / 'test_kargs_vargs_default_omitted.c'],
  'kargs_void_params': ['compile_run',
                        test_dir / 'test_kargs_void_params.c'],
  'kargs_many_metadata': ['compile_run',
                          test_dir / 'test_kargs_many_metadata.c'],
  'defer': ['compile_run', test_dir / 'test_defer.c'],
  'try': ['compile_run', test_dir / 'test_try.c'],
  'try_static_typedef': ['compile_run',
                         test_dir / 'test_try_static_typedef.c'],
  'nullable_syntax': ['compile_run', test_dir / 'test_nullable_syntax.c'],
  'function_contracts': ['compile_run',
                         test_dir / 'test_function_contracts.c'],
  'static_typedef_return': ['compile_run',
                            test_dir / 'test_static_typedef_return.c'],
  'function_contracts_parse': ['preprocess',
                               test_dir / 'test_function_contracts_parse.c'],
  'function_contract_keyword_collision': ['compile_run',
                                          test_dir / 'test_function_contract_keyword_collision.c'],
  'rstr':         ['compile_run',  test_dir / 'test_rstr.c'],
  'vargs':        ['compile_run',  test_dir / 'test_vargs.c'],
  'once':         ['compile_run',  test_dir / 'test_once.c'],
  'option':       ['compile_run',  test_dir / 'test_option.c'],
  'generic_struct': ['compile_run', test_dir / 'test_generic_struct.c'],
  'gs_result':      ['compile_run', test_dir / 'test_gs_result.c'],
  'rpc_keyword_collision': ['compile_run',
                            test_dir / 'test_rpc_keyword_collision.c'],
  'rpc_strip_smoke':       ['compile_run',
                            test_dir / 'test_rpc_strip_smoke.c'],
  'rpc_unary':             ['compile_run',
                            test_dir / 'test_rpc_unary.c'],
  'rpc_server_stream':     ['compile_run',
                            test_dir / 'test_rpc_server_stream.c'],
  'rpc_client_stream':     ['compile_run',
                            test_dir / 'test_rpc_client_stream.c'],
  'rpc_bidi':              ['compile_run',
                            test_dir / 'test_rpc_bidi.c'],
  'comptime_meta_section': ['comptime_section_present',
                            test_dir / 'test_comptime_meta_main.c'],
  'comptime_meta_no_section': ['comptime_section_absent',
                               test_dir / 'test_comptime_meta_no_main.c'],
  'comptime_meta_proto_no_section': ['comptime_section_absent',
                                     test_dir / 'test_comptime_meta_proto.c'],
  'comptime_var_record': ['comptime_var_record_present',
                          test_dir / 'test_comptime_var.c'],
}

foreach name, params : ncc_compile_tests
  extra_args = []
  test_deps = [ncc_exe]
  # WP-002 Phase 1 (D-031): stack-maps default flipped to on. These
  # compile_run fixtures don't link the libn00b runtime that
  # provides n00b_gc_stack_push / _pop, so the transform would emit
  # references that fail to compile. Opt out per-test.
  # WP-003 Phase 1 (D-031): auto-roots default flipped to on. Same
  # reason — these fixtures don't link libn00b and therefore do not
  # provide the n00b GC root section types. Opt out per-test.
  test('ncc_' + name, test_runner,
    args : [ncc_exe, params[0], params[1]] + ncc_test_flags + extra_args
           + ['--ncc-no-gc-stack-maps', '--ncc-no-auto-gc-roots'],
    suite : 'ncc',
    depends : test_deps,
  )
endforeach

test('ncc_function_contracts_helper_shape', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_function_contracts_wrapper_shape.c',
          'static int __ncc_cw_shaped_body']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps',
                             '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_comptime_var_modifier_stripped', test_runner,
  args : [ncc_exe, 'preprocess_not_contains',
          test_dir / 'test_comptime_var.c',
          '[[n00b::comptime]]']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps',
                             '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_comptime_readonly_error', test_runner,
  args : [ncc_exe, 'expect_error_contains_no_output',
          test_dir / 'test_comptime_readonly_error.c',
          '6:5: comptime variable \'answer\' may only be assigned in comptime_main']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps',
                             '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_comptime_readonly_member_error', test_runner,
  args : [ncc_exe, 'expect_error_contains_no_output',
          test_dir / 'test_comptime_readonly_member_error.c',
          '9:5: comptime variable \'state\' may only be assigned in comptime_main']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps',
                             '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_comptime_readonly_subscript_error', test_runner,
  args : [ncc_exe, 'expect_error_contains_no_output',
          test_dir / 'test_comptime_readonly_subscript_error.c',
          '6:5: comptime variable \'slots\' may only be assigned in comptime_main']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps',
                             '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_comptime_readonly_deref_error', test_runner,
  args : [ncc_exe, 'expect_error_contains_no_output',
          test_dir / 'test_comptime_readonly_deref_error.c',
          '9:5: comptime variable \'root\' may only be assigned in comptime_main']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps',
                             '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_comptime_optional_ok', test_runner,
  args : [ncc_exe, 'preprocess_not_contains',
          test_dir / 'test_comptime_optional_ok.c',
          '[[n00b::optional]]']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps',
                             '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_comptime_optional_misplaced', test_runner,
  args : [ncc_exe, 'expect_error_contains_no_output',
          test_dir / 'test_comptime_optional_misplaced.c',
          '[[n00b::optional]] is only valid on a comptime_main definition']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps',
                             '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_comptime_optional_no_main', test_runner,
  args : [ncc_exe, 'expect_error_contains_no_output',
          test_dir / 'test_comptime_optional_no_main.c',
          '[[n00b::optional]] is only valid on a comptime_main definition']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps',
                             '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_comptime_smoke', test_runner,
  args : [ncc_exe, 'comptime_smoke', test_dir / 'test_comptime_smoke.c']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps',
                             '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
  timeout : 20,
)

test('ncc_comptime_object_link', test_runner,
  args : [ncc_exe, 'comptime_object_link',
          test_dir / 'test_comptime_smoke.c']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps',
                             '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
  timeout : 20,
)

test('ncc_comptime_mixed_link', test_runner,
  args : [ncc_exe, 'comptime_mixed_link',
          test_dir / 'test_comptime_smoke.c']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps',
                             '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
  timeout : 20,
)

test('ncc_comptime_link_order', test_runner,
  args : [ncc_exe, 'comptime_link_order',
          test_dir / 'test_comptime_link_order.c']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps',
                             '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
  timeout : 20,
)

test('ncc_comptime_forced_include', test_runner,
  args : [ncc_exe, 'comptime_forced_include',
          test_dir / 'test_comptime_smoke.c']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps',
                             '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
  timeout : 20,
)

test('ncc_comptime_fail', test_runner,
  args : [ncc_exe, 'comptime_fail', test_dir / 'test_comptime_fail.c']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps',
                             '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
  timeout : 20,
)

test('ncc_comptime_atomic_exit', test_runner,
  args : [ncc_exe, 'comptime_atomic_exit',
          test_dir / 'test_comptime_fail.c']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps',
                             '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
  timeout : 20,
)

test('ncc_comptime_atomic_signal', test_runner,
  args : [ncc_exe, 'comptime_atomic_signal',
          test_dir / 'test_comptime_atomic_signal.c']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps',
                             '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
  timeout : 20,
)

test('ncc_comptime_no_comptime_flag', test_runner,
  args : [ncc_exe, 'comptime_no_comptime',
          test_dir / 'test_comptime_smoke.c']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps',
                             '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
  timeout : 20,
)

test('ncc_comptime_optional_metadata', test_runner,
  args : [ncc_exe, 'comptime_optional_metadata',
          test_dir / 'test_comptime_optional_metadata.c']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps',
                             '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
  timeout : 20,
)

test('ncc_comptime_degrade', test_runner,
  args : [ncc_exe, 'comptime_degrade',
          test_dir / 'test_comptime_degrade.c']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps',
                             '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
  timeout : 20,
)

test('ncc_comptime_xcompile_guard', test_runner,
  args : [ncc_exe, 'comptime_xcompile_guard',
          test_dir / 'test_comptime_xcompile_guard.c']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps',
                             '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
  timeout : 20,
)

test('ncc_comptime_var_link_strip', test_runner,
  args : [ncc_exe, 'comptime_var_link_strip',
          test_dir / 'test_comptime_var.c']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps',
                             '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
  timeout : 20,
)

test('ncc_comptime_var_object_link_strip', test_runner,
  args : [ncc_exe, 'comptime_var_object_link_strip',
          test_dir / 'test_comptime_var.c']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps',
                             '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
  timeout : 20,
)

test('ncc_comptime_image_e2e', test_runner,
  args : [ncc_exe, 'comptime_image_e2e',
          test_dir / 'test_comptime_image_e2e.c'],
  suite : 'ncc',
  depends : ncc_exe,
  timeout : 30,
)

test('ncc_static_init_const_e2e', test_runner,
  args : [ncc_exe, 'comptime_image_e2e',
          test_dir / 'test_static_init_const_e2e.c'],
  suite : 'ncc',
  depends : ncc_exe,
  timeout : 30,
)

test('ncc_static_init_writable_e2e', test_runner,
  args : [ncc_exe, 'comptime_image_e2e',
          test_dir / 'test_static_init_writable_e2e.c'],
  suite : 'ncc',
  depends : ncc_exe,
  timeout : 30,
)

test('ncc_static_init_buffer_representative', test_runner,
  args : [ncc_exe, 'comptime_image_e2e',
          test_dir / 'test_static_init_buffer_representative.c'],
  suite : 'ncc',
  depends : ncc_exe,
  timeout : 30,
)

test('ncc_buffer_static_migrated', test_runner,
  args : [ncc_exe, 'comptime_image_e2e',
          test_dir / 'test_buffer_static_migrated.c'],
  suite : 'ncc',
  depends : ncc_exe,
  timeout : 30,
)

test('ncc_array_static_migrated', test_runner,
  args : [ncc_exe, 'comptime_image_e2e',
          test_dir / 'test_array_static_migrated.c'],
  suite : 'ncc',
  depends : ncc_exe,
  timeout : 90,
)

test('ncc_array_static_minimal_include', test_runner,
  args : [ncc_exe, 'comptime_image_e2e',
          test_dir / 'test_array_static_minimal_include.c'],
  suite : 'ncc',
  depends : ncc_exe,
  timeout : 60,
)

test('ncc_array_static_indirect_alloc_include', test_runner,
  args : [ncc_exe, 'comptime_image_e2e',
          test_dir / 'test_array_static_indirect_alloc_include.c'],
  suite : 'ncc',
  depends : ncc_exe,
  timeout : 60,
)

test('ncc_array_static_const_elem_migrated', test_runner,
  args : [ncc_exe, 'comptime_image_e2e',
          test_dir / 'test_array_static_const_elem_migrated.c'],
  suite : 'ncc',
  depends : ncc_exe,
  timeout : 60,
)

test('ncc_list_static_migrated', test_runner,
  args : [ncc_exe, 'comptime_image_e2e',
          test_dir / 'test_list_static_migrated.c'],
  suite : 'ncc',
  depends : ncc_exe,
  timeout : 60,
)

test('ncc_dict_static_migrated', test_runner,
  args : [ncc_exe, 'comptime_image_e2e',
          test_dir / 'test_dict_static_migrated.c'],
  suite : 'ncc',
  depends : ncc_exe,
  timeout : 60,
)

test('ncc_style_static_migrated', test_runner,
  args : [ncc_exe, 'comptime_image_e2e',
          test_dir / 'test_style_static_migrated.c'],
  suite : 'ncc',
  depends : ncc_exe,
  timeout : 60,
)

test('ncc_container_baking_no_helper', test_runner,
  args : [ncc_exe, 'comptime_image_e2e',
          test_dir / 'test_container_baking_no_helper.c'],
  suite : 'ncc',
  depends : ncc_exe,
  timeout : 90,
)

test('ncc_dict_baking_no_helper', test_runner,
  args : [ncc_exe, 'comptime_image_e2e',
          test_dir / 'test_dict_baking_no_helper.c'],
  suite : 'ncc',
  depends : ncc_exe,
  timeout : 60,
)

test('ncc_array_static_const_error', test_runner,
  args : [ncc_exe, 'expect_error_contains',
          test_dir / 'test_array_static_const_error.c',
          'const array static-init migration'],
  suite : 'ncc',
  depends : ncc_exe,
  timeout : 30,
)

test('ncc_list_static_const_error', test_runner,
  args : [ncc_exe, 'expect_error_contains',
          test_dir / 'test_list_static_const_error.c',
          'const value-root static initializer'],
  suite : 'ncc',
  depends : ncc_exe,
  timeout : 30,
)

test('ncc_dict_static_const_error', test_runner,
  args : [ncc_exe, 'expect_error_contains',
          test_dir / 'test_dict_static_const_error.c',
          'const value-root static initializer'],
  suite : 'ncc',
  depends : ncc_exe,
  timeout : 30,
)

test('ncc_dict_static_unsupported_key_error', test_runner,
  args : [ncc_exe, 'expect_error_contains',
          test_dir / 'test_dict_static_unsupported_key_error.c',
          'scalar, r-string, and buffer key types only'],
  suite : 'ncc',
  depends : ncc_exe,
  timeout : 30,
)

test('ncc_dict_static_unsupported_value_error', test_runner,
  args : [ncc_exe, 'expect_error_contains',
          test_dir / 'test_dict_static_unsupported_value_error.c',
          'supports scalar values, plain r-string pointer values, buffer pointer values, and marshalable static pointer values only'],
  suite : 'ncc',
  depends : ncc_exe,
  timeout : 30,
)

test('ncc_dict_static_computed_pointer_migrated', test_runner,
  args : [ncc_exe, 'comptime_image_e2e',
          test_dir / 'test_dict_static_computed_pointer_migrated.c'],
  suite : 'ncc',
  depends : ncc_exe,
  timeout : 60,
)

test('ncc_dict_static_computed_pointer_unsupported_pointee_error', test_runner,
  args : [ncc_exe, 'expect_error_contains',
          test_dir / 'test_dict_static_computed_pointer_unsupported_pointee_error.c',
          'computed migrated pointer dict value pointee'],
  suite : 'ncc',
  depends : ncc_exe,
  timeout : 30,
)

test('ncc_dict_static_aggregate_value_error', test_runner,
  args : [ncc_exe, 'expect_error_contains',
          test_dir / 'test_dict_static_aggregate_value_error.c',
          'nested containers and aggregate values still require'],
  suite : 'ncc',
  depends : ncc_exe,
  timeout : 30,
)

test('ncc_dict_static_styled_value_error', test_runner,
  args : [ncc_exe, 'expect_error_contains',
          test_dir / 'test_dict_static_styled_value_error.c',
          'styled r-string dict values are not supported',
          '--ncc-rstr-string-type=n00b_string_t'],
  suite : 'ncc',
  depends : ncc_exe,
  timeout : 30,
)

test('ncc_list_static_styled_error', test_runner,
  args : [ncc_exe, 'expect_error_contains',
          test_dir / 'test_list_static_styled_error.c',
          'styled r-string elements are not supported',
          '--ncc-rstr-string-type=n00b_string_t'],
  suite : 'ncc',
  depends : ncc_exe,
  timeout : 30,
)

test('ncc_list_static_aggregate_scalar_error', test_runner,
  args : [ncc_exe, 'expect_error_contains',
          test_dir / 'test_list_static_aggregate_scalar_error.c',
          'non-pointer field'],
  suite : 'ncc',
  depends : ncc_exe,
  timeout : 30,
)

test('ncc_array_static_styled_error', test_runner,
  args : [ncc_exe, 'expect_error_contains',
          test_dir / 'test_array_static_styled_error.c',
          'styled r-string elements are not supported',
          '--ncc-rstr-string-type=n00b_string_t'],
  suite : 'ncc',
  depends : ncc_exe,
  timeout : 30,
)

test('ncc_array_static_aggregate_scalar_error', test_runner,
  args : [ncc_exe, 'expect_error_contains',
          test_dir / 'test_array_static_aggregate_scalar_error.c',
          'non-pointer field'],
  suite : 'ncc',
  depends : ncc_exe,
  timeout : 30,
)

test('ncc_static_init_mutation_error', test_runner,
  args : [ncc_exe, 'static_init_mutation_error',
          test_dir / 'test_static_init_mutation_error.c'],
  suite : 'ncc',
  depends : ncc_exe,
  timeout : 30,
)

test('ncc_static_init_no_comptime_degrade', test_runner,
  args : [ncc_exe, 'static_init_no_comptime_degrade',
          test_dir / 'test_static_init_const_e2e.c'],
  suite : 'ncc',
  depends : ncc_exe,
  timeout : 30,
)

test('ncc_static_init_writable_no_comptime_degrade', test_runner,
  args : [ncc_exe, 'static_init_no_comptime_degrade',
          test_dir / 'test_static_init_writable_e2e.c'],
  suite : 'ncc',
  depends : ncc_exe,
  timeout : 30,
)

test('ncc_static_init_internal_error', test_runner,
  args : [ncc_exe, 'expect_error_contains_no_output',
          test_dir / 'test_static_init_internal_error.c',
          'static initializer \'state\' has internal linkage']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps',
                             '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_function_contracts_result_return', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_function_contracts_wrapper_shape.c',
          'return result']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps',
                             '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_function_contracts_debug_gate', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_function_contracts_wrapper_shape.c',
          '#ifndef NDEBUG']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps',
                             '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_function_contracts_trap', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_function_contracts_wrapper_shape.c',
          '__builtin_trap']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps',
                             '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_function_contracts_later_requires_lowered', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_function_contracts_wrapper_shape.c',
          'x < 1000']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps',
                             '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_function_contracts_later_ensures_lowered', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_function_contracts_wrapper_shape.c',
          'result < 1000']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps',
                             '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_function_contracts_requires_removed', test_runner,
  args : [ncc_exe, 'preprocess_not_contains',
          test_dir / 'test_function_contracts_wrapper_shape.c',
          'requires']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps',
                             '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_function_contracts_ensures_removed', test_runner,
  args : [ncc_exe, 'preprocess_not_contains',
          test_dir / 'test_function_contracts_wrapper_shape.c',
          'ensures']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps',
                             '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_function_contracts_kargs_requires_removed', test_runner,
  args : [ncc_exe, 'preprocess_not_contains',
          test_dir / 'test_function_contracts.c',
          'requires']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps',
                             '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_function_contracts_kargs_ensures_removed', test_runner,
  args : [ncc_exe, 'preprocess_not_contains',
          test_dir / 'test_function_contracts.c',
          'ensures']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps',
                             '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_function_contracts_fnptr_arg_forwarding', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_function_contracts_wrapper_shape.c',
          '__ncc_cw_apply_fn_body (fn, x)']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps',
                             '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_function_contracts_array_arg_forwarding', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_function_contracts_wrapper_shape.c',
          '__ncc_cw_first_value_body (values)']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps',
                             '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_function_contracts_support_statements_preserved', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_function_contracts_wrapper_shape.c',
          'support_shadow = x + 1']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps',
                             '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_function_contracts_kargs_helper_uses_local', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_function_contracts_wrapper_shape.c',
          '__ncc_cw_shaped_kargs_body (x, bias)']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps',
                             '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_function_contracts_extension_helper_shape', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_function_contracts_wrapper_shape.c',
          'static int __ncc_cw_extension_contract_body']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps',
                             '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_function_contracts_release_kargs_statement_default', test_runner,
  args : [ncc_exe, 'compile_run',
          test_dir / 'test_function_contracts.c']
         + ncc_test_flags + ['-DNDEBUG', '--ncc-no-gc-stack-maps',
                             '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_static_object_descriptors', test_runner,
  args : [ncc_exe, 'compile_run',
          test_dir / 'test_static_object_descriptors.c']
         + ncc_test_flags + ncc_static_descriptor_flags
         + ['--ncc-no-gc-stack-maps', '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_static_object_descriptors_array_data', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_static_object_descriptors.c',
          'static int __ncc_arraylit_']
         + ncc_test_flags + ncc_static_descriptor_flags,
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_static_object_descriptors_rstr_desc', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_static_object_descriptors.c',
          'n00b_static_object_desc_t _ncc_rsd_']
         + ncc_test_flags + ncc_static_descriptor_flags,
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_static_object_descriptors_pointer_scan', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_static_object_descriptors.c',
          '.scan_kind = 2']
         + ncc_test_flags + ncc_static_descriptor_flags,
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_static_object_descriptors_nested_scan', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_static_object_descriptors.c',
          'n00b_gc_scan_cb_struct_field']
         + ncc_test_flags + ncc_static_descriptor_flags,
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_static_object_descriptors_sparse_scan', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_static_object_descriptors.c',
          'n00b_gc_scan_cb_struct_layout']
         + ncc_test_flags + ncc_static_descriptor_flags,
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_static_object_descriptors_sparse_offset', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_static_object_descriptors.c',
          '__builtin_offsetof (descriptor_sparse_item_t, left)']
         + ncc_test_flags + ncc_static_descriptor_flags,
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_static_object_descriptors_entry_attr', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_static_object_descriptors.c',
          '__attribute__ ((used))']
         + ncc_test_flags + ncc_static_descriptor_flags,
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_typemap_direct_pointer_offset', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_gc_typemap.c',
          '__builtin_offsetof(gcmap_direct_fn_t,payload)']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps', '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_typemap_direct_function_pointer_skipped', test_runner,
  args : [ncc_exe, 'preprocess_not_contains',
          test_dir / 'test_gc_typemap.c',
          '__builtin_offsetof(gcmap_direct_fn_t,callback)']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps', '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_typemap_typedef_pointer_offset', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_gc_typemap.c',
          '__builtin_offsetof(gcmap_typedef_fn_t,payload)']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps', '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_typemap_typedef_function_pointer_skipped', test_runner,
  args : [ncc_exe, 'preprocess_not_contains',
          test_dir / 'test_gc_typemap.c',
          '__builtin_offsetof(gcmap_typedef_fn_t,callback)']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps', '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_typemap_user_scan_user_scanned', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_gc_typemap.c',
          '__builtin_offsetof(gcmap_user_scan_field_t,scan_user)']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps', '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_typemap_runtime_scan_user_skipped', test_runner,
  args : [ncc_exe, 'preprocess_not_contains',
          test_dir / 'test_gc_typemap.c',
          '__builtin_offsetof(gcmap_runtime_shape_t,scan_user)']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps', '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_typemap_runtime_lock_skipped', test_runner,
  args : [ncc_exe, 'preprocess_not_contains',
          test_dir / 'test_gc_typemap.c',
          '__builtin_offsetof(gcmap_runtime_shape_t,lock)']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps', '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_typemap_runtime_allocator_skipped', test_runner,
  args : [ncc_exe, 'preprocess_not_contains',
          test_dir / 'test_gc_typemap.c',
          '__builtin_offsetof(gcmap_runtime_shape_t,allocator)']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps', '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_typemap_runtime_payload_scanned', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_gc_typemap.c',
          '__builtin_offsetof(gcmap_runtime_shape_t,payload)']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps', '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_typemap_runtime_near_miss_scan_user_scanned', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_gc_typemap.c',
          '__builtin_offsetof(gcmap_runtime_near_miss_t,scan_user)']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps', '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_typemap_noscan_trailing_field_skipped', test_runner,
  args : [ncc_exe, 'preprocess_not_contains',
          test_dir / 'test_gc_typemap.c',
          '__builtin_offsetof(gcmap_noscan_trailing_t,noscan_ptr)']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps', '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_typemap_noscan_trailing_sibling_scanned', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_gc_typemap.c',
          '__builtin_offsetof(gcmap_noscan_trailing_t,payload)']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps', '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_typemap_noscan_prefix_field_skipped', test_runner,
  args : [ncc_exe, 'preprocess_not_contains',
          test_dir / 'test_gc_typemap.c',
          '__builtin_offsetof(gcmap_noscan_prefix_t,noscan_ptr)']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps', '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_typemap_noscan_prefix_sibling_scanned', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_gc_typemap.c',
          '__builtin_offsetof(gcmap_noscan_prefix_t,kept)']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps', '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_typemap_noscan_attribute_stripped', test_runner,
  args : [ncc_exe, 'preprocess_not_contains',
          test_dir / 'test_gc_typemap.c',
          'n00b::noscan']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps', '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

# --- WP-001: [[n00b::transient]] field attribute --------------------------

# A transient table descriptor type is emitted.
test('ncc_transient_table_emitted', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_transient.c',
          'n00b_transient_layout_t']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps', '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

# The trmap section entry is emitted.
test('ncc_transient_section_emitted', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_transient.c',
          'n00b_trmap']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps', '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

# A trailing-transient sub-word scalar emits a BYTE size expression.
test('ncc_transient_trailing_scalar_byte_size', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_transient.c',
          'sizeof(((trans_trailing_t *)0)->fd)']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps', '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

# ...and a raw byte offset -- NOT divided by sizeof(void*) (byte-granular).
test('ncc_transient_trailing_scalar_not_word_divided', test_runner,
  args : [ncc_exe, 'preprocess_not_contains',
          test_dir / 'test_transient.c',
          '__builtin_offsetof(trans_trailing_t,fd)/sizeof(void*)']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps', '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

# A non-transient pointer sibling stays OUT of the transient table...
test('ncc_transient_sibling_absent_from_table', test_runner,
  args : [ncc_exe, 'preprocess_not_contains',
          test_dir / 'test_transient.c',
          'sizeof(((trans_trailing_t *)0)->keep_ptr)']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps', '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

# ...but is still scanned by the GC pointer map.
test('ncc_transient_sibling_still_gc_scanned', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_transient.c',
          '(__builtin_offsetof(trans_trailing_t,keep_ptr)/sizeof(void*))']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps', '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

# A prefix-transient pointer is in the transient table...
test('ncc_transient_prefix_pointer_in_table', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_transient.c',
          'sizeof(((trans_prefix_t *)0)->handle)']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps', '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

# ...AND still GC-scanned (transient is not noscan).
test('ncc_transient_pointer_still_gc_scanned', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_transient.c',
          '(__builtin_offsetof(trans_prefix_t,handle)/sizeof(void*))']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps', '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

# A plain scalar in a transient-free type appears in neither table.
test('ncc_transient_plain_scalar_absent', test_runner,
  args : [ncc_exe, 'preprocess_not_contains',
          test_dir / 'test_transient.c',
          'sizeof(((trans_none_t *)0)->plain)']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps', '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

# The ncc-only attribute is stripped before C is emitted (upstream auto-strip).
test('ncc_transient_attribute_stripped', test_runner,
  args : [ncc_exe, 'preprocess_not_contains',
          test_dir / 'test_transient.c',
          'n00b::transient']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps', '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_typemap_static_assert_c23', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_gc_typemap.c',
          'static_assert((__builtin_offsetof(gcmap_direct_fn_t,payload)%sizeof(void*))==0']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps', '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_typemap_no_legacy_static_assert', test_runner,
  args : [ncc_exe, 'preprocess_not_contains',
          test_dir / 'test_gc_typemap.c',
          '_Static_assert']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps', '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_typemap_section_attr_c23', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_gc_typemap.c',
          '[[gnu::section("n00b_gcmap"), gnu::used]]']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps', '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_typemap_legacy_section_attr_rewritten_c23', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_gc_typemap.c',
          '[[gnu::section("__DATA,n00b_gcmap"), gnu::used]]']
         + ncc_test_flags
         + ['--ncc-no-gc-stack-maps',
            '--ncc-no-auto-gc-roots',
            '--ncc-static-object-entry-attr=__attribute__((section("__DATA,n00b_stobj"),used))'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_typemap_legacy_section_attr_no_legacy_attribute', test_runner,
  args : [ncc_exe, 'preprocess_not_contains',
          test_dir / 'test_gc_typemap.c',
          '__attribute__((section("__DATA,n00b_gcmap")']
         + ncc_test_flags
         + ['--ncc-no-gc-stack-maps',
            '--ncc-no-auto-gc-roots',
            '--ncc-static-object-entry-attr=__attribute__((section("__DATA,n00b_stobj"),used))'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_typemap_gcidx_entry', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_gc_typemap.c',
          '__ncc_gcidx_ent_']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps', '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_typemap_disabled_omits_gcmap', test_runner,
  args : [ncc_exe, 'preprocess_not_contains',
          test_dir / 'test_gc_typemap.c',
          'n00b_gcmap']
         + ncc_test_flags
         + ['--ncc-no-gc-stack-maps',
            '--ncc-no-auto-gc-roots',
            '--ncc-no-gc-typemaps'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_typemap_disabled_omits_gcidx', test_runner,
  args : [ncc_exe, 'preprocess_not_contains',
          test_dir / 'test_gc_typemap.c',
          '__ncc_gcidx_ent_']
         + ncc_test_flags
         + ['--ncc-no-gc-stack-maps',
            '--ncc-no-auto-gc-roots',
            '--ncc-no-gc-typemaps'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_typemap_pointer_array_skipped', test_runner,
  args : [ncc_exe, 'preprocess_not_contains',
          test_dir / 'test_gc_typemap.c',
          'sizeof(gcmap_pointer_array_t)']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps', '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_typemap_union_mixed_skipped', test_runner,
  args : [ncc_exe, 'preprocess_not_contains',
          test_dir / 'test_gc_typemap.c',
          'sizeof(gcmap_union_mixed_t)']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps', '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

# Discriminated-union (n00b_variant_t) GC maps: a variant's value word is
# described precisely via its selector discriminant instead of warning + a
# conservative scan.

# A pointer-bearing variant emits a variant descriptor (not a flat offset).
test('ncc_gc_typemap_variant_descriptor_emitted', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_gc_typemap_variant.c',
          'n00b_gc_variant_field_t']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps', '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

# The descriptor records the selector offset and, per alternative, the
# element-relative pointer offsets that are live when that selector is active.
test('ncc_gc_typemap_variant_selector_offset', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_gc_typemap_variant.c',
          '__builtin_offsetof(vmap_variant_t,selector)']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps', '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

# A single-pointer alternative's pointer is the value word, addressed through
# its named union field (value.field_0), not a bare `value` word.
test('ncc_gc_typemap_variant_value_ptr_offset', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_gc_typemap_variant.c',
          '__builtin_offsetof(vmap_variant_t,value.field_0)']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps', '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

# A by-value AGGREGATE alternative is described precisely: an arm table
# (n00b_gc_variant_arm_t) carries the pointer offsets of the struct's fields,
# addressed through value.<field>.<member>. (Previously this fell back to a
# conservative scan.)
test('ncc_gc_typemap_variant_aggregate_arm', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_gc_typemap_variant.c',
          'n00b_gc_variant_arm_t']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps', '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_typemap_variant_aggregate_arm_field_offset', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_gc_typemap_variant.c',
          '__builtin_offsetof(vmap_agg_variant_t,value.field_0.a)']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps', '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

# An EMBEDDED variant field is described in-place: the selector/value offsets
# are taken through the containing struct's field path.
test('ncc_gc_typemap_variant_embedded', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_gc_typemap_variant.c',
          '__builtin_offsetof(vmap_holder_t,choice.selector)']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps', '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

# The containing struct still gets its ordinary (unconditional) pointer offsets
# alongside the variant descriptor.
test('ncc_gc_typemap_variant_embedded_flat_offset', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_gc_typemap_variant.c',
          '__builtin_offsetof(vmap_holder_t,head)']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps', '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

# An all-scalar variant stays precise without a variant descriptor (its value
# word is never a heap pointer) and must not be skipped.
test('ncc_gc_typemap_variant_all_scalar_no_descriptor', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_gc_typemap_variant.c',
          'sizeof(vmap_scalar_variant_t)']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps', '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

# A _generic_struct variant (the real n00b_variant_t shape) resolves via the
# type model and gets a precise variant descriptor, even though the legacy
# aggregate table cannot resolve its typedef alias.
test('ncc_gc_typemap_variant_generic_struct', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_gc_typemap_variant.c',
          '__builtin_offsetof(vmap_generic_variant_t,selector)']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps', '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

# Traditional-union deprecation: a bare (non-variant) union is steered toward
# n00b_variant_t.

# A hand-rolled tagged union warns.
test('ncc_union_deprecation_warn', test_runner,
  args : [ncc_exe, 'preprocess_stderr_contains',
          test_dir / 'test_union_deprecation.c',
          'traditional C union is discouraged']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps', '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

# --ncc-allow-unions suppresses the warning entirely.
test('ncc_union_deprecation_allow', test_runner,
  args : [ncc_exe, 'preprocess_stderr_omits',
          test_dir / 'test_union_deprecation.c',
          'traditional C union']
         + ncc_test_flags
         + ['--ncc-no-gc-stack-maps', '--ncc-no-auto-gc-roots',
            '--ncc-allow-unions'],
  suite : 'ncc',
  depends : ncc_exe,
)

# --ncc-error-on-union makes it a hard error (non-zero exit).
test('ncc_union_deprecation_error', test_runner,
  args : [ncc_exe, 'expect_error_contains',
          test_dir / 'test_union_deprecation.c',
          'traditional C union is discouraged']
         + ncc_test_flags
         + ['--ncc-no-gc-stack-maps', '--ncc-no-auto-gc-roots',
            '--ncc-error-on-union'],
  suite : 'ncc',
  depends : ncc_exe,
)

# An n00b_variant_t value-union and a [[n00b::raw_union]] union are NOT flagged.
test('ncc_union_deprecation_excluded', test_runner,
  args : [ncc_exe, 'preprocess_stderr_omits',
          test_dir / 'test_union_deprecation_excluded.c',
          'traditional C union']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps', '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

if host_machine.system() != 'windows' and cc.has_header('pthread.h')
  test('ncc_constexpr_pthread', test_runner,
    args : [ncc_exe, 'compile_run', test_dir / 'test_constexpr_pthread.c']
           + ncc_test_flags + ['--ncc-no-gc-stack-maps'],
    suite : 'ncc',
    depends : ncc_exe,
  )
endif

# rstr_ansi test links against libncc (ncc_string_to_ansi lives there).
test('ncc_rstr_ansi', test_runner,
  args : [ncc_exe, 'compile_run', test_dir / 'test_rstr_ansi.c']
         + ncc_test_flags + [libncc.full_path()] + ['--ncc-no-gc-stack-maps'],
  suite : 'ncc',
  depends : [ncc_exe, libncc],
)

test('ncc_gc_stack_maps_default_off', test_runner,
  args : [ncc_exe, 'preprocess_not_contains',
          test_dir / 'test_gc_stack_maps_flag.c', 'n00b_gc_stack_']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps'],
  suite : 'ncc',
  depends : ncc_exe,
)

# WP-002 Phase 1: pins the stack-maps default-on invariant (D-031).
# With the `gc_stack_maps` meson option's default flipped to `true`
# (and NCC_GC_STACK_MAPS_DEFAULT defined accordingly), ncc invoked
# WITHOUT any --ncc-gc-stack-maps flag must still fire the transform
# on a function with an eligible stack-local pointer slot. Locks the
# default-on behavior against future regressions.
test('ncc_gc_stack_maps_default_on', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_gc_stack_maps_default_on.c', 'n00b_gc_stack_']
         + ncc_test_flags,
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_stack_maps_nogc_attribute_skips_function', test_runner,
  args : [ncc_exe, 'preprocess_not_contains',
          test_dir / 'test_gc_stack_maps_nogc_attribute.c', 'n00b_gc_stack_']
         + ncc_test_flags,
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_stack_maps_nogc_attribute_stripped', test_runner,
  args : [ncc_exe, 'preprocess_not_contains',
          test_dir / 'test_gc_stack_maps_nogc_attribute.c', 'n00b::nogc']
         + ncc_test_flags,
  suite : 'ncc',
  depends : ncc_exe,
)

# WP-001 Phase 1: pins the no-op-when-off contract for the
# xform_gc_globals transform (spec § 6.1, D-006). Even when the
# source contains a TU-scope pointer-bearing static that would
# qualify under the eligibility rules of later phases, ncc invoked
# WITHOUT --ncc-auto-gc-roots must emit zero auto-roots output.
# Under WP-003 Phase 1 (D-031) the auto-roots default flipped to
# on, so this test must explicitly pass --ncc-no-auto-gc-roots to
# exercise the off path that the assertion pins.
test('ncc_auto_gc_roots_flag_off_no_op', test_runner,
  args : [ncc_exe, 'preprocess_not_contains',
          test_dir / 'test_auto_gc_roots_flag_off_no_op.c',
          '__ncc_gc_root_table_']
         + ncc_test_flags + ['--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

# WP-003 Phase 1: pins the auto-roots default-on invariant (D-031).
# With the `auto_gc_roots` meson option's default flipped to `true`
# (and NCC_AUTO_GC_ROOTS_DEFAULT defined accordingly), ncc invoked
# WITHOUT any --ncc-auto-gc-roots flag must still fire the
# xform_gc_globals transform on a TU-scope pointer-bearing decl.
# Locks the default-on behavior against future regressions.
test('ncc_auto_gc_roots_default_on', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_auto_gc_roots_default_on.c',
          '__ncc_gc_root_table_']
         + ncc_test_flags,
  suite : 'ncc',
  depends : ncc_exe,
)

# WP-001 Phase 2: pointer-scalar TU-scope decls register. The
# emit shape per spec § 4.1 / D-005 is one root table and one
# linker-section entry per TU. Three assertions pin the per-symbol
# entries (`& x` and `& y` in declaration order) and the section
# descriptor. All use the same source so a missing emit fails every
# check.
test('ncc_auto_gc_roots_pointer_scalar_x', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_auto_gc_roots_pointer_scalar.c',
          '(void *) & x']
         + ncc_test_flags + ['--ncc-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_auto_gc_roots_pointer_scalar_y', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_auto_gc_roots_pointer_scalar.c',
          '(void *) & y']
         + ncc_test_flags + ['--ncc-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_auto_gc_roots_pointer_scalar_section_entry', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_auto_gc_roots_pointer_scalar.c',
          '__ncc_gc_root_section_entry_']
         + ncc_test_flags + ['--ncc-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

# WP-001 Phase 2: `extern` declarations do NOT register
# (spec § 2.2 row 1). The source includes a registering decl
# alongside so the per-decl skip rule is exercised (not the
# zero-qualifying-decls emit-nothing rule of spec § 4.3).
test('ncc_auto_gc_roots_extern_skipped', test_runner,
  args : [ncc_exe, 'preprocess_not_contains',
          test_dir / 'test_auto_gc_roots_extern_skipped.c',
          '(void *) & extern_singleton']
         + ncc_test_flags + ['--ncc-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_auto_gc_roots_extern_skipped_local_registered', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_auto_gc_roots_extern_skipped.c',
          '(void *) & local_root']
         + ncc_test_flags + ['--ncc-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

# WP-001 Phase 2: function pointers do NOT register
# (spec § 2.2 row 2).
test('ncc_auto_gc_roots_function_pointer_skipped', test_runner,
  args : [ncc_exe, 'preprocess_not_contains',
          test_dir / 'test_auto_gc_roots_function_pointer_skipped.c',
          '(void *) & fn_ptr']
         + ncc_test_flags + ['--ncc-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_auto_gc_roots_function_pointer_other_registered', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_auto_gc_roots_function_pointer_skipped.c',
          '(void *) & registered']
         + ncc_test_flags + ['--ncc-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

# WP-001 Phase 2: TU-scope decls whose name begins with `__ncc_`
# do NOT register (they belong to ncc-emitted static-object
# pipelines — static-image, once-guard, buflit, future ones — and
# are registered by their own machinery; spec § 2.2 row 3).
test('ncc_auto_gc_roots_ncc_emitted_skipped', test_runner,
  args : [ncc_exe, 'preprocess_not_contains',
          test_dir / 'test_auto_gc_roots_ncc_emitted_skipped.c',
          '(void *) & __ncc_marker_static']
         + ncc_test_flags + ['--ncc-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_auto_gc_roots_ncc_emitted_other_registered', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_auto_gc_roots_ncc_emitted_skipped.c',
          '(void *) & regular_static']
         + ncc_test_flags + ['--ncc-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

# WP-001 Phase 2: `const char *s = "literal"` does NOT register
# (spec § 2.2 row 3 — the one initializer-aware skip rule). A
# `const char *t = some_non_literal_expression;` DOES register.
# Both assertions are needed: the contains-check confirms the
# transform fires on this source; the not-contains-check confirms
# the per-decl skip rule.
test('ncc_auto_gc_roots_const_char_literal_skipped', test_runner,
  args : [ncc_exe, 'preprocess_not_contains',
          test_dir / 'test_auto_gc_roots_const_char_star_skipped.c',
          '(void *) & literal_str']
         + ncc_test_flags + ['--ncc-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_auto_gc_roots_const_char_non_literal_registered', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_auto_gc_roots_const_char_star_skipped.c',
          '(void *) & buffer_ref']
         + ncc_test_flags + ['--ncc-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

# WP-001 Phase 3: pointer-array TU-scope decls register as a SINGLE
# table entry with `num_words = N` (spec § 4.1, plan Phase 3 DoD #1).
test('ncc_auto_gc_roots_pointer_array_addr', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_auto_gc_roots_pointer_array.c',
          '(void *) & xs']
         + ncc_test_flags + ['--ncc-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_auto_gc_roots_pointer_array_num_words', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_auto_gc_roots_pointer_array.c',
          '.num_words = 8']
         + ncc_test_flags + ['--ncc-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

# WP-001 Phase 3: function-pointer arrays do NOT register
# (spec § 2.2 row 2 extended to arrays). The source carries a
# pointer-array decl alongside so the per-decl skip rule is the
# subject of the assertion, not the zero-decls emit-nothing rule.
test('ncc_auto_gc_roots_function_pointer_array_skipped', test_runner,
  args : [ncc_exe, 'preprocess_not_contains',
          test_dir / 'test_auto_gc_roots_function_pointer_array_skipped.c',
          '(void *) & fns']
         + ncc_test_flags + ['--ncc-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_auto_gc_roots_function_pointer_array_other_registered', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_auto_gc_roots_function_pointer_array_skipped.c',
          '(void *) & registered']
         + ncc_test_flags + ['--ncc-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

# WP-001 Phase 4: pointer-bearing structs emit ONE table entry per
# pointer-run (spec § 4.1). For a struct with two pointer fields
# separated by a non-pointer slot, the emit produces two entries
# (one per run), each addressed via `__builtin_offsetof(typeof(...))`.
test('ncc_auto_gc_roots_struct_with_pointers_field_a', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_auto_gc_roots_struct_with_pointers.c',
          '(void *) & (mixed).a']
         + ncc_test_flags + ['--ncc-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_auto_gc_roots_struct_with_pointers_field_c', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_auto_gc_roots_struct_with_pointers.c',
          '(void *) & (mixed).c']
         + ncc_test_flags + ['--ncc-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

# WP-001 Phase 4: nested struct fields are recognized; the emit's
# address-of operator targets the dotted path to the inner field.
# Note: ncc's grammar restricts `__builtin_offsetof`'s 2nd argument
# to a single identifier (c_ncc.bnf line 599), so we emit the
# equivalent `&(name).a.b.c` form instead — clang's address-of
# applied to a designated subobject expression yields the same
# byte address.
test('ncc_auto_gc_roots_nested_struct_offset', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_auto_gc_roots_nested_struct.c',
          '(void *) & (outer).in.p']
         + ncc_test_flags + ['--ncc-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

# Regression: static aggregate fields whose type comes from
# `_generic_struct` lowering must be walked after the concrete struct
# definition has been emitted. This is the shape used by embedded
# `n00b_list_t(...)` fields in Wax globals.
test('ncc_auto_gc_roots_nested_generic_struct', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_auto_gc_roots_nested_generic_struct.c',
          '(void *) & (g_endpoint).events.data']
         + ncc_test_flags + ['--ncc-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

# WP-001 Phase 4: adjacent pointer fields COALESCE into one entry
# with `num_words = 2` (spec § 4.1). The struct in the source has
# two pointer fields followed by an `int`; the emit must show one
# 2-word run, NOT two 1-word entries.
test('ncc_auto_gc_roots_pointer_run_coalescing_num_words', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_auto_gc_roots_pointer_run_coalescing.c',
          '.num_words = 2']
         + ncc_test_flags + ['--ncc-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_auto_gc_roots_pointer_run_coalescing_no_split', test_runner,
  args : [ncc_exe, 'preprocess_not_contains',
          test_dir / 'test_auto_gc_roots_pointer_run_coalescing.c',
          '(void *) & (pair).b']
         + ncc_test_flags + ['--ncc-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

# WP-001 Phase 5: `[[n00b::nomap]]` skips the decl silently (no
# table entry) but a sibling unannotated decl still registers.
test('ncc_auto_gc_roots_nomap_skipped', test_runner,
  args : [ncc_exe, 'preprocess_not_contains',
          test_dir / 'test_auto_gc_roots_nomap_attribute.c',
          '(void *) & legacy_singleton']
         + ncc_test_flags + ['--ncc-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_auto_gc_roots_nomap_sibling_registered', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_auto_gc_roots_nomap_attribute.c',
          '(void *) & registered_root']
         + ncc_test_flags + ['--ncc-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

# WP-001 Phase 5: trailing-position `[[n00b::nomap]]` (after the
# declarator, before the `;`) also skips the decl. Spec § 3.1.
test('ncc_auto_gc_roots_nomap_trailing_skipped', test_runner,
  args : [ncc_exe, 'preprocess_not_contains',
          test_dir / 'test_auto_gc_roots_nomap_attribute.c',
          '(void *) & trailing_nomap']
         + ncc_test_flags + ['--ncc-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

# WP-001 Phase 5: the `[[n00b::nomap]]` attribute is STRIPPED from
# the post-transform emit so clang never sees an unknown attribute.
test('ncc_auto_gc_roots_nomap_attribute_stripped', test_runner,
  args : [ncc_exe, 'preprocess_not_contains',
          test_dir / 'test_auto_gc_roots_nomap_attribute.c',
          'n00b :: nomap']
         + ncc_test_flags + ['--ncc-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

# WP-001 Phase 5: incomplete struct types trigger the D-009 warn-
# and-skip diagnostic (severity warning, build does not fail).
# Uses the new `preprocess_stderr_contains` runner mode (added in
# this dispatch) because none of the existing modes covers
# "exit 0 + stderr substring + stdout omits substring".
test('ncc_nullable_warn', test_runner,
  args : [ncc_exe, 'preprocess_stderr_contains',
          test_dir / 'test_nullable_warn.c',
          'possible null dereference of nullable \'p\'',
          '?']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps', '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_nullable_interproc', test_runner,
  args : [ncc_exe, 'preprocess_stderr_contains',
          test_dir / 'test_nullable_interproc.c',
          'to non-nullable parameter 1 of \'need\'',
          '?']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps', '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_nullable_paren', test_runner,
  args : [ncc_exe, 'preprocess_stderr_contains',
          test_dir / 'test_nullable_paren.c',
          'possible null dereference of nullable \'p\'',
          '?']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps', '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_nullable_nullmacro', test_runner,
  args : [ncc_exe, 'preprocess_stderr_contains',
          test_dir / 'test_nullable_nullmacro.c',
          'possible null dereference of nullable \'p\'',
          '?']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps', '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_nullable_bind', test_runner,
  args : [ncc_exe, 'preprocess_stderr_contains',
          test_dir / 'test_nullable_bind.c',
          'nullable result of \'mk()\' assigned to non-nullable',
          '?']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps', '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_nullable_guards', test_runner,
  args : [ncc_exe, 'preprocess_stderr_omits',
          test_dir / 'test_nullable_guards.c',
          'null dereference']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps', '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_nodiscard_warn', test_runner,
  args : [ncc_exe, 'preprocess_stderr_contains',
          test_dir / 'test_nodiscard_warn.c',
          'result of \'mk()\' is ignored',
          '_generic_struct']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps', '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_nodiscard_ok', test_runner,
  args : [ncc_exe, 'preprocess_stderr_omits',
          test_dir / 'test_nodiscard_ok.c',
          'is ignored']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps', '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_auto_gc_roots_incomplete_struct_warn', test_runner,
  args : [ncc_exe, 'preprocess_stderr_contains',
          test_dir / 'err_auto_gc_roots_incomplete_struct.c',
          'forward_obj',
          '(void *) & forward_obj']
         + ncc_test_flags + ['--ncc-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

# WP-003 / D-036 (F-2): thread_local / _Thread_local decls have
# per-thread storage; their address is not a compile-time constant,
# so a static `n00b_gc_root_t[]` table cannot embed `&var`. The
# transform must skip these decls (mirrors the `extern` skip).
test('ncc_auto_gc_roots_thread_local_c23_skipped', test_runner,
  args : [ncc_exe, 'preprocess_not_contains',
          test_dir / 'test_auto_gc_roots_thread_local_skipped.c',
          '(void *) & tls_singleton_c23']
         + ncc_test_flags + ['--ncc-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_auto_gc_roots_thread_local_c11_skipped', test_runner,
  args : [ncc_exe, 'preprocess_not_contains',
          test_dir / 'test_auto_gc_roots_thread_local_skipped.c',
          '(void *) & tls_singleton_c11']
         + ncc_test_flags + ['--ncc-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

# Anchor: ensure the plain decl in the same file IS registered so
# the assertions above pin the skip rule, not the zero-decls path.
test('ncc_auto_gc_roots_thread_local_plain_registered', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_auto_gc_roots_thread_local_skipped.c',
          '(void *) & plain_root']
         + ncc_test_flags + ['--ncc-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

# WP-003 / D-036 (F-3): `_Atomic(T *)` globals are a single opaque
# pointer-word root, not a struct whose fields can be walked. The
# transform must register one scalar entry (num_words = 1) and
# never descend into the wrapped type's fields. `_Atomic(scalar)`
# (non-pointer payload) must be skipped entirely.
test('ncc_auto_gc_roots_atomic_pointer_registered', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_auto_gc_roots_atomic_pointer_scalar.c',
          '(void *) & atomic_ptr']
         + ncc_test_flags + ['--ncc-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_auto_gc_roots_atomic_pointer_num_words', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_auto_gc_roots_atomic_pointer_scalar.c',
          '.num_words = 1']
         + ncc_test_flags + ['--ncc-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_auto_gc_roots_atomic_struct_member', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_auto_gc_roots_atomic_struct_member.c',
          '& (atomic_member_holder).head']
         + ncc_test_flags + ['--ncc-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_auto_gc_roots_atomic_struct_member_array_addr', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_auto_gc_roots_atomic_struct_member.c',
          '& (atomic_member_holder).tail']
         + ncc_test_flags + ['--ncc-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_auto_gc_roots_atomic_struct_member_array_num_words', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_auto_gc_roots_atomic_struct_member.c',
          '.num_words = 3']
         + ncc_test_flags + ['--ncc-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_auto_gc_roots_atomic_scalar_skipped', test_runner,
  args : [ncc_exe, 'preprocess_not_contains',
          test_dir / 'test_auto_gc_roots_atomic_pointer_scalar.c',
          '(void *) & atomic_scalar']
         + ncc_test_flags + ['--ncc-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_stack_maps_flag_accepted', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_gc_stack_maps_flag.c', 'n00b_gc_stack_push']
         + ncc_test_flags + ['--ncc-gc-stack-maps'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_stack_maps_root_discovery', test_runner,
  args : [ncc_exe, 'preprocess', test_dir / 'test_gc_stack_maps_roots.c']
         + ncc_test_flags + ['--ncc-gc-stack-maps'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_stack_maps_pointer_array_words', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_gc_stack_maps_roots.c', '.num_words = 3']
         + ncc_test_flags + ['--ncc-gc-stack-maps'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_stack_maps_aggregate_field_roots', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_gc_stack_maps_roots.c', '& box.nested.inner']
         + ncc_test_flags + ['--ncc-gc-stack-maps'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_stack_maps_aggregate_not_word_range', test_runner,
  args : [ncc_exe, 'preprocess_not_contains',
          test_dir / 'test_gc_stack_maps_roots.c', 'sizeof (box)']
         + ncc_test_flags + ['--ncc-gc-stack-maps'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_stack_maps_aggregate_array_field_roots', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_gc_stack_maps_roots.c', 'filters [0].ctx']
         + ncc_test_flags + ['--ncc-gc-stack-maps'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_stack_maps_aggregate_array_not_pointer_array', test_runner,
  args : [ncc_exe, 'preprocess_not_contains',
          test_dir / 'test_gc_stack_maps_roots.c', 'sizeof (filters)']
         + ncc_test_flags + ['--ncc-gc-stack-maps'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_stack_maps_flexible_member_field_root', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_gc_stack_maps_roots.c', '& flex.known']
         + ncc_test_flags + ['--ncc-gc-stack-maps'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_stack_maps_flexible_member_not_sized', test_runner,
  args : [ncc_exe, 'preprocess_not_contains',
          test_dir / 'test_gc_stack_maps_roots.c', 'flex.opaque']
         + ncc_test_flags + ['--ncc-gc-stack-maps'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_stack_maps_atomic_pointer_field', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_gc_stack_maps_roots.c', '& holder.store']
         + ncc_test_flags + ['--ncc-gc-stack-maps'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_stack_maps_atomic_not_pointee_fields', test_runner,
  args : [ncc_exe, 'preprocess_not_contains',
          test_dir / 'test_gc_stack_maps_roots.c', 'holder.store.next']
         + ncc_test_flags + ['--ncc-gc-stack-maps'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_stack_maps_atomic_aggregate_offset', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_gc_stack_maps_roots.c', '__builtin_offsetof']
         + ncc_test_flags + ['--ncc-gc-stack-maps'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_stack_maps_atomic_aggregate_not_member_access', test_runner,
  args : [ncc_exe, 'preprocess_not_contains',
          test_dir / 'test_gc_stack_maps_roots.c', 'atomic_store.next']
         + ncc_test_flags + ['--ncc-gc-stack-maps'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_stack_maps_cleanup_attr', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_gc_stack_maps_roots.c',
          'cleanup (n00b_gc_stack_pop)']
         + ncc_test_flags + ['--ncc-gc-stack-maps'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_stack_maps_scalar_array_not_root', test_runner,
  args : [ncc_exe, 'preprocess_not_contains',
          test_dir / 'test_gc_stack_maps_scalar_array.c', '& values']
         + ncc_test_flags + ['--ncc-gc-stack-maps'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_stack_maps_empty_body_param', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_gc_stack_maps_empty_body.c', 'n00b_gc_stack_push']
         + ncc_test_flags + ['--ncc-gc-stack-maps'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_stack_maps_function_pointer_name', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_gc_stack_maps_function_pointer.c', '& fn']
         + ncc_test_flags + ['--ncc-gc-stack-maps'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_stack_maps_qualified_pointer_name', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_gc_stack_maps_qualified_pointer.c', '& state']
         + ncc_test_flags + ['--ncc-gc-stack-maps'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_stack_maps_array_sizeof_bounds', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_gc_stack_maps_array_sizeof.c',
          'sizeof (vecs) / sizeof (void *)']
         + ncc_test_flags + ['--ncc-gc-stack-maps'],
  suite : 'ncc',
  depends : ncc_exe,
)

# D-033 positive case: VLA of a pointer-bearing struct must compile
# and emit a single conservative whole-buffer scan root.  We assert
# both that the runtime-sized num_words expression appears and that
# the address-of-element-zero is recorded as a root.
test('ncc_gc_stack_maps_vla_pointer_aggregate_num_words', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_gc_stack_maps_vla_pointer_aggregate.c',
          'sizeof (fields) / (sizeof (void *))']
         + ncc_test_flags + ['--ncc-gc-stack-maps'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_stack_maps_vla_pointer_aggregate_address', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_gc_stack_maps_vla_pointer_aggregate.c',
          '& fields [0]']
         + ncc_test_flags + ['--ncc-gc-stack-maps'],
  suite : 'ncc',
  depends : ncc_exe,
)

# D-033 negative case: VLA of a pointerless aggregate must NOT
# emit any scan address for the VLA -- there are no roots to track.
test('ncc_gc_stack_maps_vla_scalar_aggregate_no_root', test_runner,
  args : [ncc_exe, 'preprocess_not_contains',
          test_dir / 'test_gc_stack_maps_vla_scalar_aggregate.c',
          '& pairs [0]']
         + ncc_test_flags + ['--ncc-gc-stack-maps'],
  suite : 'ncc',
  depends : ncc_exe,
)

# D-033 regression: the runtime-bounded VLA num_words expression must
# NOT trigger clang's `-Wsizeof-array-div` warning, which fires when
# `sizeof(arr) / sizeof(void *)` is computed for an array whose element
# type is not `void *`.  The fix is parenthesizing the divisor:
# `sizeof(arr) / (sizeof(void *))`.  We verify by compiling the same
# pointer-aggregate VLA fixture with `-Werror=sizeof-array-div`; if the
# warning ever regresses, this test fails.
test('ncc_gc_stack_maps_vla_pointer_aggregate_no_sizeof_array_div',
  test_runner,
  args : [ncc_exe, 'compile_run',
          test_dir / 'test_gc_stack_maps_vla_pointer_aggregate_main.c']
         + ncc_test_flags
         + ['--ncc-gc-stack-maps',
            '-Wsizeof-array-div', '-Werror=sizeof-array-div'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_stack_maps_computed_goto_skipped', test_runner,
  args : [ncc_exe, 'preprocess_not_contains',
          test_dir / 'test_gc_stack_maps_computed_goto.c',
          'n00b_gc_stack_push']
         + ncc_test_flags + ['--ncc-gc-stack-maps'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_stack_maps_relaxed_for_init_skipped', test_runner,
  args : [ncc_exe, 'preprocess_not_contains',
          test_dir / 'err_gc_stack_maps_for_init.c',
          '& cursor']
         + ncc_test_flags + ['--ncc-gc-stack-maps-relaxed'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_stack_maps_system_header_skipped', test_runner,
  args : [ncc_exe, 'preprocess_not_contains',
          test_dir / 'test_gc_stack_maps_system_header.c',
          '.function_name = "__sputc"']
         + ncc_test_flags + ['--ncc-gc-stack-maps'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_stack_maps_system_header_user_function_kept', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_gc_stack_maps_system_header.c',
          '.function_name = "gc_stack_maps_system_header_fixture"']
         + ncc_test_flags + ['--ncc-gc-stack-maps'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_stack_maps_homebrew_header_skipped', test_runner,
  args : [ncc_exe, 'preprocess_not_contains',
          test_dir / 'test_gc_stack_maps_homebrew_header.c',
          '.function_name = "ncc_fake_homebrew_inline"']
         + ncc_test_flags + ['--ncc-gc-stack-maps'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_stack_maps_thread_runtime_skipped', test_runner,
  args : [ncc_exe, 'preprocess_not_contains',
          test_dir / 'test_gc_stack_maps_thread_runtime_skip.c',
          'n00b_gc_stack_push']
         + ncc_test_flags + ['--ncc-gc-stack-maps'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_stack_maps_setjmp_diagnostic', test_runner,
  args : [ncc_exe, 'expect_error_contains',
          test_dir / 'err_gc_stack_maps_setjmp.c',
          'unsafe non-local exit call']
         + ncc_test_flags + ['--ncc-gc-stack-maps'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_stack_maps_setjmp_spoof_diagnostic', test_runner,
  args : [ncc_exe, 'expect_error_contains',
          test_dir / 'err_gc_stack_maps_setjmp_spoof.c',
          'unsafe non-local exit call']
         + ncc_test_flags + ['--ncc-gc-stack-maps'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_stack_maps_longjmp_diagnostic', test_runner,
  args : [ncc_exe, 'expect_error_contains',
          test_dir / 'err_gc_stack_maps_longjmp.c',
          'unsafe non-local exit call']
         + ncc_test_flags + ['--ncc-gc-stack-maps'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_stack_maps_stw_macro_allowed', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_gc_stack_maps_stw_macro.c',
          'n00b_gc_stack_push']
         + ncc_test_flags + ['--ncc-gc-stack-maps'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_stack_maps_nonlocal_api_allowed', test_runner,
  args : [ncc_exe, 'preprocess_contains',
          test_dir / 'test_gc_stack_maps_nonlocal_api.c',
          'n00b_gc_stack_push']
         + ncc_test_flags + ['--ncc-gc-stack-maps'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_stack_maps_nonlocal_runtime_skipped', test_runner,
  args : [ncc_exe, 'preprocess_not_contains',
          test_dir / 'test_gc_stack_maps_nonlocal_runtime_skip.c',
          'n00b_gc_stack_push']
         + ncc_test_flags + ['--ncc-gc-stack-maps'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_stack_maps_collect_runtime_skipped', test_runner,
  args : [ncc_exe, 'preprocess_not_contains',
          test_dir / 'test_gc_stack_maps_collect_skip.c',
          'n00b_gc_stack_push']
         + ncc_test_flags + ['--ncc-gc-stack-maps'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_stack_maps_runtime_compile', test_runner,
  args : [ncc_exe, 'compile_run', test_dir / 'test_gc_stack_maps_runtime.c']
         + ncc_test_flags + ['--ncc-gc-stack-maps'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_gc_stack_maps_control_flow', test_runner,
  args : [ncc_exe, 'compile_run',
          test_dir / 'test_gc_stack_maps_control_flow.c']
         + ncc_test_flags + ['--ncc-gc-stack-maps', '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_force_include_not_replayed', test_runner,
  args : [ncc_exe, 'compile_run', test_dir / 'test_force_include_once.c']
         + ncc_test_flags + ['-include', test_dir / 'force_include_once.h',
                              '--ncc-no-gc-stack-maps'],
  suite : 'ncc',
  depends : ncc_exe,
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

test('ncc_crt_entry_default_no_inject', test_runner,
  args : [ncc_exe, 'custom_entry_default_no_inject',
          test_dir / 'test_crt_entry_smoke.c']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps',
                             '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_crt_entry_compile_only_no_inject', test_runner,
  args : [ncc_exe, 'custom_entry_compile_only_no_inject',
          test_dir / 'test_crt_entry_smoke.c']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps',
                             '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_crt_entry_assemble_only_no_inject', test_runner,
  args : [ncc_exe, 'custom_entry_assemble_only_no_inject',
          test_dir / 'test_crt_entry_smoke.c']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps',
                             '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_crt_entry_syntax_only_no_inject', test_runner,
  args : [ncc_exe, 'custom_entry_syntax_only_no_inject',
          test_dir / 'test_crt_entry_smoke.c']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps',
                             '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_crt_entry_dep_only_no_inject', test_runner,
  args : [ncc_exe, 'custom_entry_dep_only_no_inject',
          test_dir / 'test_crt_entry_smoke.c']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps',
                             '--ncc-no-auto-gc-roots',
                             '--ncc-constexpr-include',
                             'ncc-passthrough-marker'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_crt_entry_smoke', test_runner,
  args : [ncc_exe, 'custom_entry_run',
          test_dir / 'test_crt_entry_smoke.c']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps',
                             '--ncc-no-auto-gc-roots'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_crt_entry_object_smoke', test_runner,
  args : [ncc_exe, 'custom_entry_object_run',
          test_dir / 'test_crt_entry_smoke.c']
         + ncc_test_flags + ['--ncc-no-gc-stack-maps',
                             '--ncc-no-auto-gc-roots',
                             '--ncc-constexpr-include',
                             'ncc-object-link-marker'],
  suite : 'ncc',
  depends : ncc_exe,
)

test('ncc_compiler_temp_source', test_runner,
  args : [ncc_exe, 'compiler_temp_source', test_dir / 'test_bang.c']
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
  'err_rpc_void_return':         test_dir / 'err_rpc_void_return.c',
  'err_rpc_no_ctx':              test_dir / 'err_rpc_no_ctx.c',
  'err_rpc_bad_method':          test_dir / 'err_rpc_bad_method.c',
  'err_rpc_with_kargs':          test_dir / 'err_rpc_with_kargs.c',
  'err_rpc_dup_method':          test_dir / 'err_rpc_dup_method.c',
}

foreach name, src : ncc_error_tests
  test('ncc_' + name, test_runner,
    args : [ncc_exe, 'expect_error', src],
    suite : 'ncc',
    depends : ncc_exe,
  )
endforeach

ncc_error_contains_tests = {
  'err_array_literal_local_mutable': [
    test_dir / 'err_array_literal_local_mutable.c',
    'non-const local array literal',
  ],
  'err_array_literal_nested_mismatch': [
    test_dir / 'err_array_literal_nested_mismatch.c',
    'must be an array literal like',
  ],
  'err_array_literal_non_array_target': [
    test_dir / 'err_array_literal_non_array_target.c',
    'requires an ncc_array_t(T) or n00b_array_t(T) object target',
  ],
  'err_array_literal_explicit_non_array_target': [
    test_dir / 'err_array_literal_explicit_non_array_target.c',
    'requires an ncc_array_t(T) or n00b_array_t(T) object target',
  ],
  'err_array_literal_pointer_target': [
    test_dir / 'err_array_literal_pointer_target.c',
    'must target an array object, not a pointer',
  ],
  'err_array_literal_rstr_type': [
    test_dir / 'err_array_literal_rstr_type.c',
    'r-string array element requires element type',
  ],
  'err_array_literal_scalar_nested': [
    test_dir / 'err_array_literal_scalar_nested.c',
    'is scalar, so nested array literal',
  ],
  'err_array_literal_statement': [
    test_dir / 'err_array_literal_statement.c',
    'array literals are currently supported only',
  ],
  'err_array_literal_unsupported_elem': [
    test_dir / 'err_array_literal_unsupported_elem.c',
    'is not supported for static initialization yet',
  ],
  'err_function_contract_declaration': [
    test_dir / 'err_function_contract_declaration.c',
    'parse FAILED',
  ],
  'err_function_contract_once': [
    test_dir / 'err_function_contract_once.c',
    'functions with contracts are not supported yet',
  ],
  'err_function_contract_once_extension': [
    test_dir / 'err_function_contract_once_extension.c',
    'functions with contracts are not supported yet',
  ],
  'err_function_contract_rpc': [
    test_dir / 'err_function_contract_rpc.c',
    '@rpc cannot be combined with function contracts',
  ],
  'err_function_contract_rpc_extension': [
    test_dir / 'err_function_contract_rpc_extension.c',
    '@rpc cannot be combined with function contracts',
  ],
  'err_function_contract_result_requires': [
    test_dir / 'err_function_contract_result_requires.c',
    'result is only available in non-void ensures',
  ],
  'err_function_contract_result_void': [
    test_dir / 'err_function_contract_result_void.c',
    'result is only available in non-void ensures',
  ],
  'err_function_contract_call': [
    test_dir / 'err_function_contract_call.c',
    'function calls are not allowed in function contracts',
  ],
  'err_function_contract_old_requires': [
    test_dir / 'err_function_contract_old_requires.c',
    'old(...) is not allowed in a \'requires\' clause',
  ],
  'err_defer_goto': [
    test_dir / 'err_defer_goto.c',
    'may not (yet) also use goto',
  ],
  'err_defer_escape': [
    test_dir / 'err_defer_escape.c',
    'control transfer (return/break/continue/goto) inside a _defer body',
  ],
  'err_try_outside_function': [
    test_dir / 'err_try_outside_function.c',
    '_try is only valid inside a function',
  ],
  'err_function_contract_return': [
    test_dir / 'err_function_contract_return.c',
    'return is not allowed in function contracts',
  ],
  'err_function_contract_goto': [
    test_dir / 'err_function_contract_goto.c',
    'goto is not allowed in function contracts',
  ],
  'err_function_contract_mutates_param': [
    test_dir / 'err_function_contract_mutates_param.c',
    'contract may mutate only contract-local variables',
  ],
  'err_function_contract_mutates_param_before_local': [
    test_dir / 'err_function_contract_mutates_param_before_local.c',
    'contract may mutate only contract-local variables',
  ],
  'err_function_contract_mutates_param_after_for_local': [
    test_dir / 'err_function_contract_mutates_param_after_for_local.c',
    'contract may mutate only contract-local variables',
  ],
  'err_function_contract_mutates_global': [
    test_dir / 'err_function_contract_mutates_global.c',
    'contract may mutate only contract-local variables',
  ],
  'err_function_contract_mutates_pointee': [
    test_dir / 'err_function_contract_mutates_pointee.c',
    'contract may mutate only contract-local variables',
  ],
  'err_function_contract_mutates_member': [
    test_dir / 'err_function_contract_mutates_member.c',
    'contract may mutate only contract-local variables',
  ],
  'err_function_contract_mutates_array': [
    test_dir / 'err_function_contract_mutates_array.c',
    'contract may mutate only contract-local variables',
  ],
  'err_function_contract_result_shadow': [
    test_dir / 'err_function_contract_result_shadow.c',
    'result may not be declared in function contracts',
  ],
  'err_array_literal_union_pointer': [
    test_dir / 'err_array_literal_union_pointer.c',
    'precise active-member static layout is not supported yet',
  ],
  'err_array_literal_transient_field': [
    test_dir / 'err_array_literal_transient_field.c',
    'transient type',
  ],
  'err_array_literal_field_bound_expr': [
    test_dir / 'err_array_literal_field_bound_expr.c',
    'non-numeric bounds',
  ],
  'err_array_literal_missing_field_policy': [
    test_dir / 'err_array_literal_missing_field_policy.c',
    'no static-layout policy',
  ],
  'err_list_literal_non_list_target': [
    test_dir / 'err_list_literal_non_list_target.c',
    'list literal initializer',
  ],
  'err_list_literal_local_mutable': [
    test_dir / 'err_list_literal_local_mutable.c',
    'non-const local list literal',
  ],
  'err_list_literal_bstr_type': [
    test_dir / 'err_list_literal_bstr_type.c',
    'b-string list element requires element type',
  ],
  'err_dict_literal_non_dict_target': [
    test_dir / 'err_dict_literal_non_dict_target.c',
    'dict literal initializer',
  ],
  'err_dict_literal_bare_in_non_dict_target': [
    test_dir / 'err_dict_literal_bare_in_non_dict_target.c',
    'dict literal initializer',
  ],
  'err_dict_literal_local_mutable': [
    test_dir / 'err_dict_literal_local_mutable.c',
    'non-const local dict literal',
  ],
  # WP-007 migrated dict roots now intercept file-scope writable
  # struct-pointer-keyed dicts before legacy helper fallback.
  'err_dict_literal_struct_pointer_key': [
    test_dir / 'err_dict_literal_struct_pointer_key.c',
    'currently supports scalar, r-string, and buffer key types only',
  ],
  # Phase 3c.iii: non-pointer-non-scalar key types (e.g. `float`)
  # land in DICT_KEY_KIND_UNSUPPORTED and emit a friendly
  # "dict literal key type ... is not supported" diagnostic.
  'err_dict_literal_unsupported_key_type': [
    test_dir / 'err_dict_literal_unsupported_key_type.c',
    'dict literal key type',
  ],
  # Phase 3c.iii: value types outside the scalar / pointer /
  # registered-aggregate / nested-container set (e.g.
  # `_Complex double`) emit a friendly "dict literal value type
  # ... is not supported" diagnostic.
  'err_dict_literal_unsupported_value_type': [
    test_dir / 'err_dict_literal_unsupported_value_type.c',
    'dict literal value type',
  ],
  # Phase 3c.i duplicate-key compile-time rejection (D-065).
  'err_dict_literal_duplicate_key': [
    test_dir / 'err_dict_literal_duplicate_key.c',
    'duplicate key',
  ],
  'err_buffer_literal_target': [
    test_dir / 'err_buffer_literal_target.c',
    'buffer literal initializer must target n00b_buffer_t *',
  ],
  'err_gc_stack_maps_unnamed_param': [
    test_dir / 'err_gc_stack_maps_unnamed_param.c',
    'pointer or aggregate parameter has no name',
    '--ncc-gc-stack-maps',
  ],
  'err_gc_stack_maps_vla': [
    test_dir / 'err_gc_stack_maps_vla.c',
    'variable-length or incomplete array',
    '--ncc-gc-stack-maps',
  ],
  'err_gc_stack_maps_for_init': [
    test_dir / 'err_gc_stack_maps_for_init.c',
    'unsupported statement context',
    '--ncc-gc-stack-maps',
  ],
}

foreach name, params : ncc_error_contains_tests
  extra_args = []
  if params.length() > 2
    extra_args = [params[2]]
  endif
  test('ncc_' + name, test_runner,
    args : [ncc_exe, 'expect_error_contains', params[0], params[1]]
           + ncc_test_flags + extra_args,
    suite : 'ncc',
    depends : ncc_exe,
  )
endforeach
