#pragma once

#include <stdbool.h>

// Parsed command-line state shared by the ncc driver and small internal helpers.
typedef struct {
    const char  *input_file;
    const char  *output_file;
    const char  *compiler;
    const char  *constexpr_headers; // comma-separated <...> or "..." headers

    bool         has_E;
    bool         has_c;
    bool         has_S;
    bool         has_fsyntax_only;
    bool         has_dep_only;
    bool         has_std;       // user specified -std=
    bool         no_ncc;
    bool         ncc_help;
    bool         dump_tokens;
    bool         dump_tree;
    bool         dump_tree_raw;
    bool         dump_output;
    bool         gc_stack_maps;
    bool         gc_stack_maps_relaxed;
    bool         gc_typemaps;
    bool         auto_gc_roots;
    bool         custom_entry;
    bool         no_comptime;
    bool         allow_unions;    // suppress the traditional-union deprecation
    bool         error_on_union;  // escalate the deprecation to a hard error
    bool         static_identity_generate_namespace;

    // Dependency file generation (handled separately from compilation).
    bool         has_dep_flags; // -MD or -MMD present
    bool         dep_mmmd;      // -MMD (user headers only) vs -MD (all)
    const char  *dep_mf;        // -MF <file>
    const char  *dep_mq;        // -MQ <target>
    const char  *dep_mt;        // -MT <target>

    // rstr template overrides (CLI > meson define > default).
    const char  *rstr_template_styled;
    const char  *rstr_template_plain;
    const char  *rstr_static_ref_template_styled;
    const char  *rstr_static_ref_template_plain;
    const char  *rstr_static_ref_expr_styled;
    const char  *rstr_static_ref_expr_plain;
    const char  *array_literal_data_template;
    const char  *array_literal_data_expr;
    const char  *static_object_entry_attr;

    // vargs/once/rstr overrides (CLI > meson define > default).
    const char  *vargs_type;
    const char  *once_prefix;
    const char  *rstr_string_type;
    const char  *rstr_text_style_type;
    const char  *rstr_style_record_type;

    // Grammar file override (CLI > env > embedded).
    const char  *grammar_file;

    // Flags to pass through to clang.
    const char **clang_args;
    int          n_clang_args;
    int          clang_args_cap;

    // Arguments passed to comptime_main after synthesized argv[0].
    const char **comptime_args;
    int          n_comptime_args;
    int          comptime_args_cap;
} ncc_opts_t;
