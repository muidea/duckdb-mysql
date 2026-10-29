#include "mysql_types.hpp"
#include <iostream>
#include <stdexcept>

using namespace duckdb;
static void Require(bool value) {
	if (!value) {
		throw std::runtime_error("decimal type precision assertion");
	}
}
int main() {
	MySQLTypeConfig config;
	for (int precision : {1, 18, 38, 39, 40, 65}) {
		for (int scale : {0, 1, 30}) {
			if (scale > precision) {
				continue;
			}
			for (bool is_unsigned : {false, true}) {
				MySQLTypeData column {"decimal", "decimal", precision, scale};
				auto catalog_type = MySQLTypes::TypeToLogicalType(config, column);
				MYSQL_FIELD field {};
				field.type = MYSQL_TYPE_NEWDECIMAL;
				field.decimals = scale;
				field.flags = NUM_FLAG | (is_unsigned ? UNSIGNED_FLAG : 0);
				field.length = precision + (is_unsigned ? 0 : 1) + (scale ? 1 : 0);
				auto query_type = MySQLTypes::FieldToLogicalType(config, &field);
				Require(catalog_type == query_type);
				if (precision <= 38) {
					Require(query_type.id() == LogicalTypeId::DECIMAL);
					Require(DecimalType::GetWidth(query_type) == precision && DecimalType::GetScale(query_type) == scale);
				} else {
					Require(query_type.id() == LogicalTypeId::VARCHAR);
					Require(query_type.GetAlias() == "MYSQL_DECIMAL(" + std::to_string(precision) + "," + std::to_string(scale) + ")");
					Require(StringType::GetCollation(query_type) == "mysql_exact_decimal");
				}
			}
		}
	}
	std::cout << "decimal catalog/query precision tests passed\n";
	auto decode = [&](const LogicalType &type, const string &text, enum_field_types mysql_type = MYSQL_TYPE_VAR_STRING) {
		return MySQLTypes::DecodeTextValue(config, type, mysql_type, text.data(), text.size());
	};
	Require(decode(LogicalType::BIGINT, "9007199254740993").GetValue<int64_t>() == 9007199254740993LL);
	Require(decode(LogicalType::UBIGINT, "18446744073709551615").ToString() == "18446744073709551615");
	Require(decode(LogicalType::VARCHAR, "001").ToString() == "001");
	Require(decode(LogicalType::VARCHAR, "1").type() == LogicalType::VARCHAR);
	Require(decode(LogicalType::VARCHAR, "-838:59:59.123456", MYSQL_TYPE_TIME).ToString() == "-838:59:59.123456");
	Require(decode(LogicalType::VARCHAR, "").ToString().empty());
	Require(decode(LogicalType::SMALLINT, "-32768").GetValue<int16_t>() == -32768);
	Require(decode(LogicalType::BIGINT, "-9223372036854775808").ToString() == "-9223372036854775808");
	const string wide = "12345678901234567890123456789012345.123456789012345678901234567890";
	auto wide_type = MySQLTypes::TypeToLogicalType(config, {"decimal", "decimal(65,30)", 65, 30});
	Require(decode(wide_type, wide, MYSQL_TYPE_NEWDECIMAL).ToString() == wide);
	Require(decode(LogicalType::DECIMAL(38, 2), "123456789012345678901234567890123456.78").ToString() ==
	        "123456789012345678901234567890123456.78");
	string binary("\0\xff\\\0", 4);
	Require(StringValue::Get(decode(LogicalType::BLOB, binary)) == binary);
	Require(MySQLTypes::DecodeTextValue(config, LogicalType::INTEGER, MYSQL_TYPE_LONG, nullptr, 0).IsNull());
	Require(decode(LogicalType::DATE, "0000-00-00", MYSQL_TYPE_DATE).IsNull());
	Require(decode(LogicalType::TIMESTAMP, "2026-10-29 01:02:03.123456", MYSQL_TYPE_DATETIME).ToString() ==
	        "2026-10-29 01:02:03.123456");
	Require(decode(LogicalType::BOOLEAN, string(1, '\1'), MYSQL_TYPE_BIT).GetValue<bool>());
	Require(!decode(LogicalType::BOOLEAN, string(1, '\0'), MYSQL_TYPE_BIT).GetValue<bool>());
	Require(decode(LogicalType::TIMESTAMP, "0000-00-00 00:00:00.000000", MYSQL_TYPE_DATETIME).IsNull());
	config.incomplete_dates_as_nulls = true;
	Require(decode(LogicalType::DATE, "2026-00-29", MYSQL_TYPE_DATE).IsNull());
	config.incomplete_dates_as_nulls = false;
	MYSQL_FIELD integer_field {};
	integer_field.type = MYSQL_TYPE_LONG;
	Require(MySQLTypes::FieldToLogicalType(config, &integer_field) == LogicalType::INTEGER);
	integer_field.flags = NUM_FLAG | UNSIGNED_FLAG;
	Require(MySQLTypes::FieldToLogicalType(config, &integer_field) == LogicalType::UINTEGER);
	MYSQL_FIELD string_field {};
	string_field.type = MYSQL_TYPE_STRING;
	string_field.flags = BINARY_FLAG;
	string_field.charsetnr = 33;
	Require(MySQLTypes::FieldToLogicalType(config, &string_field) == LogicalType::VARCHAR);
	string_field.charsetnr = 63;
	Require(MySQLTypes::FieldToLogicalType(config, &string_field) == LogicalType::BLOB);
	for (const auto &item : vector<pair<LogicalType, string>> {
	         {LogicalType::SMALLINT, "32768"}, {LogicalType::BIGINT, "9223372036854775808"},
	         {LogicalType::UBIGINT, "-1"}, {LogicalType::INTEGER, "1.2"}, {LogicalType::INTEGER, "1e2"},
	         {LogicalType::INTEGER, "1suffix"}, {LogicalType::DECIMAL(3, 2), "1.234"},
	         {LogicalType::INTEGER, ""}, {LogicalType::INTEGER, "+"},
	         {LogicalType::DATE, "2026-00-29"}}) {
		bool rejected = false;
		try {
			decode(item.first, item.second);
		} catch (const Exception &) {
			rejected = true;
		}
		Require(rejected);
	}
	std::cout << "typed text-protocol precision tests passed\n";
}
