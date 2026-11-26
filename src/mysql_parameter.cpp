#include "mysql_parameter.hpp"
#include "mysql_temporal_utils.hpp"

#include "duckdb/common/exception/conversion_exception.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/datetime.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"

namespace duckdb {

template <typename NUM_TYPE>
static void FillNumberBuffer(Value &value, vector<char> &bind_buffer) {
	bind_buffer.resize(sizeof(NUM_TYPE));
	NUM_TYPE val = value.GetValueUnsafe<NUM_TYPE>();
	std::memcpy(bind_buffer.data(), &val, sizeof(NUM_TYPE));
}

static void FillDateBuffer(Value &value, vector<char> &bind_buffer) {
	MYSQL_TIME mt;
	std::memset(&mt, '\0', sizeof(MYSQL_TIME));
	date_t dd = DateValue::Get(value);
	if (!Date::IsFinite(dd)) {
		throw ConversionException("Non-finite DATE value \"%s\" is outside MySQL's supported range", value.ToString());
	}
	int32_t year, month, day;
	Date::Convert(dd, year, month, day);
	if (!IsMySQLDateYearInRange(year) || month < 1 || month > 12 || day < 1 || day > 31) {
		throw ConversionException("DATE value \"%s\" is outside MySQL's supported range (0001-01-01..9999-12-31)",
		                          value.ToString());
	}

	mt.year = static_cast<unsigned int>(year);
	mt.month = static_cast<unsigned int>(month);
	mt.day = static_cast<unsigned int>(day);

	bind_buffer.resize(sizeof(MYSQL_TIME));
	std::memcpy(bind_buffer.data(), &mt, sizeof(MYSQL_TIME));
}

static void FillTimeBuffer(Value &value, vector<char> &bind_buffer) {
	MYSQL_TIME mt;
	std::memset(&mt, '\0', sizeof(MYSQL_TIME));
	dtime_t dt = TimeValue::Get(value);
	int32_t hour, minute, second, micros;
	Time::Convert(dt, hour, minute, second, micros);
	if (hour < 0 || hour > 24 || minute < 0 || minute > 59 || second < 0 || second > 59 || micros < 0 ||
	    micros > 999999 || (hour == 24 && (minute != 0 || second != 0 || micros != 0))) {
		throw ConversionException("TIME value \"%s\" is outside MySQL parameter range", value.ToString());
	}

	mt.hour = static_cast<unsigned int>(hour);
	mt.minute = static_cast<unsigned int>(minute);
	mt.second = static_cast<unsigned int>(second);
	mt.second_part = static_cast<unsigned long>(micros);

	bind_buffer.resize(sizeof(MYSQL_TIME));
	std::memcpy(bind_buffer.data(), &mt, sizeof(MYSQL_TIME));
}

static void FillTimestampBuffer(Value &value, vector<char> &bind_buffer) {
	MYSQL_TIME mt;
	std::memset(&mt, '\0', sizeof(MYSQL_TIME));
	timestamp_t ts = value.type().id() == LogicalTypeId::TIMESTAMP_TZ
	                     ? timestamp_t(TimestampTZValue::Get(value).value)
	                     : TimestampValue::Get(value);
	if (!Timestamp::IsFinite(ts)) {
		throw ConversionException("Non-finite timestamp value \"%s\" is outside MySQL's supported range",
		                          value.ToString());
	}
	if (value.type().id() == LogicalTypeId::TIMESTAMP_TZ && !IsMySQLTimestampUTCInRange(ts.value)) {
		throw ConversionException("TIMESTAMPTZ value \"%s\" is outside MySQL TIMESTAMP range "
		                          "(1970-01-01 00:00:01..2038-01-19 03:14:07.999999 UTC)",
		                          value.ToString());
	}
	date_t dd;
	dtime_t dt;
	Timestamp::Convert(ts, dd, dt);
	int32_t year, month, day;
	Date::Convert(dd, year, month, day);
	int32_t hour, minute, second, micros;
	Time::Convert(dt, hour, minute, second, micros);
	if (!IsMySQLDateYearInRange(year) || month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 ||
	    minute < 0 || minute > 59 || second < 0 || second > 59 || micros < 0 || micros > 999999) {
		throw ConversionException("Timestamp value \"%s\" is outside MySQL's supported range", value.ToString());
	}

	mt.year = static_cast<unsigned int>(year);
	mt.month = static_cast<unsigned int>(month);
	mt.day = static_cast<unsigned int>(day);
	mt.hour = static_cast<unsigned int>(hour);
	mt.minute = static_cast<unsigned int>(minute);
	mt.second = static_cast<unsigned int>(second);
	mt.second_part = static_cast<unsigned long>(micros);

	bind_buffer.resize(sizeof(MYSQL_TIME));
	std::memcpy(bind_buffer.data(), &mt, sizeof(MYSQL_TIME));
}

MySQLParameter::MySQLParameter(const string &query, Value value_p) : value(std::move(value_p)) {
	if (value.IsNull()) {
		return;
	}

	switch (value.type().id()) {
	case LogicalTypeId::BOOLEAN:
		this->buffer_type = MYSQL_TYPE_TINY;
		FillNumberBuffer<bool>(value, bind_buffer);
		break;
	case LogicalTypeId::TINYINT:
		this->buffer_type = MYSQL_TYPE_TINY;
		FillNumberBuffer<int8_t>(value, bind_buffer);
		break;
	case LogicalTypeId::UTINYINT:
		this->buffer_type = MYSQL_TYPE_TINY;
		this->is_unsigned = true;
		FillNumberBuffer<uint8_t>(value, bind_buffer);
		break;
	case LogicalTypeId::SMALLINT:
		this->buffer_type = MYSQL_TYPE_SHORT;
		FillNumberBuffer<int16_t>(value, bind_buffer);
		break;
	case LogicalTypeId::USMALLINT:
		this->buffer_type = MYSQL_TYPE_SHORT;
		this->is_unsigned = true;
		FillNumberBuffer<uint16_t>(value, bind_buffer);
		break;
	case LogicalTypeId::INTEGER:
		this->buffer_type = MYSQL_TYPE_LONG;
		FillNumberBuffer<int32_t>(value, bind_buffer);
		break;
	case LogicalTypeId::UINTEGER:
		this->buffer_type = MYSQL_TYPE_LONG;
		this->is_unsigned = true;
		FillNumberBuffer<uint32_t>(value, bind_buffer);
		break;
	case LogicalTypeId::BIGINT:
		this->buffer_type = MYSQL_TYPE_LONGLONG;
		FillNumberBuffer<int64_t>(value, bind_buffer);
		break;
	case LogicalTypeId::UBIGINT:
		this->buffer_type = MYSQL_TYPE_LONGLONG;
		this->is_unsigned = true;
		FillNumberBuffer<uint64_t>(value, bind_buffer);
		break;
	case LogicalTypeId::FLOAT:
		this->buffer_type = MYSQL_TYPE_FLOAT;
		FillNumberBuffer<float>(value, bind_buffer);
		break;
	case LogicalTypeId::DOUBLE:
		this->buffer_type = MYSQL_TYPE_DOUBLE;
		FillNumberBuffer<double>(value, bind_buffer);
		break;
	case LogicalTypeId::DATE:
		this->buffer_type = MYSQL_TYPE_DATE;
		FillDateBuffer(value, bind_buffer);
		break;
	case LogicalTypeId::TIME:
		this->buffer_type = MYSQL_TYPE_TIME;
		FillTimeBuffer(value, bind_buffer);
		break;
	case LogicalTypeId::TIMESTAMP:
		this->buffer_type = MYSQL_TYPE_DATETIME;
		FillTimestampBuffer(value, bind_buffer);
		break;
	case LogicalTypeId::TIMESTAMP_TZ:
		this->buffer_type = MYSQL_TYPE_TIMESTAMP;
		FillTimestampBuffer(value, bind_buffer);
		break;
	case LogicalTypeId::VARCHAR:
		// use string ref from the value
		break;
	default:
		throw IOException("Unsupported parameters type: \"%s\", MySQL query \"%s\"", value.type(), query.c_str());
	}
}

MYSQL_BIND MySQLParameter::CreateBind() {
	MYSQL_BIND bind;
	std::memset(&bind, '\0', sizeof(MYSQL_BIND));

	if (value.IsNull()) {
		bind.buffer_type = MYSQL_TYPE_NULL;
		bind.length = &bind_length;
	} else if (value.type().id() == LogicalTypeId::VARCHAR) {
		const string &str = StringValue::Get(value);
		bind.buffer_type = MYSQL_TYPE_VAR_STRING;
		bind.buffer = const_cast<char *>(str.c_str());
		bind.buffer_length = str.length();
		bind_length = str.length();
		bind.length = &bind_length;
	} else {
		bind.buffer_type = buffer_type;
		bind.is_unsigned = is_unsigned;
		bind.buffer = bind_buffer.data();
		bind.buffer_length = bind_buffer.size();
		bind_length = bind_buffer.size();
		bind.length = &bind_length;
	}

	return bind;
}

} // namespace duckdb
