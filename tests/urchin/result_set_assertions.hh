/*
 * Copyright 2015 Cloudius Systems
 */

#pragma once

#include <map>

#include "query-result-set.hh"

//
// Contains assertions for query::result_set objects
//
// Example use:
//
//  assert_that(rs)
//     .has(a_row().with_column("column_name", "value"));
//

class row_assertion {
    std::map<bytes, boost::any> _expected_values;
public:
    row_assertion& with_column(bytes name, boost::any value) {
        _expected_values.emplace(name, value);
        return *this;
    }
private:
    friend class result_set_assertions;
    bool matches(const query::result_set_row& row) const;
    sstring describe(schema_ptr s) const;
};

inline
row_assertion a_row() {
    return {};
}

class result_set_assertions {
    const query::result_set& _rs;
public:
    result_set_assertions(const query::result_set& rs) : _rs(rs) { }
    const result_set_assertions& has(const row_assertion& ra) const;
    const result_set_assertions& has_only(const row_assertion& ra) const;
    const result_set_assertions& is_empty() const;
};

// Make rs live as long as the returned assertion object is used
inline
result_set_assertions assert_that(const query::result_set& rs) {
    return { rs };
}
