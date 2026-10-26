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
}
