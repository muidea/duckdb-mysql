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
	std::cout << "column rename definition preservation/bounds tests passed\n";
}
