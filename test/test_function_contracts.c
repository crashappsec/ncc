static int
positive_input(int x)
    requires { x > 0; }
{
    return x + 1;
}

static int
positive_output(int x)
    ensures { result > x; }
{
    return x + 2;
}

static int
bounded_increment(int x)
    requires { x >= 0; x < 90; }
    ensures { result >= x; result < 100; }
{
    return x + 3;
}

static void
record_value(int *out, int value)
    requires { out != nullptr; }
    ensures { *out == value; }
{
    *out = value;
}

static int
with_keyword_bias(int x)
    _kargs { int bias = 2; }
    requires { x >= 0; bias >= 0; }
    ensures { result == x + bias; result >= x; }
{
    return x + bias;
}

static int
double_value(int x)
{
    return x * 2;
}

static int
call_checked(int (*fn)(int), int x)
    requires { fn != nullptr; x > 0; }
    ensures { result > x; }
{
    return fn(x);
}

static int
first_value(int values[3])
    requires { values != nullptr; values[0] >= 0; }
    ensures { result == values[0]; }
{
    return values[0];
}

static int
support_declaration_contract(int x)
    requires {
        int y = x + 1;
        y > x;
    }
    ensures {
        int delta = result - x;
        delta == 2;
    }
{
    return x + 2;
}

static int
support_loop_contract(int x)
    requires {
        int seen = 0;
        for (int i = 0; i < x; i++) {
            seen++;
        }
        seen == x;
    }
    ensures {
        int seen = 0;
        while (seen < result - x) {
            seen++;
        }
        seen == 3;
    }
{
    return x + 3;
}

static int
contract_local_mutation(int x)
    requires {
        int acc = 0;
        if (x > 0) {
            acc = x;
            acc += 3;
            ++acc;
            acc--;
        }
        acc == x + 3;
    }
    ensures {
        int delta = 0;
        for (int i = x; i < result; i++) {
            delta++;
        }
        delta == 4;
    }
{
    return x + 4;
}

static int
plain_result_identifier(int result)
{
    int local_result = result + 1;
    return local_result;
}

struct result_field_contract {
    int result;
};

static int
field_result_identifier(struct result_field_contract item)
    requires { item.result > 0; }
    ensures { result > item.result; }
{
    return item.result + 1;
}

static int
parenthesized_and_cast_contract(int x)
    requires { (((x))) > 0; ((long)x) > 0; }
    ensures { ((int)result) == x + 1; }
{
    return x + 1;
}

static int keyword_default_counter = 0;

static int
next_keyword_default(void)
{
    keyword_default_counter++;
    return 40 + keyword_default_counter;
}

static int
single_keyword_default(int x)
    _kargs { int bias = next_keyword_default(); }
    requires { bias > 0; }
    ensures { result == x + bias; }
{
    return x + bias;
}

static int statement_keyword_default_counter = 0;

static int
statement_keyword_default(int x)
    _kargs { int bias = ({ statement_keyword_default_counter++; 7; }); }
    requires { bias > 0; }
    ensures { result == x + bias; }
{
    return x + bias;
}

int
main(void)
{
    if (positive_input(10) != 11) {
        return 1;
    }

    if (positive_output(20) != 22) {
        return 2;
    }

    if (bounded_increment(30) != 33) {
        return 3;
    }

    int value = 0;
    record_value(&value, 44);
    if (value != 44) {
        return 4;
    }

    if (with_keyword_bias(10) != 12) {
        return 5;
    }

    if (with_keyword_bias(10, .bias = 5) != 15) {
        return 6;
    }

    if (call_checked(double_value, 4) != 8) {
        return 7;
    }

    int values[3] = {8, 13, 21};
    if (first_value(values) != 8) {
        return 8;
    }

    if (support_declaration_contract(20) != 22) {
        return 9;
    }

    if (support_loop_contract(7) != 10) {
        return 10;
    }

    if (contract_local_mutation(9) != 13) {
        return 11;
    }

    if (plain_result_identifier(20) != 21) {
        return 12;
    }

    struct result_field_contract item = {.result = 30};
    if (field_result_identifier(item) != 31) {
        return 13;
    }

    if (parenthesized_and_cast_contract(41) != 42) {
        return 14;
    }

    keyword_default_counter = 0;
    if (single_keyword_default(5) != 46) {
        return 15;
    }
    if (keyword_default_counter != 1) {
        return 16;
    }

    keyword_default_counter = 0;
    if (single_keyword_default(5, .bias = 9) != 14) {
        return 17;
    }
    if (keyword_default_counter != 0) {
        return 18;
    }

    statement_keyword_default_counter = 0;
    if (statement_keyword_default(5) != 12) {
        return 19;
    }
    if (statement_keyword_default_counter != 1) {
        return 20;
    }

    statement_keyword_default_counter = 0;
    if (statement_keyword_default(5, .bias = 11) != 16) {
        return 21;
    }
    if (statement_keyword_default_counter != 0) {
        return 22;
    }

    return 0;
}
