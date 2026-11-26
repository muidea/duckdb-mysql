#include "mysql_column_rename.hpp"
#include <iostream>
#include <stdexcept>

using namespace duckdb;
static void RequireAt(bool value, int line) {
	if (!value) {
		throw std::runtime_error("column rename definition assertion failed at line " + std::to_string(line));
	}
}
#define Require(value) RequireAt(value, __LINE__)
static void Reject(const std::string &s, const std::string &table = "t") {
	try {
		MySQLRenameDefinition::Parse(s, table);
	} catch (const std::invalid_argument &) {
		return;
	}
	throw std::runtime_error("ambiguous column definition accepted");
}
int main() {
	const std::string source = R"SQL(CREATE TABLE `t` (
  `id` bigint(20) unsigned NOT NULL AUTO_INCREMENT,
  `值``old` decimal(65,30) unsigned zerofill NOT NULL DEFAULT '1.230000000000000000000000000000' COMMENT 'a,b (c) ''quoted''; \\' ,
  `e` enum('one,two','x)y','''z') CHARACTER SET utf8mb4 COLLATE utf8mb4_bin DEFAULT 'one,two',
  `ts` timestamp(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6),
  `g` int(11) GENERATED ALWAYS AS ((`id` + 1)) STORED COMMENT 'kept',
  `b` bit(3) DEFAULT b'010',
  `v` varchar(32) /*!50600 COLUMN_FORMAT DYNAMIC */ DEFAULT NULL,
  PRIMARY KEY (`id`),
  KEY `ix` (`值``old`)
) ENGINE=InnoDB AUTO_INCREMENT=42 DEFAULT CHARSET=utf8mb4)SQL";
	auto parsed = MySQLRenameDefinition::Parse(source, "t");
	Require(parsed.columns.size() == 7);
	Require(parsed.columns[1].name == "值`old");
	Require(parsed.columns[1].definition.find("decimal(65,30) unsigned zerofill") == 0);
	Require(parsed.columns[4].definition.find("GENERATED ALWAYS AS ((`id` + 1)) STORED") != std::string::npos);
	auto sql = parsed.Change("s`x", "t", "值`old", "新`列");
	Require(sql == "ALTER TABLE `s``x`.`t` CHANGE COLUMN `值``old` `新``列` " + parsed.columns[1].definition);
	auto renamed = parsed;
	renamed.columns[1].name = "新`列";
	Require(parsed.Preserved(renamed, "值`old", "新`列"));
	renamed.columns[1].definition += " ";
	Require(!parsed.Preserved(renamed, "值`old", "新`列"));
	renamed = parsed;
	renamed.columns[1].name = "新`列";
	renamed.columns[0].definition = "bigint(20) NOT NULL";
	Require(!parsed.Preserved(renamed, "值`old", "新`列"));
	renamed = parsed;
	renamed.columns.erase(renamed.columns.begin());
	Require(!parsed.Preserved(renamed, "值`old", "新`列"));
	Require(!parsed.Preserved(parsed, "absent", "new"));
	const auto implicit = MySQLRenameDefinition::Parse(
	    "CREATE TABLE `t` (`v` varchar(20) COMMENT 'DEFAULT NULL, kept', `other` int(11))", "t");
	auto explicit_null = MySQLRenameDefinition::Parse(
	    "CREATE TABLE `t` (`v2` varchar(20) DEFAULT NULL COMMENT 'DEFAULT NULL, kept', `other` int(11))", "t");
	Require(!implicit.Preserved(explicit_null, "v", "v2"));
	Require(implicit.Preserved(explicit_null, "v", "v2", true));
	Require(explicit_null.Preserved(implicit, "v2", "v", true));
	explicit_null.columns[1].definition += " DEFAULT NULL";
	Require(!implicit.Preserved(explicit_null, "v", "v2", true)); // unrelated column stays strict
	for (const auto &definition : {"varchar(20) NOT NULL COMMENT 'DEFAULT NULL, kept'",
	                              "varchar(21) DEFAULT NULL COMMENT 'DEFAULT NULL, kept'",
	                              "varchar(20) DEFAULT 'NULL' COMMENT 'DEFAULT NULL, kept'",
	                              "varchar(20) DEFAULT NULL COMMENT 'changed'",
	                              "varchar(20) DEFAULT 0 COMMENT 'DEFAULT NULL, kept'"}) {
		auto changed = implicit;
		changed.columns[0] = {"v2", definition};
		Require(!implicit.Preserved(changed, "v", "v2", true));
	}
	Require(MySQLRenameDefinition::WithoutNullDefault("int GENERATED ALWAYS AS ((`DEFAULT NULL` + 1)) STORED") ==
	        "int GENERATED ALWAYS AS ((`DEFAULT NULL` + 1)) STORED");
	Require(MySQLRenameDefinition::WithoutNullDefault("enum('DEFAULT NULL','x') DEFAULT NULL") == "enum('DEFAULT NULL','x')");
	Require(MySQLRenameDefinition::WithoutNullDefault("varchar(20) /* DEFAULT NULL */") == "varchar(20) /* DEFAULT NULL */");
	const auto escaped = MySQLRenameDefinition::Parse(
	    R"SQL(CREATE TABLE `t` (`x` varchar(50) DEFAULT 'a\'b\\c' COMMENT 'comma, \'quote\', \\backslash'))SQL", "t");
	Require(escaped.columns.size() == 1);
	Require(escaped.Change("s", "t", "x", "y").find(R"SQL(DEFAULT 'a\'b\\c' COMMENT 'comma, \'quote\', \\backslash')SQL") != std::string::npos);
	auto unicode_default = escaped;
	unicode_default.StringDefault("x", "utf8mb4", "F09F9880");
	Require(unicode_default.columns[0].definition == R"SQL(varchar(50) DEFAULT _utf8mb4 X'F09F9880' COMMENT 'comma, \'quote\', \\backslash')SQL");
	auto empty = MySQLRenameDefinition::Parse("CREATE TABLE `t` (`e` varchar(5) DEFAULT '' COMMENT 'DEFAULT quote')", "t");
	empty.StringDefault("e", "latin1", "");
	Require(empty.columns[0].definition == "varchar(5) DEFAULT _latin1 X'' COMMENT 'DEFAULT quote'");
	for (auto charset : {"UTF8", "utf8;DROP", "", "utf8'"}) {
		try {
			unicode_default.StringDefault("x", charset, "41");
			throw std::runtime_error("invalid default charset accepted");
		} catch (const std::invalid_argument &) {
		}
	}
	for (auto s : {"", "CREATE TABLE `other` (`i` int)", "CREATE TABLE `t` ()", "CREATE TABLE `t` (`i` int,)",
	               "CREATE TABLE `t` (`i` int, `i` int)", "CREATE TABLE `t` (`i` int DEFAULT 'open)",
	               "CREATE TABLE `t` (`i` int /* open)", "CREATE TABLE `t` (`i` int; DROP TABLE x)",
	               "CREATE TABLE `t` (`i` int --comment\n)", "CREATE TABLE `t` (`i`int)",
	               "CREATE TABLE `t` (`i` )", "CREATE TABLE `t` (`i` decimal(12,3)",
	               "CREATE TABLE t (`i` int)", "CREATE TABLE `t (`i` int)"}) {
		Reject(s);
	}
	Reject(std::string("CREATE TABLE `t` (`i` int)") + '\0');
	Reject(std::string(MySQLRenameDefinition::MAX_BYTES + 1, 'x'));
	Reject("CREATE TABLE `t` (`i` int DEFAULT " + std::string(257, '(') + "0" + std::string(257, ')') + ")");
	try {
		parsed.Change("s", "t", "missing", "new");
		throw std::runtime_error("missing column accepted");
	} catch (const std::invalid_argument &) {
	}

	// SetNullability & ReplaceType / Modify preservation tests
	auto null_test = MySQLRenameDefinition::Parse(
	    "CREATE TABLE `t` (`a` int(11) DEFAULT NULL, `b` varchar(16) CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin NOT NULL DEFAULT 'NULL' COMMENT 'NOT NULL', `ts` timestamp(3) NULL DEFAULT NULL, `keep` bigint(20) NOT NULL DEFAULT 1)",
	    "t");
	auto a_nn = null_test;
	a_nn.SetNullability("a", true);
	Require(a_nn.columns[0].definition == "int(11) NOT NULL");
	Require(a_nn.Modify("s", "t", "a") == "ALTER TABLE `s`.`t` MODIFY COLUMN `a` int(11) NOT NULL");
	Require(null_test.PreservedOtherColumns(a_nn, "a"));

	auto b_null = null_test;
	b_null.SetNullability("b", false);
	Require(b_null.columns[1].definition ==
	        "varchar(16) CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin NULL DEFAULT 'NULL' COMMENT 'NOT NULL'");
	auto b_nn = b_null;
	b_nn.SetNullability("b", true);
	Require(b_nn.columns[1].definition ==
	        "varchar(16) CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin NOT NULL DEFAULT 'NULL' COMMENT 'NOT NULL'");

	auto ts_nn = null_test;
	ts_nn.SetNullability("ts", true);
	Require(ts_nn.columns[2].definition == "timestamp(3) NOT NULL");
	auto ts_widen = null_test;
	ts_widen.ReplaceType("ts", "TIMESTAMP(6)");
	Require(ts_widen.columns[2].definition == "TIMESTAMP(6) NULL DEFAULT NULL");

	auto widen = null_test;
	widen.ReplaceType("b", "VARCHAR(64)");
	Require(widen.columns[1].definition ==
	        "VARCHAR(64) CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin NOT NULL DEFAULT 'NULL' COMMENT 'NOT NULL'");
	Require(widen.Modify("s", "t", "b") ==
	        "ALTER TABLE `s`.`t` MODIFY COLUMN `b` VARCHAR(64) CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin NOT NULL DEFAULT 'NULL' COMMENT 'NOT NULL'");

	auto dec_def = MySQLRenameDefinition::Parse(
	    "CREATE TABLE `t` (`n` decimal(10,2) NOT NULL DEFAULT '1.23', `u` int(11) unsigned NOT NULL)", "t");
	dec_def.ReplaceType("n", "DECIMAL(18,2)");
	Require(dec_def.columns[0].definition == "DECIMAL(18,2) NOT NULL DEFAULT '1.23'");
	try {
		dec_def.ReplaceType("u", "BIGINT");
		throw std::runtime_error("unsigned ReplaceType accepted");
	} catch (const std::invalid_argument &) {
	}

	auto bool_def = MySQLRenameDefinition::Parse(
	    "CREATE TABLE `t` (`flag` tinyint(1) NOT NULL DEFAULT '1', `u` tinyint(1) unsigned NOT NULL)", "t");
	Require(bool_def.Change("s", "t", "flag", "flag2") ==
	        "ALTER TABLE `s`.`t` CHANGE COLUMN `flag` `flag2` BOOLEAN NOT NULL DEFAULT '1'");
	Require(bool_def.Modify("s", "t", "flag") ==
	        "ALTER TABLE `s`.`t` MODIFY COLUMN `flag` BOOLEAN NOT NULL DEFAULT '1'");
	Require(bool_def.Modify("s", "t", "u") ==
	        "ALTER TABLE `s`.`t` MODIFY COLUMN `u` tinyint(1) unsigned NOT NULL");

	std::cout << "column rename definition preservation/bounds tests passed\n";
}
