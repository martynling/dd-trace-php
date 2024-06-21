// Unless explicitly stated otherwise all files in this repository are
// dual-licensed under the Apache-2.0 License or BSD-3-Clause License.
//
// This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2021 Datadog, Inc.
#include "backtrace.h"
#include "configuration.h"
#include "ddtrace.h"
#include "logging.h"
#include "php_compat.h"
#include "php_objects.h"
#include "string_helpers.h"

static const int MAX_FRAMES_ALLOWED = 32;
static const int NO_LIMIT = 0;

static zend_string *_dd_stack_key;
static zend_string *_frame_line;
static zend_string *_frame_function;
static zend_string *_frame_file;
static zend_string *_frame_id;

bool php_backtrace_frame_to_datadog_backtrace_frame( // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    zval *php_backtrace_frame, zval *datadog_backtrace_frame, int index)
{
    if (Z_TYPE_P(php_backtrace_frame) != IS_ARRAY) {
        return false;
    }
    HashTable *frame = Z_ARRVAL_P(php_backtrace_frame);
    zval *line = zend_hash_str_find(frame, "line", sizeof("line") - 1);
    zval *function =
        zend_hash_str_find(frame, "function", sizeof("function") - 1);
    zval *file = zend_hash_str_find(frame, "file", sizeof("file") - 1);
    zval id;
    ZVAL_LONG(&id, index);

#ifdef TESTING
    // In order to be able to test full path encoded everywhere lets set
    // only the file name without path
    char *file_name = strrchr(Z_STRVAL_P(file), '/');
    Z_TRY_DELREF_P(file);
    ZVAL_STRINGL(file, file_name + 1, strlen(file_name) - 1);
#endif

    array_init(datadog_backtrace_frame);
    HashTable *datadog_backtrace_frame_ht = Z_ARRVAL_P(datadog_backtrace_frame);
    zend_hash_add(datadog_backtrace_frame_ht, _frame_line, line);
    zend_hash_add(datadog_backtrace_frame_ht, _frame_function, function);
    zend_hash_add(datadog_backtrace_frame_ht, _frame_file, file);
    zend_hash_add(datadog_backtrace_frame_ht, _frame_id, &id);

    Z_TRY_ADDREF_P(function);
    Z_TRY_ADDREF_P(file);

    return true;
}

void php_backtrace_to_datadog_backtrace(
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    zval *php_backtrace, zval *datadog_backtrace)
{
    if (Z_TYPE_P(php_backtrace) != IS_ARRAY) {
        return;
    }

    int max_frames = get_global_DD_APPSEC_MAX_STACK_TRACE_DEPTH();
    HashTable *php_backtrace_ht = Z_ARRVAL_P(php_backtrace);
    int frames_on_stack = zend_array_count(php_backtrace_ht);

    int top = MIN(max_frames, MAX_FRAMES_ALLOWED);
    int bottom = 0;
    UNUSED(bottom);
    if (frames_on_stack > MAX_FRAMES_ALLOWED && top == MAX_FRAMES_ALLOWED) {
        top = 8;
        bottom = 24;
    }

    array_init(datadog_backtrace);

    HashTable *datadog_backtrace_ht = Z_ARRVAL_P(datadog_backtrace);

    zval *tmp;
    zend_ulong index;
    ZEND_HASH_FOREACH_NUM_KEY_VAL(php_backtrace_ht, index, tmp)
    {
        zval new_frame;

        if (!php_backtrace_frame_to_datadog_backtrace_frame(
                tmp, &new_frame, index)) {
            continue;
        }

        zend_hash_next_index_insert_new(datadog_backtrace_ht, &new_frame);
        if (--top == 0) {
            break;
        }
    }
    ZEND_HASH_FOREACH_END();

    if (bottom > 0) {
        int position = frames_on_stack - bottom;
        ZEND_HASH_FOREACH_FROM(php_backtrace_ht, 0, position)
        {
            int index = __h;
            tmp = _z;
            zval new_frame;

            if (!php_backtrace_frame_to_datadog_backtrace_frame(
                    tmp, &new_frame, index)) {
                continue;
            }

            zend_hash_next_index_insert_new(datadog_backtrace_ht, &new_frame);
        }
        ZEND_HASH_FOREACH_END();
    }
}

void generate_backtrace(zval *result)
{
    if (!get_global_DD_APPSEC_STACK_TRACE_ENABLED()) {
        array_init(result);
        return;
    }

    zval php_backtrace;
    zend_fetch_debug_backtrace(
        &php_backtrace, 1, DEBUG_BACKTRACE_IGNORE_ARGS, NO_LIMIT);

    php_backtrace_to_datadog_backtrace(&php_backtrace, result);

    zval_dtor(&php_backtrace);
}

static PHP_FUNCTION(datadog_appsec_testing_generate_backtrace)
{
    if (zend_parse_parameters_none() == FAILURE) {
        return;
    }

    generate_backtrace(return_value);
}

static PHP_FUNCTION(datadog_appsec_testing_report_backtrace)
{
    if (zend_parse_parameters_none() == FAILURE) {
        RETURN_FALSE;
    }

    zval backtrace;
    generate_backtrace(&backtrace);

    add_entry_to_meta_struct(_dd_stack_key, &backtrace);

    RETURN_TRUE;
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(
    void_ret_bool_arginfo, 0, 0, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(
    void_ret_array_arginfo, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

// clang-format off
static const zend_function_entry testing_functions[] = {
    ZEND_RAW_FENTRY(DD_TESTING_NS "generate_backtrace", PHP_FN(datadog_appsec_testing_generate_backtrace), void_ret_array_arginfo,0)
    ZEND_RAW_FENTRY(DD_TESTING_NS "report_backtrace", PHP_FN(datadog_appsec_testing_report_backtrace), void_ret_bool_arginfo, 0)
    PHP_FE_END
};
// clang-format on

static void _register_testing_objects()
{
    if (!get_global_DD_APPSEC_TESTING()) {
        return;
    }

    dd_phpobj_reg_funcs(testing_functions);
}

void dd_backtrace_startup()
{
    _dd_stack_key =
        zend_string_init_interned("_dd.stack", sizeof("_dd.stack") - 1, 1);
    _frame_line = zend_string_init_interned("line", sizeof("line") - 1, 1);
    _frame_function =
        zend_string_init_interned("function", sizeof("function") - 1, 1);
    _frame_file = zend_string_init_interned("file", sizeof("file") - 1, 1);
    _frame_id = zend_string_init_interned("id", sizeof("id") - 1, 1);
#ifdef TESTING
    _register_testing_objects();
#endif
}
