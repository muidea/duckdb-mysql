#pragma once
#include "duckdb.hpp"

namespace duckdb {
// Explicit optional output; old table-function callers keep their original ABI.
struct GuardedSourceError {
    string product, state, code, message, detail, hint, schema, table, column, constraint;
    static string Quote(const string &value) {
        static const char hex[] = "0123456789abcdef";
        string result = "\"";
        for (auto byte : value) {
            auto ch = static_cast<unsigned char>(byte);
            if (ch == '"' || ch == '\\') { result += '\\'; result += ch; }
            else if (ch < 32) { result += "\\u00"; result += hex[ch >> 4]; result += hex[ch & 15]; }
            else result += ch;
        }
        return result + "\"";
    }
    string JSON() const {
        if (message.empty()) return "";
        return "{\"version\":1,\"product\":" + Quote(product) + ",\"state\":" + Quote(state) +
            ",\"code\":" + Quote(code) + ",\"message\":" + Quote(message) + ",\"detail\":" + Quote(detail) +
            ",\"hint\":" + Quote(hint) + ",\"schema\":" + Quote(schema) + ",\"table\":" + Quote(table) +
            ",\"column\":" + Quote(column) + ",\"constraint\":" + Quote(constraint) + "}";
    }
};
}
