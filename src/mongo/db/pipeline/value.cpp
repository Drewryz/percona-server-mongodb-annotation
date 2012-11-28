/**
 * Copyright (c) 2011 10gen Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "mongo/pch.h"

#include "mongo/db/pipeline/value.h"

#include <boost/functional/hash.hpp>

#include "mongo/db/jsobj.h"
#include "mongo/db/pipeline/builder.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
    using namespace mongoutils;

    void ValueStorage::putString(StringData s) {
        const size_t sizeWithNUL = s.size() + 1;
        if (sizeWithNUL <= sizeof(shortStrStorage)) {
            shortStr = true;
            shortStrSize = s.size();
            s.copyTo( shortStrStorage, true );
        }
        else {
            intrusive_ptr<const RCString> rcs = RCString::create(s);
            fassert(16492, rcs);
            genericRCPtr = rcs.get();
            intrusive_ptr_add_ref(genericRCPtr);
            refCounter = true;
        }
    }

    void ValueStorage::putDocument(const Document& d) {
        genericRCPtr = d._storage.get();

        if (genericRCPtr) { // NULL here means empty document
            intrusive_ptr_add_ref(genericRCPtr);
            refCounter = true;
        }
    }

    void ValueStorage::putVector(const RCVector* vec) {
        fassert(16485, vec);

        genericRCPtr = vec;
        intrusive_ptr_add_ref(genericRCPtr);
        refCounter = true;
    }

    Document ValueStorage::getDocument() const {
        if (!genericRCPtr)
            return Document();

        dassert(typeid(*genericRCPtr) == typeid(const DocumentStorage));
        const DocumentStorage* documentPtr = static_cast<const DocumentStorage*>(genericRCPtr);
        return Document(documentPtr);
    }

    Value::Value(BSONType theType): _storage(theType) {
        switch(getType()) {
        case Undefined:
        case jstNULL:
        case Object: // empty
            break;

        case Array: // empty
            _storage.putVector(new RCVector());
            break;

        case Bool:
            _storage.boolValue = false;
            break;

        case NumberDouble:
            _storage.doubleValue = 0;
            break;

        case NumberInt:
            _storage.intValue = 0;
            break;

        case NumberLong:
            _storage.longValue = 0;
            break;

        case Date:
            _storage.dateValue = 0;
            break;

        case Timestamp:
            _storage.timestampValue = 0;
            break;

        default:
            // nothing else is allowed
            uassert(16001, str::stream() <<
                    "can't create empty Value of type " << typeName(getType()), false);
            break;
        }
    }


    Value Value::createFromBsonElement(const BSONElement* pBsonElement) {
        return Value(*pBsonElement);
    }

    Value::Value(const BSONElement& elem) : _storage(elem.type()) {
        switch(getType()) {
        case NumberDouble:
            _storage.doubleValue = elem.Double();
            break;

        case String:
            _storage.putString(StringData(elem.valuestr(), elem.valuestrsize()-1));
            break;

        case Object: {
            _storage.putDocument(Document(elem.embeddedObject()));
            break;
        }

        case Array: {
            intrusive_ptr<RCVector> vec (new RCVector);
            BSONForEach(sub, elem.embeddedObject()) {
                vec->vec.push_back(Value(sub));
            }
            _storage.putVector(vec.get());
            break;
        }

        case jstOID:
            BOOST_STATIC_ASSERT(sizeof(_storage.oid) == sizeof(OID));
            memcpy(_storage.oid, elem.OID().getData(), sizeof(OID));
            break;

        case Bool:
            _storage.boolValue = elem.boolean();
            break;

        case Date:
            // this is really signed but typed as unsigned for historical reasons
            _storage.dateValue = static_cast<long long>(elem.date().millis);
            break;

        case RegEx:
            _storage.putString(elem.regex());
            // TODO elem.regexFlags();
            break;

        case NumberInt:
            _storage.intValue = elem.numberInt();
            break;

        case Timestamp:
            // asDate is a poorly named function that returns a ReplTime
            _storage.timestampValue = elem._opTime().asDate();
            break;

        case NumberLong:
            _storage.longValue = elem.numberLong();
            break;

        case Undefined:
        case jstNULL:
            break;

        case BinData:
        case Symbol:
        case CodeWScope:

            /* these shouldn't happen in this context */
        case MinKey:
        case EOO:
        case DBRef:
        case Code:
        case MaxKey:
            uassert(16002, str::stream() <<
                    "can't create Value of BSON type " << typeName(getType()), false);
            break;
        }
    }

    Value Value::createIntOrLong(long long value) {
        if (value > numeric_limits<int>::max() || value < numeric_limits<int>::min()) {
            // it is too large to be an int and should remain a long
            return Value(value);
        }

        // should be an int since all arguments were int and it fits
        return createInt(value);
    }

    Value Value::createDate(const long long value) {
        // Can't directly construct because constructor would clash with createLong
        Value val (Date);
        val._storage.dateValue = value;
        return val;
    }

    double Value::getDouble() const {
        BSONType type = getType();
        if (type == NumberInt)
            return _storage.intValue;
        if (type == NumberLong)
            return static_cast< double >( _storage.longValue );

        verify(type == NumberDouble);
        return _storage.doubleValue;
    }

    Document Value::getDocument() const {
        verify(getType() == Object);
        return _storage.getDocument();
    }

    Value Value::operator[] (size_t index) const {
        if (missing() || getType() != Array || index >= getArrayLength())
            return Value();

        return getArray()[index];
    }

    Value Value::operator[] (StringData name) const {
        if (missing() || getType() != Object)
            return Value();

        return getDocument()[name];
    }

    BSONObjBuilder& operator << (BSONObjBuilderValueStream& builder, const Value& val) {
        if (val.missing())
            return builder.builder();

        switch(val.getType()) {
        case jstNULL:      return builder << BSONNULL;
        case Undefined:    return builder << BSONUndefined;
        case jstOID:       return builder << val.getOid();
        case NumberInt:    return builder << val.getInt();
        case NumberLong:   return builder << val.getLong();
        case NumberDouble: return builder << val.getDouble();
        case String:       return builder << val.getStringData();
        case Bool:         return builder << val.getBool();
        case Date:         return builder << Date_t(val.getDate());
        case Timestamp:    return builder << val.getTimestamp();
        case Object:       return builder << val.getDocument();

        case Array: {
            const vector<Value>& array = val.getArray();
            const size_t n = array.size();
            BSONArrayBuilder arrayBuilder(builder.subarrayStart());
            for(size_t i = 0; i < n; i++) {
                array[i].addToBsonArray(&arrayBuilder);
            }
            arrayBuilder.doneFast();
            return builder.builder();
        }


        // TODO: these need to not be appended as strings SERVER-6470
        case RegEx:  return builder << val.getRegex();
        case Symbol: return builder << val.getSymbol();

            /* these shouldn't appear in this context */
        case BinData:
        case CodeWScope:
        case MinKey:
        case EOO:
        case DBRef:
        case Code:
        case MaxKey:
            verify(false); // CW TODO better message
        }
        verify(false);
    }

    void Value::addToBsonObj(BSONObjBuilder* pBuilder, StringData fieldName) const {
        *pBuilder << fieldName.data() << *this;
    }

    void Value::addToBsonArray(BSONArrayBuilder* pBuilder) const {
        if (!missing()) { // don't want to increment builder's counter
            *pBuilder << *this;
        }
    }

    bool Value::coerceToBool() const {
        // TODO Unify the implementation with BSONElement::trueValue().
        switch(getType()) {
        case String:
        case Object:
        case Array:
        case BinData:
        case jstOID:
        case Date:
        case RegEx:
        case Symbol:
        case Timestamp:
            return true;

        case jstNULL:
        case Undefined:
            return false;

        case Bool: return _storage.boolValue;
        case NumberInt: return _storage.intValue;
        case NumberLong: return _storage.longValue;
        case NumberDouble: return _storage.doubleValue;

            /* these shouldn't happen in this context */
        case CodeWScope:
        case MinKey:
        case EOO:
        case DBRef:
        case Code:
        case MaxKey:
        default:
            verify(false); // CW TODO better message
        }
    }

    int Value::coerceToInt() const {
        switch(getType()) {
        case NumberDouble:
            return static_cast<int>(_storage.doubleValue);

        case NumberInt:
            return _storage.intValue;

        case NumberLong:
            return static_cast<int>(_storage.longValue);

        case jstNULL:
        case Undefined:
            return 0;

        case String:
        default:
            uassert(16003, str::stream() <<
                    "can't convert from BSON type " << typeName(getType()) <<
                    " to int",
                    false);
        } // switch(getType())
    }

    long long Value::coerceToLong() const {
        switch(getType()) {
        case NumberDouble:
            return static_cast<long long>(_storage.doubleValue);

        case NumberInt:
            return static_cast<long long>(_storage.intValue);

        case NumberLong:
            return _storage.longValue;

        case jstNULL:
        case Undefined:
            return 0;

        case String:
        default:
            uassert(16004, str::stream() <<
                    "can't convert from BSON type " << typeName(getType()) <<
                    " to long",
                    false);
        } // switch(getType())
    }

    double Value::coerceToDouble() const {
        switch(getType()) {
        case NumberDouble:
            return _storage.doubleValue;

        case NumberInt:
            return static_cast<double>(_storage.intValue);

        case NumberLong:
            return static_cast<double>(_storage.longValue);

        case jstNULL:
        case Undefined:
            return 0;

        case String:
        default:
            uassert(16005, str::stream() <<
                    "can't convert from BSON type " << typeName(getType()) <<
                    " to double",
                    false);
        } // switch(getType())
    }

    long long Value::coerceToDate() const {
        switch(getType()) {
        case Date:
            return getDate();

        case Timestamp:
            return getTimestamp().getSecs() * 1000LL;

        default:
            uassert(16006, str::stream() <<
                    "can't convert from BSON type " << typeName(getType()) << " to Date",
                    false);
        } // switch(getType())
    }

    time_t Value::coerceToTimeT() const {
        long long millis = coerceToDate();
        if (millis < 0) {
            // We want the division below to truncate toward -inf rather than 0
            // eg Dec 31, 1969 23:59:58.001 should be -2 seconds rather than -1
            // This is needed to get the correct values from coerceToTM
            if ( -1999 / 1000 != -2) { // this is implementation defined
                millis -= 1000-1;
            }
        }
        const long long seconds = millis / 1000;

        uassert(16421, "Can't handle date values outside of time_t range",
               seconds >= std::numeric_limits<time_t>::min() &&
               seconds <= std::numeric_limits<time_t>::max());

        return static_cast<time_t>(seconds);
    }
    tm Value::coerceToTm() const {
        // See implementation in Date_t.
        // Can't reuse that here because it doesn't support times before 1970
        time_t dtime = coerceToTimeT();
        tm out;

#if defined(_WIN32) // Both the argument order and the return values differ
        bool itWorked = gmtime_s(&out, &dtime) == 0;
#else
        bool itWorked = gmtime_r(&dtime, &out) != NULL;
#endif

        if (!itWorked) {
            if (dtime < 0) {
                // Windows docs say it doesn't support these, but empirically it seems to work
                uasserted(16422, "gmtime failed - your system doesn't support dates before 1970");
            }
            else {
                uasserted(16423, str::stream() << "gmtime failed to convert time_t of " << dtime);
            }
        }

        return out;
    }

    static string tmToISODateString(const tm& time) {
        char buf[128];
        size_t len = strftime(buf, 128, "%Y-%m-%dT%H:%M:%S", &time);
        verify(len > 0);
        verify(len < 128);
        return buf;
    }

    string Value::coerceToString() const {
        stringstream ss;
        switch(getType()) {
        case NumberDouble:
            ss << _storage.doubleValue;
            return ss.str();

        case NumberInt:
            ss << _storage.intValue;
            return ss.str();

        case NumberLong:
            ss << _storage.longValue;
            return ss.str();

        case String:
            return getString();

        case Timestamp:
            ss << getTimestamp().toStringPretty();
            return ss.str();

        case Date:
            return tmToISODateString(coerceToTm());

        case jstNULL:
        case Undefined:
            return "";

        default:
            uassert(16007, str::stream() <<
                    "can't convert from BSON type " << typeName(getType()) <<
                    " to String",
                    false);
        } // switch(getType())
    }

    OpTime Value::coerceToTimestamp() const {
        switch(getType()) {
        case Timestamp:
            return getTimestamp();

        default:
            uassert(16378, str::stream() <<
                    "can't convert from BSON type " << typeName(getType()) <<
                    " to timestamp",
                    false);
        } // switch(getType())
    }

    // Helper function for Value::compare.
    // Better than l-r for cases where difference > MAX_INT
    template <typename T>
    inline static int cmp(const T& left, const T& right) {
        if (left < right) {
            return -1;
        }
        else if (left == right) {
            return 0;
        }
        else {
            dassert(left > right);
            return 1;
        }
    }

    // Special case for double since it needs special NaN handling
    inline static int cmp(double left, double right) {
        // The following is lifted directly from compareElementValues
        // to ensure identical handling of NaN
        if (left < right) 
            return -1;
        if (left == right)
            return 0;
        if (isNaN(left))
            return isNaN(right) ? 0 : -1;
        return 1;
    }

    int Value::compare(const Value& rL, const Value& rR) {
        // TODO: remove conditional after SERVER-6571
        BSONType lType = rL.missing() ? EOO : rL.getType();
        BSONType rType = rR.missing() ? EOO : rR.getType();

        int ret = lType == rType
                    ? 0 // fast-path common case
                    : cmp(canonicalizeBSONType(lType),
                          canonicalizeBSONType(rType));

        if (ret)
            return ret;

        switch(lType) {
        // For supported types, order is the same as in compareElementValues().
        // All unsupported types at end.

        // These are valueless types
        case EOO:
        case Undefined:
        case jstNULL:
            return ret;

        case Bool:
            return rL.getBool() - rR.getBool();

        // WARNING: Timestamp and Date have same canonical type, but compare differently.
        // Maintaining behavior from normal BSON.
        case Timestamp: // unsigned
            return cmp(rL._storage.timestampValue, rR._storage.timestampValue);
        case Date: // signed
            return cmp(rL._storage.dateValue, rR._storage.dateValue);

        // Numbers should compare by equivalence even if different types
        case NumberDouble:
        case NumberLong:
        case NumberInt:
            switch (getWidestNumeric(lType, rType)) {
            case NumberDouble: return cmp(rL.getDouble(), rR.getDouble());
            case NumberLong:   return cmp(rL.getLong(),   rR.getLong());
            case NumberInt:    return cmp(rL.getInt(),    rR.getInt());
            default: verify(false);
            }

        case jstOID:
            return memcmp(rL._storage.oid, rR._storage.oid, sizeof(OID));

        case String:
            return rL.getStringData().compare(rR.getStringData());

        case Object:
            return Document::compare(rL.getDocument(), rR.getDocument());

        case Array: {
            const vector<Value>& lArr = rL.getArray();
            const vector<Value>& rArr = rR.getArray();

            const size_t elems = min(lArr.size(), rArr.size());
            for (size_t i = 0; i < elems; i++ ) {
                // compare the two corresponding elements
                const int cmp = Value::compare(lArr[i], rArr[i]);
                if (cmp)
                    return cmp; // values are unequal
            }

            // if we get here we are either equal or one is prefix of the other 
            return cmp(lArr.size(), rArr.size());
        }

        case RegEx: // TODO: consider flags
            return rL.getRegex().compare(rR.getRegex());

        // unsupported types
        case BinData:
        case Symbol:
        case CodeWScope:
        case MinKey:
        case DBRef:
        case Code:
        case MaxKey:
            uassert(16017, str::stream() <<
                    "comparisons of values of BSON type " << typeName(lType) <<
                    " are not supported", false);
        } // switch(lType)

        verify(false);
    }

    void Value::hash_combine(size_t &seed) const {
        // TODO: remove conditional after SERVER-6571
        if (missing()) {
            return; // same as Undefined
        }
        switch(getType()) {
            /*
              Numbers whose values are equal need to hash to the same thing
              as well.  Note that Value::compare() promotes numeric values to
              their largest common form in order for comparisons to work.
              We must hash all numeric values as if they are doubles so that
              things like grouping work.  We don't know what values will come
              down the pipe later, but if we start out with int representations
              of a value, and later see double representations of it, they need
              to end up in the same buckets.
             */
        case NumberDouble:
        case NumberLong:
        case NumberInt: {
            boost::hash_combine(seed, getDouble());
            break;
        }

        case String: {
            StringData sd = getStringData();
            boost::hash_range(seed, sd.rawData(), (sd.rawData() + sd.size()));
            break;
        }

        case Object:
            getDocument()->hash_combine(seed);
            break;

        case Array: {
            const vector<Value>& vec = getArray();
            for (size_t i=0; i < vec.size(); i++)
                vec[i].hash_combine(seed);
            break;
        }

        case BinData:
        case Symbol:
        case CodeWScope:
            uassert(16018, str::stream() <<
                    "hashes of values of BSON type " << typeName(getType()) <<
                    " are not supported", false);
            break;

        case jstOID:
            getOid().hash_combine(seed);
            break;

        case Bool:
            boost::hash_combine(seed, getBool());
            break;

        case Date:
            boost::hash_combine(seed, getDate());
            break;

        case RegEx:
            boost::hash_combine(seed, getRegex());
            break;

        case Timestamp:
            boost::hash_combine(seed, _storage.timestampValue);
            break;

        case Undefined:
        case jstNULL:
            break;

            /* these shouldn't happen in this context */
        case MinKey:
        case EOO:
        case DBRef:
        case Code:
        case MaxKey:
            verify(false); // CW TODO better message
            break;
        } // switch(getType())
    }

    BSONType Value::getWidestNumeric(BSONType lType, BSONType rType) {
        if (lType == NumberDouble) {
            switch(rType) {
            case NumberDouble:
            case NumberLong:
            case NumberInt:
            case jstNULL:
            case Undefined:
                return NumberDouble;

            default:
                break;
            }
        }
        else if (lType == NumberLong) {
            switch(rType) {
            case NumberDouble:
                return NumberDouble;

            case NumberLong:
            case NumberInt:
            case jstNULL:
            case Undefined:
                return NumberLong;

            default:
                break;
            }
        }
        else if (lType == NumberInt) {
            switch(rType) {
            case NumberDouble:
                return NumberDouble;

            case NumberLong:
                return NumberLong;

            case NumberInt:
            case jstNULL:
            case Undefined:
                return NumberInt;

            default:
                break;
            }
        }
        else if ((lType == jstNULL) || (lType == Undefined)) {
            switch(rType) {
            case NumberDouble:
                return NumberDouble;

            case NumberLong:
                return NumberLong;

            case NumberInt:
                return NumberInt;

            default:
                break;
            }
        }

        // Reachable, but callers must subsequently err out in this case.
        return Undefined;
    }

    size_t Value::getApproximateSize() const {
        switch(getType()) {
        case String:
            return sizeof(Value) + sizeof(RCString) + getStringData().size();

        case Object:
            return sizeof(Value) + getDocument()->getApproximateSize();

        case Array: {
            size_t size = sizeof(Value);
            size += sizeof(RCVector);
            const size_t n = getArray().size();
            for(size_t i = 0; i < n; ++i) {
                size += getArray()[i].getApproximateSize();
            }
            return size;
        }

        case NumberDouble:
        case BinData:
        case jstOID:
        case Bool:
        case Date:
        case RegEx:
        case Symbol:
        case CodeWScope:
        case NumberInt:
        case Timestamp:
        case NumberLong:
        case jstNULL:
        case Undefined:
            return sizeof(Value);

            /* these shouldn't happen in this context */
        case MinKey:
        case EOO:
        case DBRef:
        case Code:
        case MaxKey:
            verify(false); // CW TODO better message
        }

        /*
          We shouldn't get here.  In order to make the implementor think about
          these cases, they are all listed explicitly, above.  The compiler
          should complain if they aren't all listed, because there's no
          default.  However, not all the compilers seem to do that.  Therefore,
          this final catch-all is here.
         */
        verify(false);
    }

    string Value::toString() const {
        // TODO use StringBuilder when operator << is ready
        stringstream out;
        out << *this;
        return out.str();
    }

    ostream& operator << (ostream& out, const Value& val) {
        if (val.missing()) return out << "MISSING";

        switch(val.getType()) {
        case jstOID: return out << val.getOid();
        case String: return out << '"' << val.getString() << '"';
        case RegEx: return out << '/' << val.getRegex() << '/';
        case Symbol: return out << val.getSymbol();
        case Bool: return out << (val.getBool() ? "true" : "false");
        case NumberDouble: return out << val.getDouble();
        case NumberLong: return out << val.getLong();
        case NumberInt: return out << val.getInt();
        case jstNULL: return out << "null";
        case Undefined: return out << "undefined";
        case Date: return out << time_t_to_String_short(val.coerceToTimeT());
        case Timestamp: return out << val.getTimestamp().toString();
        case Object: return out << val.getDocument()->toString();
        case Array: {
            out << "[";
            const size_t n = val.getArray().size();
            for(size_t i = 0; i < n; i++) {
                if (i)
                    out << ", ";
                out << val.getArray()[i];
            }
            out << "]";
            return out;
        }

            /* these shouldn't happen in this context */
        case CodeWScope:
        case BinData: 
        case MinKey:
        case EOO:
        case DBRef:
        case Code:
        case MaxKey:
            verify(false); // CW TODO better message
        }


        // Not in default case to trigger better warning if a case is missing
        verify(false);
    }

}
