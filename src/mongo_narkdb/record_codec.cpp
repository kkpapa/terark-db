#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage
#ifdef _MSC_VER
#pragma warning(disable: 4800) // bool conversion
#pragma warning(disable: 4244) // 'return': conversion from '__int64' to 'double', possible loss of data
#pragma warning(disable: 4267) // '=': conversion from 'size_t' to 'int', possible loss of data
#endif

#include "record_codec.h"

#include <mongo/util/log.h>
#include <mongo/bson/bsonobjbuilder.h>
#include <mongo/util/hex.h>
#include <nark/io/DataIO.hpp>
#include <nark/io/MemStream.hpp>
#include <nark/lcast.hpp>

namespace mongo { namespace narkdb {

// all non-schema fields packed into this field as ColumnType::CarBin
const char G_schemaLessFieldName[] = "$$";

using namespace nark;
using nark::db::ColumnType;

static void narkEncodeBsonArray(const BSONObj& arr, valvec<char>& encoded);
static void narkEncodeBsonObject(const BSONObj& obj, valvec<char>& encoded);

static
void narkEncodeBsonElemVal(const BSONElement& elem, valvec<char>& encoded) {
	const char* value = elem.value();
	switch (elem.type()) {
	case EOO:
	case Undefined:
	case jstNULL:
	case MaxKey:
	case MinKey:
		break;
	case mongo::Bool:
		encoded.push_back(value[0] ? 1 : 0);
		break;
	case NumberInt:
		encoded.append(value, 4);
		break;
	case bsonTimestamp: // low 32 bit is always positive
	case mongo::Date:
	case NumberDouble:
	case NumberLong:
		encoded.append(value, 8);
		break;
	case jstOID:
	//	log() << "encode: OID=" << toHexLower(value, OID::kOIDSize);
		encoded.append(value, OID::kOIDSize);
		break;
	case Symbol:
	case Code:
	case mongo::String:
	//	log() << "encode: strlen+1=" << elem.valuestrsize() << ", str=" << elem.valuestr();
		encoded.append(value + 4, elem.valuestrsize());
		break;
	case DBRef:
		encoded.append(value + 4, elem.valuestrsize() + OID::kOIDSize);
		break;
	case mongo::Array:
		narkEncodeBsonArray(elem.embeddedObject(), encoded);
		break;
	case Object:
		narkEncodeBsonObject(elem.embeddedObject(), encoded);
		break;
	case CodeWScope:
		encoded.append(value, elem.objsize());
		break;
	case BinData:
		encoded.append(value, 5 + elem.valuestrsize());
		break;
	case RegEx:
		{
			const char* p = value;
			size_t len1 = strlen(p); // regex len
			p += len1 + 1;
			size_t len2 = strlen(p);
			encoded.append(p, len1 + 1 + len2 + 1);
		}
		break;
	default:
		{
			StringBuilder ss;
			ss << "narkEncodeIndexKey(): BSONElement: bad elem.type " << (int)elem.type();
			std::string msg = ss.str();
			massert(10320, msg.c_str(), false);
		}
	}
}

static void narkEncodeBsonArray(const BSONObj& arr, valvec<char>& encoded) {
	int cnt = 0;
	int itemType = 128;
	BSONForEach(item, arr) {
		if (itemType == 128) {
			itemType = item.type();
		} else {
			if (item.type() != itemType) itemType = 129;
		}
		cnt++;
	}
	{
		unsigned char  buf[8];
		encoded.append(buf, nark::save_var_uint32(buf, cnt));
	}
	if (cnt) {
		invariant(itemType != 128);
		encoded.push_back((unsigned char)(itemType));
		BSONForEach(item, arr) {
			if (itemType == 129) { // heterogeneous array items
				encoded.push_back((unsigned char)item.type());
			}
			narkEncodeBsonElemVal(item, encoded);
		}
	}
}

static void narkEncodeBsonObject(const BSONObj& obj, valvec<char>& encoded) {
	BSONForEach(elem, obj) {
		encoded.push_back((unsigned char)(elem.type()));
		encoded.append(elem.fieldName(), elem.fieldNameSize());
		narkEncodeBsonElemVal(elem, encoded);
	}
	encoded.push_back((unsigned char)EOO);
}

SchemaRecordCoder::SchemaRecordCoder() {
}
SchemaRecordCoder::~SchemaRecordCoder() {
}

template<class Vec>
static void Move_AutoGrownMemIO_to_valvec(AutoGrownMemIO& io, Vec& v) {
	BOOST_STATIC_ASSERT(sizeof(typename Vec::value_type) == 1);
	BOOST_STATIC_ASSERT(sizeof(v[0]) == 1);
	v.clear();
	v.risk_set_size(io.tell());
	v.risk_set_data((typename Vec::value_type*)io.buf());
	v.risk_set_capacity(io.size());
	io.risk_release_ownership();
}

template<class FromType>
static void
encodeConvertFrom(ColumnType type, const char* value,
				  valvec<char>* encoded, bool isLastField) {
	typedef unsigned char byte;
	FromType x = unaligned_load<FromType>((const byte*)value);
	switch (type) {
	default:
		invariant(!"encodeConvertFrom: bad type conversion");
		break;
	case ColumnType::Sint08:
		encoded->push_back(int8_t(x));
		break;
	case ColumnType::Uint08:
		encoded->push_back(uint8_t(x));
		break;
	case ColumnType::Sint16:
		unaligned_save((byte*)encoded->grow_no_init(2), int16_t(x));
		break;
	case ColumnType::Uint16:
		unaligned_save((byte*)encoded->grow_no_init(2), uint16_t(x));
		break;
	case ColumnType::Sint32:
		unaligned_save((byte*)encoded->grow_no_init(4), int32_t(x));
		break;
	case ColumnType::Uint32:
		unaligned_save((byte*)encoded->grow_no_init(4), uint32_t(x));
		break;
	case ColumnType::Sint64:
		unaligned_save((byte*)encoded->grow_no_init(8), int64_t(x));
		break;
	case ColumnType::Uint64:
		unaligned_save((byte*)encoded->grow_no_init(8), uint64_t(x));
		break;
	case ColumnType::Float32:
		BOOST_STATIC_ASSERT(sizeof(float) == 4);
		unaligned_save((byte*)encoded->grow_no_init(4), float(x));
		break;
	case ColumnType::Float64:
		BOOST_STATIC_ASSERT(sizeof(double) == 8);
		unaligned_save((byte*)encoded->grow_no_init(8), double(x));
		break;
	case ColumnType::Float128:
		abort(); // not implemented yet
		break;
	case ColumnType::StrZero:
		encoded->append(nark::lcast(x));
		if (!isLastField)
			encoded->push_back('\0');
		break;
	case ColumnType::Binary: {
		std::string str = nark::lcast(x);
		invariant(str.size() < 127);
		if (!isLastField) {
			encoded->push_back(byte(str.size()+1)); // a one-byte var_uint
		}
		encoded->append(str.c_str(), str.size() + 1); // with '\0'
		break; }
	}
}

template<class NumType>
NumType patchMongoLogStream(NumType x) { return x; }
char patchMongoLogStream(signed char x) { return x; }
char patchMongoLogStream(unsigned char x) { return x; }
int patchMongoLogStream(short x) { return x; }

template<class NumType>
static void
appendNumber(double x, valvec<char>* encoded) {
	NumType y;
	if (x <= std::numeric_limits<NumType>::min())
		y = std::numeric_limits<NumType>::min();
	else if (x >= std::numeric_limits<NumType>::max())
		y = std::numeric_limits<NumType>::max();
	else
		y = static_cast<NumType>(x);
//	LOG(2) << "x=" << x << ", y=" << patchMongoLogStream(y);
	unaligned_save((unsigned char*)encoded->grow_no_init(sizeof(y)), y);
}

static void
encodeConvertFromDouble(ColumnType type, const char* value,
						valvec<char>* encoded, bool isLastField) {
	typedef unsigned char byte;
	double x = unaligned_load<double>((const byte*)value);
	switch (type) {
	default:
		invariant(!"encodeConvertFromDouble: bad type conversion");
		break;
	case ColumnType::Sint08:
		appendNumber<int8_t>(x, encoded);
		break;
	case ColumnType::Uint08:
		appendNumber<uint8_t>(x, encoded);
		break;
	case ColumnType::Sint16:
		appendNumber<int16_t>(x, encoded);
		break;
	case ColumnType::Uint16:
		appendNumber<uint16_t>(x, encoded);
		break;
	case ColumnType::Sint32:
		appendNumber<int32_t>(x, encoded);
		break;
	case ColumnType::Uint32:
		appendNumber<uint32_t>(x, encoded);
		break;
	case ColumnType::Sint64:
		appendNumber<int64_t>(x, encoded);
		break;
	case ColumnType::Uint64:
		appendNumber<uint64_t>(x, encoded);
		break;
	case ColumnType::Float32:
		BOOST_STATIC_ASSERT(sizeof(float) == 4);
		unaligned_save((byte*)encoded->grow_no_init(4), float(x));
		break;
	case ColumnType::Float64:
		BOOST_STATIC_ASSERT(sizeof(double) == 8);
		unaligned_save((byte*)encoded->grow_no_init(8), double(x));
		break;
	case ColumnType::Float128:
		abort(); // not implemented yet
		break;
	case ColumnType::StrZero:
		encoded->append(nark::lcast(x));
		if (!isLastField)
			encoded->push_back('\0');
		break;
	case ColumnType::Binary: {
		std::string str = nark::lcast(x);
		invariant(str.size() < 127);
		if (!isLastField) {
			encoded->push_back(byte(str.size()+1)); // a one-byte var_uint
		}
		encoded->append(str.c_str(), str.size() + 1); // with '\0'
		break; }
	}
}

static void
encodeConvertString(ColumnType type, const char* str, valvec<char>* encoded) {
	typedef unsigned char byte;
	char* endp = nullptr;
	switch (type) {
	default:
		invariant(!"encodeConvertString: bad type conversion");
		break;
	case ColumnType::Sint08:
		{
			int8_t x = (int8_t)strtol(str, &endp, 10);
			if ('\0' == *endp)
				unaligned_save((byte*)encoded->grow_no_init(1), x);
			else
				THROW_STD(invalid_argument, "str is not a number");
		}
		break;
	case ColumnType::Uint08:
		{
			uint8_t x = (uint8_t)strtoul(str, &endp, 10);
			if ('\0' == *endp)
				unaligned_save((byte*)encoded->grow_no_init(1), x);
			else
				THROW_STD(invalid_argument, "str is not a number");
		}
		break;
	case ColumnType::Sint16:
		{
			int16_t x = (int16_t)strtol(str, &endp, 10);
			if ('\0' == *endp)
				unaligned_save((byte*)encoded->grow_no_init(2), x);
			else
				THROW_STD(invalid_argument, "str is not a number");
		}
		break;
	case ColumnType::Uint16:
		{
			uint16_t x = (uint16_t)strtoul(str, &endp, 10);
			if ('\0' == *endp)
				unaligned_save((byte*)encoded->grow_no_init(2), x);
			else
				THROW_STD(invalid_argument, "str is not a number");
		}
		break;
	case ColumnType::Sint32:
		{
			int32_t x = (int32_t)strtol(str, &endp, 10);
			if ('\0' == *endp)
				unaligned_save((byte*)encoded->grow_no_init(4), x);
			else
				THROW_STD(invalid_argument, "str is not a number");
		}
		break;
	case ColumnType::Uint32:
		{
			uint32_t x = (uint32_t)strtoul(str, &endp, 10);
			if ('\0' == *endp)
				unaligned_save((byte*)encoded->grow_no_init(4), x);
			else
				THROW_STD(invalid_argument, "str is not a number");
		}
		break;
	case ColumnType::Sint64:
		{
			int64_t x = (int64_t)strtoll(str, &endp, 10);
			if ('\0' == *endp)
				unaligned_save((byte*)encoded->grow_no_init(8), x);
			else
				THROW_STD(invalid_argument, "str is not a number");
		}
		break;
	case ColumnType::Uint64:
		{
			uint64_t x = (uint64_t)strtoull(str, &endp, 10);
			if ('\0' == *endp)
				unaligned_save((byte*)encoded->grow_no_init(8), x);
			else
				THROW_STD(invalid_argument, "str is not a number");
		}
		break;
	case ColumnType::Float32:
		BOOST_STATIC_ASSERT(sizeof(float) == 4);
		{
			float x = strtof(str, &endp);
			if ('\0' == *endp)
				unaligned_save((byte*)encoded->grow_no_init(4), x);
			else
				THROW_STD(invalid_argument, "str is not a number");
		}
		break;
	case ColumnType::Float64:
		BOOST_STATIC_ASSERT(sizeof(double) == 8);
		{
			double x = strtod(str, &endp);
			if ('\0' == *endp)
				unaligned_save((byte*)encoded->grow_no_init(8), x);
			else
				THROW_STD(invalid_argument, "str is not a number");
		}
		break;
	case ColumnType::Float128:
		abort(); // not implemented yet
		break;
	}
}

// static
bool SchemaRecordCoder::fieldsEqual(const FieldsMap& x, const FieldsMap& y) {
	if (x.end_i() != y.end_i()) {
		return false;
	}
	for (size_t i = 0; i < x.end_i(); ++i) {
		fstring xname = x.key(i);
		size_t j = y.find_i(xname);
		if (j == y.end_i()) {
			return false;
		}
		fstring yname = y.key(j);
		BSONElement xe(xname.data()-1, xname.size()+1, BSONElement::FieldNameSizeTag());
		BSONElement ye(yname.data()-1, yname.size()+1, BSONElement::FieldNameSizeTag());
		if (xe.type() == NumberDouble || ye.type() == NumberDouble) {
			double xd = xe.numberDouble();
			double yd = ye.numberDouble();
			if (fabs((xd - yd) / xd) > 0.1)
				return false;
			else
				continue;
		}
		else if (xe != ye)
			return false;
	}
	return true;
}

// static
void SchemaRecordCoder::parseToFields(const BSONObj& obj, FieldsMap* fields) {
	fields->erase_all();
//	std::string fieldnames;
	BSONForEach(elem, obj) {
	//	const char* fieldname = elem.fieldName(); // gcc-4.9.3 produce error code
		fstring fieldname = elem.fieldName(); // gcc is ok
		auto ib = fields->insert_i(fieldname);
	//	LOG(1)	<< "insert('" << fieldname.c_str() << "', len=" << fieldname.size() << ")=" << ib.first;
	//	LOG(1)	<< "find_i('" << fieldname.c_str() << "', len=" << fieldname.size() << ")=" << fields->find_i(fieldname);
		if (!ib.second) {
			THROW_STD(invalid_argument,
					"bad bson: duplicate fieldname: %s", fieldname.c_str());
		}
		invariant(fields->elem_at(ib.first).size() == fieldname.size());
//		fieldnames += fieldname.c_str();
//		fieldnames.push_back(',');
	}
//	fieldnames.pop_back();
//	LOG(1)	<< "SchemaRecordCoder::encode: bsonFields=" << fieldnames;
/*
	fieldnames.resize(0);
	for (size_t i = 0; i < fields->end_i(); ++i) {
		fstring fieldname = fields->elem_at(i);
		fieldnames.append(fieldname.c_str());
		fieldnames.push_back(',');
		LOG(1) << "hash(fieldname='" << fieldname.str() << "')=" << fields->hash_v(fieldname);
		invariant(fields->find_i(fieldname) == i);
	}
	fieldnames.pop_back();
	LOG(1)	<< "SchemaRecordCoder::encode: m_fields=" << fieldnames;
*/
}

// for WritableSegment, param schema is m_rowSchema, param exclude is nullptr
// for ReadonlySegment, param schema is m_nonIndexSchema,
//                      param exclude is m_uniqIndexFields
void SchemaRecordCoder::encode(const Schema* schema, const Schema* exclude,
							   const BSONObj& obj, valvec<char>* encoded) {
	assert(nullptr != schema);
	encoded->resize(0);
	parseToFields(obj, &m_fields);
	m_stored.resize_fill(m_fields.end_i(), false);

	// last is $$ field, the schema-less fields
	size_t schemaColumn
		= schema->m_columnsMeta.end_key(1) == G_schemaLessFieldName
		? schema->m_columnsMeta.end_i() - 1
		: schema->m_columnsMeta.end_i()
		;
	for(size_t i = 0; i < schemaColumn; ++i) {
		fstring     colname = schema->m_columnsMeta.key(i);
		const auto& colmeta = schema->m_columnsMeta.val(i);
		assert(colname != G_schemaLessFieldName);
		size_t j = m_fields.find_i(colname);
		if (j >= m_fields.end_i()) {
			LOG(1)	<< "colname=" << colname.str() << " is missing"
					<< ", j=" << j
					<< ", m_fields.end_i()=" << m_fields.end_i()
					<< ", bson=" << obj.toString();
		}
		invariant(j < m_fields.end_i());
		bool isLastField = schema->m_columnsMeta.end_i() - 1 == i;
		BSONElement elem(m_fields.key(j).data() - 1, colname.size()+1,
						 BSONElement::FieldNameSizeTag());
		BSONType elemType = elem.type();
		const char* value = elem.value();
		switch (elemType) {
		case EOO:
		case Undefined:
		case jstNULL:
		case MaxKey:
		case MinKey:
			break;
		case mongo::Bool:
			encoded->push_back(value[0] ? 1 : 0);
			assert(colmeta.type == nark::db::ColumnType::Uint08);
			break;
		case NumberInt:
			encodeConvertFrom<int32_t>(colmeta.type, value, encoded, isLastField);
			break;
		case NumberDouble:
			encodeConvertFromDouble(colmeta.type, value, encoded, isLastField);
			break;
		case NumberLong:
			encodeConvertFrom<int64_t>(colmeta.type, value, encoded, isLastField);
			break;
		case bsonTimestamp: // low 32 bit is always positive
			invariant(colmeta.type == ColumnType::Sint64 ||
					  colmeta.type == ColumnType::Uint64);
			encoded->append(value, 8);
			break;
		case mongo::Date:
			encodeConvertFrom<int64_t>(colmeta.type, value, encoded, isLastField);
			if (colmeta.type == ColumnType::Uint32 ||
				colmeta.type == ColumnType::Sint32)
			{
				int64_t millisec = ConstDataView(value).read<LittleEndian<int64_t>>();
				int64_t sec = millisec / 1000;
				DataView(encoded->grow_no_init(4)).write<LittleEndian<int>>(sec);
			}
			else if (colmeta.type == ColumnType::Uint64 ||
					 colmeta.type == ColumnType::Sint64) {
				encoded->append(value, 8);
			}
			else {
				invariant(!"mongo::Date must map to one of nark sint32, uint32, sint64, uint64");
			}
			break;
		case jstOID:
		//	log() << "encode: OID=" << toHexLower(value, OID::kOIDSize);
			encoded->append(value, OID::kOIDSize);
			assert(colmeta.type == nark::db::ColumnType::Fixed);
			assert(colmeta.fixedLen == OID::kOIDSize);
			break;
		case Symbol:
		case Code:
		case mongo::String:
		//	log() << "encode: strlen+1=" << elem.valuestrsize() << ", str=" << elem.valuestr();
			if (colmeta.type == nark::db::ColumnType::StrZero) {
				encoded->append(value + 4, elem.valuestrsize());
			}
			else {
				encodeConvertString(colmeta.type, value + 4, encoded);
			}
			break;
		case DBRef:
			assert(0); // deprecated, should not in data
			encoded->append(value + 4, elem.valuestrsize() + OID::kOIDSize);
			break;
		case mongo::Array:
			assert(colmeta.type == nark::db::ColumnType::CarBin);
			{
				size_t oldsize = encoded->size();
				encoded->resize(oldsize + 4); // reserve for uint32 length
				narkEncodeBsonArray(elem.embeddedObject(), *encoded);
				size_t len = encoded->size() - (oldsize + 4);
				DataView(encoded->data()+oldsize)
						.write(LittleEndian<uint32_t>(uint32_t(len)));
			}
			break;
		case Object:
			assert(colmeta.type == nark::db::ColumnType::CarBin);
			{
				size_t oldsize = encoded->size();
				encoded->resize(oldsize + 4); // reserve for uint32 length
				narkEncodeBsonObject(elem.embeddedObject(), *encoded);
				size_t len = encoded->size() - (oldsize + 4);
				DataView(encoded->data()+oldsize)
						.write(LittleEndian<uint32_t>(uint32_t(len)));
			}
			break;
		case CodeWScope:
			assert(colmeta.type == nark::db::ColumnType::CarBin);
			{
				assert(colmeta.type == nark::db::ColumnType::CarBin);
				size_t oldsize = encoded->size();
				encoded->resize(oldsize + 8); // reserve for uint32 length + uint32 codelen
				DataView(encoded->data()+oldsize + 4)
						.write(LittleEndian<uint32_t>(elem.codeWScopeCodeLen()));
				encoded->append(elem.codeWScopeCode(), elem.codeWScopeCodeLen());
				narkEncodeBsonObject(elem.codeWScopeObject(), *encoded);
				size_t len = encoded->size() - (oldsize + 4);
				DataView(encoded->data()+oldsize)
						.write(LittleEndian<uint32_t>(uint32_t(len)));
			}
			encoded->append(value, elem.objsize());
			break;
		case BinData:
			{
				assert(colmeta.type == nark::db::ColumnType::CarBin);
				uint32_t len = elem.valuestrsize() + 1; // 1 is for subtype byte
				encoded->resize(encoded->size() + 4);
				DataView(encoded->end() - 4)
						.write(LittleEndian<uint32_t>(len));
				encoded->append(value + 4, 1 + elem.valuestrsize());
			}
			break;
		case RegEx:
			{
				const char* p = value;
				size_t len1 = strlen(p); // regex len
				p += len1 + 1;
				size_t len2 = strlen(p);
				encoded->append(p, len1 + 1 + len2 + 1);
			}
			assert(colmeta.type == nark::db::ColumnType::TwoStrZero);
			break;
		default:
			{
				StringBuilder ss;
				ss << BOOST_CURRENT_FUNCTION
				   << ": BSONElement: bad elem.type " << (int)elem.type();
				std::string msg = ss.str();
				massert(10320, msg.c_str(), false);
			}
		}
		m_stored.set1(j);
	}

	if (schemaColumn == schema->columnNum()) {
		// has no schema-less column
		bool isAllStored = m_stored.isall1();
		assert(isAllStored);
		if (!isAllStored) {
			THROW_STD(invalid_argument,
				"schema is forced on all fields, but input data has extra fields");
		}
		return;
	}

	size_t idx = 0;
	for (auto it = obj.begin(), End = obj.end(); it != End; ++it, ++idx) {
		if (m_stored.is1(idx))
			continue;
		BSONElement elem = *it;
		fstring fieldName = elem.fieldName();
		assert(fieldName.end()[0] == 0);
		if (exclude) {
			size_t colid = exclude->m_columnsMeta.find_i(fieldName);
			if (colid >= exclude->columnNum())
				continue;
		}
		encoded->push_back((unsigned char)elem.type());
		encoded->append(fieldName.data(), fieldName.size()+1);
		narkEncodeBsonElemVal(elem, *encoded);
	}
}

nark::valvec<char>
SchemaRecordCoder::encode(const Schema* schema, const Schema* exclude, const BSONObj& obj) {
	nark::valvec<char> encoded;
	encode(schema, exclude, obj, &encoded);
	return encoded;
}

typedef LittleEndianDataOutput<AutoGrownMemIO> MyBsonBuilder;

static void narkDecodeBsonObject(MyBsonBuilder& bb, const char*& pos, const char* end);
static void narkDecodeBsonArray(MyBsonBuilder& bb, const char*& pos, const char* end);

template<class TargetBsonType>
static TargetBsonType
decodeConvertTo(ColumnType fromSchemaType, const char*& pos) {
	TargetBsonType val = 0;
	switch (fromSchemaType) {
	default:
		invariant(!"encodeConvertFrom: bad type conversion");
		break;
	case ColumnType::Sint08:
		val = TargetBsonType((signed char)*pos++);
		break;
	case ColumnType::Uint08:
		val = TargetBsonType((unsigned char)*pos++);
		break;
	case ColumnType::Sint16:
		val = TargetBsonType(unaligned_load<int16_t>(pos));
		pos += 2;
		break;
	case ColumnType::Uint16:
		val = TargetBsonType(unaligned_load<uint16_t>(pos));
		pos += 2;
		break;
	case ColumnType::Sint32:
		val = TargetBsonType(unaligned_load<int32_t>(pos));
		pos += 4;
		break;
	case ColumnType::Uint32:
		val = TargetBsonType(unaligned_load<uint32_t>(pos));
		pos += 4;
		break;
	case ColumnType::Sint64:
		val = TargetBsonType(unaligned_load<int64_t>(pos));
		pos += 8;
		break;
	case ColumnType::Uint64:
		val = TargetBsonType(unaligned_load<uint64_t>(pos));
		pos += 8;
		break;
	case ColumnType::Float32:
		if (boost::is_floating_point<TargetBsonType>::value) {
			BOOST_STATIC_ASSERT(sizeof(float) == 4);
			val = unaligned_load<float>(pos);
		}
		else {
			float x = unaligned_load<float>(pos);
			if (x <= std::numeric_limits<TargetBsonType>::min())
				val = std::numeric_limits<TargetBsonType>::min();
			else if (x >= std::numeric_limits<TargetBsonType>::max())
				val = std::numeric_limits<TargetBsonType>::max();
			else
				val = static_cast<TargetBsonType>(x);
		}
		pos += 4;
		break;
	case ColumnType::Float64:
		if (boost::is_floating_point<TargetBsonType>::value) {
			BOOST_STATIC_ASSERT(sizeof(double) == 8);
			val = unaligned_load<double>(pos);
		}
		else {
			double x = unaligned_load<double>(pos);
			if (x <= std::numeric_limits<TargetBsonType>::min())
				val = std::numeric_limits<TargetBsonType>::min();
			else if (x >= std::numeric_limits<TargetBsonType>::max())
				val = std::numeric_limits<TargetBsonType>::max();
			else
				val = static_cast<TargetBsonType>(x);
		}
		pos += 8;
		break;
	case ColumnType::Float128:
		abort(); // not implemented yet
		break;
	}
	return val;
}

static void narkDecodeBsonElemVal(MyBsonBuilder& bb, const char*& pos, const char* end, int type) {
	switch (type) {
	case EOO:
		invariant(!"narkDecodeBsonElemVal: encountered EOO");
		break;
	case Undefined:
	case jstNULL:
	case MaxKey:
	case MinKey:
		break;
	case mongo::Bool:
		bb << char(*pos ? 1 : 0);
		pos++;
		break;
	case NumberInt:
		bb.ensureWrite(pos, 4);
		pos += 4;
		break;
	case bsonTimestamp:
	case mongo::Date:
	case NumberDouble:
	case NumberLong:
		bb.ensureWrite(pos, 8);
		pos += 8;
		break;
	case jstOID:
		bb.ensureWrite(pos, OID::kOIDSize);
		pos += OID::kOIDSize;
		break;
	case Symbol:
	case Code:
	case mongo::String:
		{
			size_t len = strlen(pos);
			bb << int(len + 1);
			bb.ensureWrite(pos, len + 1);
		//	log() << "decode: strlen=" << len << ", str=" << pos;
			pos += len + 1;
		}
		break;
	case DBRef:
		{
			size_t len = strlen(pos);
			bb << int(len + 1);
			bb.ensureWrite(pos + 4, len + 1 + OID::kOIDSize);
			pos += len + 1 + OID::kOIDSize;
		}
		break;
	case mongo::Array:
		narkDecodeBsonArray(bb, pos, end);
		break;
	case Object:
		narkDecodeBsonObject(bb, pos, end);
		break;
	case CodeWScope:
		{
			int len = ConstDataView(pos).read<LittleEndian<int>>();
			bb.ensureWrite(pos, len);
			pos += len;
		}
		break;
	case BinData:
		{
			int len = ConstDataView(pos).read<LittleEndian<int>>();
			bb << len;
			bb << pos[4]; // binary data subtype
			bb.ensureWrite(pos + 5, len);
			pos += 5 + len;
		}
		break;
	case RegEx:
		{
			size_t len1 = strlen(pos); // regex len
			size_t len2 = strlen(pos + len1 + 1);
			size_t len3 = len1 + len2 + 2;
			bb.ensureWrite(pos, len3);
			pos += len3;
		}
		break;
	default:
		{
			StringBuilder ss;
			ss << "narkDecodeIndexKey(): BSONElement: bad subkey.type " << (int)type;
			std::string msg = ss.str();
			massert(10320, msg.c_str(), false);
		}
	}
}

static void narkDecodeBsonObject(MyBsonBuilder& bb, const char*& pos, const char* end) {
	int byteNumOffset = bb.tell();
	bb << 0; // reserve 4 bytes for object byteNum
	for (;;) {
		if (pos >= end) {
			THROW_STD(invalid_argument, "Invalid encoded bson object");
		}
		const int type = (unsigned char)(*pos++);
		bb << char(type);
		if (type == EOO)
			break;
		StringData fieldname = pos;
		bb.ensureWrite(fieldname.begin(), fieldname.size()+1);
		pos += fieldname.size() + 1;
		narkDecodeBsonElemVal(bb, pos, end, type);
	}
	int objByteNum = int(bb.tell() - byteNumOffset);
//	log() << "byteNumOffset" << byteNumOffset << ", objByteNum=" << objByteNum;
	DataView((char*)bb.buf() + byteNumOffset)
			.write<LittleEndian<int>>(objByteNum);
}

static void narkDecodeBsonArray(MyBsonBuilder& bb, const char*& pos, const char* end) {
	int cnt = nark::load_var_uint32((unsigned char*)pos, (const unsigned char**)&pos);
	if (0 == cnt) {
		bb << int(5); // 5 is empty bson object size
		bb << char(EOO);
		return;
	}
	int arrItemType = (unsigned char)(*pos++);
	int arrByteNumOffset = bb.tell();
	bb << int(0); // reserve for arrByteNum
	for (int arrIndex = 0; arrIndex < cnt; arrIndex++) {
		if (pos >= end) {
			THROW_STD(invalid_argument, "Invalid encoded bson array");
		}
		const int curItemType = arrItemType == 129 ? (unsigned char)(*pos++) : arrItemType;
		bb << char(curItemType);
		std::string idxStr = BSONObjBuilder::numStr(arrIndex);
		bb.ensureWrite(idxStr.c_str(), idxStr.size()+1);
		narkDecodeBsonElemVal(bb, pos, end, curItemType);
	}
	bb << char(EOO);
	int arrByteNum = bb.tell() - arrByteNumOffset;
//	log() << "arrByteNumOffset" << arrByteNumOffset << "arrByteNum=" << arrByteNum;
	(int&)bb.buf()[arrByteNumOffset] = arrByteNum;
}

SharedBuffer
SchemaRecordCoder::decode(const Schema* schema, const char* data, size_t size) {
	assert(nullptr != schema);
	LOG(1) << "SchemaRecordCoder::decode: data=" << schema->toJsonStr(fstring(data, size));
	MyBsonBuilder bb;
	const char* pos = data;
	bb.resize(sizeof(SharedBuffer::Holder) + 4 + 2 * size);
	bb.skip(sizeof(SharedBuffer::Holder));
	bb.skip(4); // object size
	size_t colnum = schema->m_columnsMeta.end_i();
	// last is $$ field, the schema-less fields
	size_t schemaColumn
		= schema->m_columnsMeta.end_key(1) == G_schemaLessFieldName
		? colnum - 1
		: colnum
		;
	const char* end = data + size;
	for (size_t i = 0; i < schemaColumn; ++i) {
		fstring     colname = schema->m_columnsMeta.key(i);
		const auto& colmeta = schema->m_columnsMeta.val(i);
		bb.writeByte(colmeta.uType);
		bb.ensureWrite(colname.data(), colname.size()+1); // include '\0'
		switch ((signed char)colmeta.uType) {
		case EOO:
			invariant(!"narkDecodeBsonElemVal: encountered EOO");
			break;
		case Undefined:
		case jstNULL:
		case MaxKey:
		case MinKey:
			assert(0);
			break;
		case mongo::Bool:
			assert(colmeta.fixedLen == 1);
			bb << char(decodeConvertTo<char>(colmeta.type, pos) ? 1 : 0);
			break;
		case NumberInt:
			bb << decodeConvertTo<int>(colmeta.type, pos);
			break;
		case bsonTimestamp:
			invariant(colmeta.type == ColumnType::Sint64 ||
					  colmeta.type == ColumnType::Uint64);
			bb.ensureWrite(pos, 8);
			pos += 8;
			break;
		case mongo::Date:
			switch (colmeta.type) {
			default:
				invariant(!"SchemaRecordCoder::decode: mongo::Date must map to one of nark sint32, uint32, sint64, uint64");
				break;
			case ColumnType::Sint32:
			case ColumnType::Uint32:
				{
					int64_t ival = ConstDataView(pos).read<LittleEndian<int>>();
					int64_t millisec = 1000 * ival;
					bb << millisec;
					pos += 4;
				}
				break;
			case ColumnType::Sint64:
			case ColumnType::Uint64:
				bb.ensureWrite(pos, 8);
				pos += 8;
				break;
			}
			break;
		case NumberDouble:
			bb << decodeConvertTo<double>(colmeta.type, pos);
			break;
		case NumberLong:
			bb << decodeConvertTo<int64_t>(colmeta.type, pos);
			break;
		case jstOID:
			invariant(colmeta.type == ColumnType::Fixed);
			invariant(colmeta.fixedLen == OID::kOIDSize);
			bb.ensureWrite(pos, OID::kOIDSize);
			pos += OID::kOIDSize;
			break;
		case Symbol:
		case Code:
		case mongo::String:
			invariant(colmeta.type == ColumnType::StrZero);
			if (colnum-1 == i) {
				size_t len = end - pos;
				if (nark_unlikely(0 == len)) {
					bb << int(1);
					bb.writeByte('\0');
				}
				else if ('\0' != end[-1]) {
					bb << int(len + 1);
					bb.ensureWrite(pos, len);
					bb.writeByte('\0');
				}
				else {
					bb << int(len);
					bb.ensureWrite(pos, len);
				}
				pos = end;
			}
			else {
				size_t len = strlen(pos);
				bb << int(len + 1);
				bb.ensureWrite(pos, len + 1);
				pos += len + 1;
			}
			break;
		case DBRef:
			{
				size_t len = strlen(pos);
				bb << int(len + 1);
				bb.ensureWrite(pos + 4, len + 1 + OID::kOIDSize);
				pos += len + 1 + OID::kOIDSize;
			}
			break;
		case mongo::Array:
			{
				invariant(colmeta.type == ColumnType::CarBin);
				size_t len = ConstDataView(pos).read<LittleEndian<uint32_t> >();
				auto   end = pos + len;
				narkDecodeBsonArray(bb, pos, end);
			}
			break;
		case Object:
			{
				invariant(colmeta.type == ColumnType::CarBin);
				size_t len = ConstDataView(pos).read<LittleEndian<uint32_t> >();
				auto   end = pos + len;
				narkDecodeBsonObject(bb, pos, end);
			}
			break;
		case CodeWScope:
			{
				invariant(colmeta.type == ColumnType::CarBin);
				int binlen = ConstDataView(pos).read<LittleEndian<int>>();
				size_t oldpos = bb.tell();
				bb << uint32_t(0); // reserve for whole len
				int codelen = ConstDataView(pos+4).read<LittleEndian<int>>();
				bb << uint32_t(codelen);
				bb.ensureWrite(pos + 8, codelen);
				auto end = pos + binlen;
				pos += 8 + codelen;
				narkDecodeBsonObject(bb, pos, end);
				uint32_t wholeLen = uint32_t(bb.tell() - oldpos);
				DataView((char*)bb.buf() + oldpos).write<LittleEndian<uint32_t> >(wholeLen);
			}
			break;
		case BinData:
			{
				invariant(colmeta.type == ColumnType::CarBin);
				int len = ConstDataView(pos).read<LittleEndian<int>>();
				bb << len - 1; // pos[4] is binary data subtype
				bb.ensureWrite(pos + 4, len);
				pos += 4 + len;
			}
			break;
		case RegEx:
			invariant(colmeta.type == ColumnType::TwoStrZero);
			if (colnum-1 == i && '\0' != end[-1]) {
				size_t len1 = strlen(pos); // regex len
				size_t len2 = end - (pos + len1 + 1);
				bb.ensureWrite(pos, len1 + 1 + len2);
				bb.writeByte('\0');
				pos = end;
			} else {
				size_t len1 = strlen(pos); // regex len
				size_t len2 = strlen(pos + len1 + 1);
				size_t len3 = len1 + len2 + 2;
				bb.ensureWrite(pos, len3);
				pos += len3;
			}
			break;
		default:
			{
				StringBuilder ss;
				ss << "narkDecodeIndexKey(): BSONElement: bad subkey.type " << (int)colmeta.uType;
				std::string msg = ss.str();
				massert(10320, msg.c_str(), false);
			}
		}
	}
	if (pos < end) {
		while (pos < end) {
			const int type = (unsigned char)(*pos++);
			bb << char(type);
			assert(EOO != type);
			StringData fieldname = pos;
			bb.ensureWrite(fieldname.begin(), fieldname.size()+1);
			pos += fieldname.size() + 1;
			narkDecodeBsonElemVal(bb, pos, end, type);
		}
		invariant(pos == end);
	}
	else {
		invariant(pos == end);
	}
	bb << char(EOO); // End of object
	bb.shrink_to_fit();
	int bsonSize = int(bb.tell() - sizeof(SharedBuffer::Holder));
	DataView((char*)bb.buf() + sizeof(SharedBuffer::Holder))
			.write<LittleEndian<int>>(bsonSize);

	return SharedBuffer::takeOwnership((char*)bb.release());
}

SharedBuffer
SchemaRecordCoder::decode(const Schema* schema, const nark::valvec<char>& encoded) {
	return decode(schema, encoded.data(), encoded.size());
}

SharedBuffer
SchemaRecordCoder::decode(const Schema* schema, StringData encoded) {
	return decode(schema, encoded.rawData(), encoded.size());
}

SharedBuffer
SchemaRecordCoder::decode(const Schema* schema, nark::fstring encoded) {
	return decode(schema, encoded.data(), encoded.size());
}

/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////

void encodeIndexKey(const Schema& indexSchema,
					const BSONObj& bson,
					nark::valvec<char>* encoded) {
//	LOG(2) << "encodeIndexKey: bson=" << bson.toString();
	encoded->erase_all();
	using nark::db::ColumnType;
	BSONObj::iterator iter = bson.begin();
	for(size_t i = 0; i < indexSchema.m_columnsMeta.end_i(); ++i) {
#if !defined(NDEBUG)
		fstring     colname = indexSchema.m_columnsMeta.key(i);
		assert(!colname.empty());
#endif
		const auto& colmeta = indexSchema.m_columnsMeta.val(i);
		BSONElement elem(iter.next());
		const char* value = elem.value();
		switch (elem.type()) {
		case EOO:
		case Undefined:
		case jstNULL:
		case MaxKey:
		case MinKey:
			break;
		case mongo::Bool:
			encoded->push_back(value[0] ? 1 : 0);
			assert(indexSchema.getColumnType(i) == ColumnType::Uint08);
			break;
		case NumberInt:
			encodeConvertFrom<int32_t>(colmeta.type, value, encoded,
				indexSchema.m_columnsMeta.end_i()-1 == i);
			break;
		case NumberDouble:
			encodeConvertFromDouble(colmeta.type, value, encoded,
				indexSchema.m_columnsMeta.end_i()-1 == i);
			break;
		case NumberLong:
			encodeConvertFrom<int64_t>(colmeta.type, value, encoded,
				indexSchema.m_columnsMeta.end_i()-1 == i);
			break;
		case bsonTimestamp: // low 32 bit is always positive
		case mongo::Date:
			encodeConvertFrom<int64_t>(colmeta.type, value, encoded,
				indexSchema.m_columnsMeta.end_i()-1 == i);
			break;
		case jstOID:
		//	log() << "encode: OID=" << toHexLower(value, OID::kOIDSize);
			encoded->append(value, OID::kOIDSize);
			assert(colmeta.type == ColumnType::Fixed);
			assert(colmeta.fixedLen == OID::kOIDSize);
			break;
		case Symbol:
		case Code:
		case mongo::String:
		//	log() << "encode: strlen+1=" << elem.valuestrsize() << ", str=" << elem.valuestr();
			if (colmeta.type == ColumnType::StrZero) {
				encoded->append(value + 4, elem.valuestrsize());
			}
			else {
				encodeConvertString(colmeta.type, value + 4, encoded);
			}
			break;
		case DBRef:
			assert(0); // deprecated, should not in data
			encoded->append(value + 4, elem.valuestrsize() + OID::kOIDSize);
			break;
		case mongo::Array:
			abort(); // not supported
			break;
		case Object:
			if (0 == i && elem.embeddedObject().isEmpty()) {
				return; // done, empty object
			}
			abort(); // not supported
			assert(colmeta.type == ColumnType::CarBin);
			break;
		case CodeWScope:
			assert(indexSchema.getColumnType(i) == ColumnType::CarBin);
			abort(); // not supported
			break;
		case BinData:
			assert(indexSchema.getColumnType(i) == ColumnType::CarBin);
			abort(); // not supported
			break;
		case RegEx:
			{
				const char* p = value;
				size_t len1 = strlen(p); // regex len
				p += len1 + 1;
				size_t len2 = strlen(p);
				encoded->append(p, len1 + 1 + len2 + 1);
			}
			assert(colmeta.type == ColumnType::TwoStrZero);
			break;
		default:
			{
				StringBuilder ss;
				ss << BOOST_CURRENT_FUNCTION
				   << ": BSONElement: bad elem.type " << (int)elem.type();
				std::string msg = ss.str();
				massert(10320, msg.c_str(), false);
			}
		}
	}
}

void encodeIndexKey(const Schema& indexSchema,
					const BSONObj& bson,
					nark::valvec<unsigned char>* encoded) {
	encodeIndexKey(indexSchema, bson,
		reinterpret_cast<nark::valvec<char>*>(encoded));
}

SharedBuffer
decodeIndexKey(const Schema& indexSchema, const char* data, size_t size) {
//	LOG(2)	<< "decodeIndexKey: size=" << size << ", data=" << indexSchema.toJsonStr(data, size);
	MyBsonBuilder bb;
	const char* pos = data;
	bb.resize(sizeof(SharedBuffer::Holder) + 4 + 2*size);
	bb.skip(sizeof(SharedBuffer::Holder));
	bb.skip(4); // object size loc
	size_t colnum = indexSchema.m_columnsMeta.end_i();
	const char* end = data + size;
	for (size_t i = 0; i < colnum; ++i) {
		fstring     colname = indexSchema.m_columnsMeta.key(i);
		const auto& colmeta = indexSchema.m_columnsMeta.val(i);
		bb.writeByte(colmeta.uType);
		bb.ensureWrite(colname.data(), colname.size()+1); // include '\0'
		switch ((signed char)colmeta.uType) {
		case EOO:
			invariant(!"narkDecodeBsonElemVal: encountered EOO");
			break;
		case Undefined:
		case jstNULL:
		case MaxKey:
		case MinKey:
			break;
		case mongo::Bool:
			bb << char(decodeConvertTo<char>(colmeta.type, pos) ? 1 : 0);
			break;
		case NumberInt:
			bb << decodeConvertTo<int>(colmeta.type, pos);
			break;
		case bsonTimestamp:
		case mongo::Date:
			bb << decodeConvertTo<int64_t>(colmeta.type, pos);
			break;
		case NumberDouble:
			bb << decodeConvertTo<double>(colmeta.type, pos);
			break;
		case NumberLong:
			bb << decodeConvertTo<int64_t>(colmeta.type, pos);
			break;
		case jstOID:
			bb.ensureWrite(pos, OID::kOIDSize);
			pos += OID::kOIDSize;
			break;
		case Symbol:
		case Code:
		case mongo::String:
			invariant(colmeta.type == ColumnType::StrZero);
			if (colnum-1 == i) {
				size_t len = end - pos;
				if ('\0' != end[-1]) {
					bb << int(len + 1);
					bb.ensureWrite(pos, len);
					bb.writeByte('\0');
				}
				else {
					bb << int(len);
					bb.ensureWrite(pos, len);
				}
				pos = end;
			}
			else {
				size_t len = strlen(pos);
				bb << int(len + 1);
				bb.ensureWrite(pos, len + 1);
				pos += len + 1;
			}
			break;
		case DBRef:
			{
				size_t len = strlen(pos);
				bb << int(len + 1);
				bb.ensureWrite(pos + 4, len + 1 + OID::kOIDSize);
				pos += len + 1 + OID::kOIDSize;
			}
			break;
		case mongo::Array:
			THROW_STD(invalid_argument, "mongo::Array must not be a index key field");
			break;
		case Object:
			THROW_STD(invalid_argument, "mongo::Object must not be a index key field");
			break;
		case CodeWScope:
			THROW_STD(invalid_argument, "mongo::CodeWScope must not be a index key field");
			break;
		case BinData:
			THROW_STD(invalid_argument, "mongo::BinData could'nt not be a index key field");
			break;
		case RegEx:
			invariant(colmeta.type == ColumnType::TwoStrZero);
			if (colnum-1 == i && '\0' != end[-1]) {
				size_t len1 = strlen(pos); // regex len
				size_t len2 = end - (pos + len1 + 1);
				bb.ensureWrite(pos, len1 + 1 + len2);
				bb.writeByte('\0');
				pos = end;
			} else {
				size_t len1 = strlen(pos); // regex len
				size_t len2 = strlen(pos + len1 + 1);
				size_t len3 = len1 + len2 + 2;
				bb.ensureWrite(pos, len3);
				pos += len3;
			}
			break;
		default:
			{
				StringBuilder ss;
				ss << "narkDecodeIndexKey(): BSONElement: bad subkey.type " << (int)colmeta.uType;
				std::string msg = ss.str();
				massert(10320, msg.c_str(), false);
			}
		}
	}
	invariant(pos == end);
	bb << char(EOO); // end of object
	bb.shrink_to_fit();
	int bsonSize = int(bb.tell() - sizeof(SharedBuffer::Holder));
	DataView((char*)bb.buf() + sizeof(SharedBuffer::Holder))
			.write<LittleEndian<int>>(bsonSize);

	return SharedBuffer::takeOwnership((char*)bb.release());
}

} } // namespace mongo::narkdb

