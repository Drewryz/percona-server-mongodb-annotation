/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/resume_token.h"

#include <boost/optional/optional_io.hpp>
#include <limits>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/pipeline/document_sources_gen.h"
#include "mongo/db/pipeline/value_comparator.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/util/hex.h"

namespace mongo {
constexpr StringData ResumeToken::kDataFieldName;
constexpr StringData ResumeToken::kTypeBitsFieldName;

namespace {

/**
 * Returns a pair of values representing the key-string encoded data and the type bits respectively.
 * Both are of type BinData, but if the type bits of the key string are all zeros then the second
 * Value will be the missing value.
 */
std::pair<Value, Value> encodeInBinDataFormat(const ResumeTokenData& data) {
    // In the legacy format we serialize clusterTime, then documentKey, then UUID.
    BSONObjBuilder builder;
    builder.append("", data.clusterTime);
    data.documentKey.addToBsonObj(&builder, "");
    if (data.uuid) {
        if (data.documentKey.missing()) {
            // Never allow a missing document key with a UUID present, as that will mess up
            // the field order.
            builder.appendNull("");
        }
        data.uuid->appendToBuilder(&builder, "");
    }
    auto keyObj = builder.obj();

    // After writing all the pieces to an object, keystring-encode that object into binary.
    KeyString encodedToken(KeyString::Version::V1, keyObj, Ordering::make(BSONObj()));
    const auto& typeBits = encodedToken.getTypeBits();

    auto rawBinary =
        BSONBinData(encodedToken.getBuffer(), encodedToken.getSize(), BinDataType::BinDataGeneral);
    auto typeBitsValue = typeBits.isAllZeros()
        ? Value()
        : Value(BSONBinData(typeBits.getBuffer(), typeBits.getSize(), BinDataType::BinDataGeneral));
    return {Value(rawBinary), typeBitsValue};
}
}  // namespace

bool ResumeTokenData::operator==(const ResumeTokenData& other) const {
    return clusterTime == other.clusterTime &&
        (Value::compare(this->documentKey, other.documentKey, nullptr) == 0) && uuid == other.uuid;
}

std::ostream& operator<<(std::ostream& out, const ResumeTokenData& tokenData) {
    return out << "{clusterTime: " << tokenData.clusterTime.toString()
               << "  documentKey: " << tokenData.documentKey << "  uuid: " << tokenData.uuid << "}";
}

ResumeToken::ResumeToken(const Document& resumeDoc) {
    _keyStringData = resumeDoc[kDataFieldName];
    _typeBits = resumeDoc[kTypeBitsFieldName];
    uassert(40647,
            str::stream() << "Bad resume token: _data of missing or of wrong type"
                          << resumeDoc.toString(),
            (_keyStringData.getType() == BSONType::BinData &&
             _keyStringData.getBinData().type == BinDataGeneral) ||
                _keyStringData.getType() == BSONType::String);
    uassert(40648,
            str::stream() << "Bad resume token: _typeBits of wrong type" << resumeDoc.toString(),
            _typeBits.missing() || (_typeBits.getType() == BSONType::BinData &&
                                    _typeBits.getBinData().type == BinDataGeneral));
}

// We encode the resume token as a KeyString with the sequence: clusterTime, uuid, documentKey.
// Only the clusterTime is required.
ResumeToken::ResumeToken(const ResumeTokenData& data) {
    BSONObjBuilder builder;
    builder.append("", data.clusterTime);
    uassert(50788,
            "Unexpected resume token with a documentKey but no UUID",
            data.uuid || data.documentKey.missing());

    if (data.uuid) {
        data.uuid->appendToBuilder(&builder, "");
    }
    data.documentKey.addToBsonObj(&builder, "");
    auto keyObj = builder.obj();
    KeyString encodedToken(KeyString::Version::V1, keyObj, Ordering::make(BSONObj()));
    _keyStringData = Value(toHex(encodedToken.getBuffer(), encodedToken.getSize()));
    const auto& typeBits = encodedToken.getTypeBits();
    if (!typeBits.isAllZeros())
        _typeBits = Value(
            BSONBinData(typeBits.getBuffer(), typeBits.getSize(), BinDataType::BinDataGeneral));
}

bool ResumeToken::operator==(const ResumeToken& other) const {
    // '_keyStringData' is enough to determine equality. The type bits are used to unambiguously
    // re-construct the original data, but we do not expect any two resume tokens to have the same
    // data and different type bits, since that would imply they have (1) the same timestamp and (2)
    // the same documentKey (possibly different types). This should not be possible because
    // documents with the same documentKey should be on the same shard and therefore should have
    // different timestamps.
    return ValueComparator::kInstance.evaluate(_keyStringData == other._keyStringData);
}

ResumeTokenData ResumeToken::getData() const {
    KeyString::TypeBits typeBits(KeyString::Version::V1);
    if (!_typeBits.missing()) {
        BSONBinData typeBitsBinData = _typeBits.getBinData();
        BufReader typeBitsReader(typeBitsBinData.data, typeBitsBinData.length);
        typeBits.resetFromBuffer(&typeBitsReader);
    }

    // Accept either serialization format.
    BufBuilder hexDecodeBuf;  // Keep this in scope until we've decoded the bytes.
    BSONBinData keyStringBinData{nullptr, 0, BinDataType::BinDataGeneral};
    boost::optional<std::string> decodedString;
    switch (_keyStringData.getType()) {
        case BSONType::BinData: {
            keyStringBinData = _keyStringData.getBinData();
            break;
        }
        case BSONType::String: {
            uassert(ErrorCodes::FailedToParse,
                    "resume token string was not a valid hex string",
                    isValidHex(_keyStringData.getStringData()));
            fromHexString(_keyStringData.getStringData(), &hexDecodeBuf);
            keyStringBinData = BSONBinData(
                hexDecodeBuf.buf(), hexDecodeBuf.getSize(), BinDataType::BinDataGeneral);
            break;
        }
        default:
            // We validate the type at parse time.
            MONGO_UNREACHABLE;
    }
    auto internalBson = KeyString::toBson(static_cast<const char*>(keyStringBinData.data),
                                          keyStringBinData.length,
                                          Ordering::make(BSONObj()),
                                          typeBits);

    BSONObjIterator i(internalBson);
    ResumeTokenData result;
    uassert(40649, "invalid empty resume token", i.more());
    result.clusterTime = i.next().timestamp();
    if (!i.more()) {
        // There was nothing other than the timestamp.
        return result;
    }
    switch (_keyStringData.getType()) {
        case BSONType::BinData: {
            // In the old format, the documentKey came first, then the UUID.
            result.documentKey = Value(i.next());
            if (i.more()) {
                result.uuid = uassertStatusOK(UUID::parse(i.next()));
            }
            break;
        }
        case BSONType::String: {
            // In the new format, the UUID comes first, then the documentKey.
            result.uuid = uassertStatusOK(UUID::parse(i.next()));
            if (i.more()) {
                result.documentKey = Value(i.next());
            }
            break;
        }
        default: { MONGO_UNREACHABLE }
    }
    uassert(40646, "invalid oversized resume token", !i.more());
    return result;
}

Document ResumeToken::toDocument(SerializationFormat format) const {
    // In most cases we expect to be serializing in the same format we were given.
    const auto dataType = _keyStringData.getType();
    if ((dataType == BSONType::BinData && format == SerializationFormat::kBinData) ||
        (dataType == BSONType::String && format == SerializationFormat::kHexString)) {
        return Document{{kDataFieldName, _keyStringData}, {kTypeBitsFieldName, _typeBits}};
    }

    // If we have to switch formats, then decompose the resume token into its pieces and
    // re-construct a resume token in the new format.
    auto data = getData();

    switch (format) {
        case SerializationFormat::kBinData: {
            // Going from the three pieces of data into BinData requires special logic, since
            // re-constructing a ResumeToken from 'data' will generate the new format.
            Value rawBinary, typeBits;
            std::tie(rawBinary, typeBits) = encodeInBinDataFormat(data);
            return Document{{kDataFieldName, rawBinary}, {kTypeBitsFieldName, typeBits}};
        }
        case SerializationFormat::kHexString: {
            // Constructing a new ResumeToken from the three pieces of data will generate a
            // hex-encoded KeyString as the token.
            const ResumeToken newResumeToken(data);
            return newResumeToken.toDocument(format);
        }
        default: { MONGO_UNREACHABLE; }
    }
}

ResumeToken ResumeToken::parse(const Document& resumeDoc) {
    return ResumeToken(resumeDoc);
}

}  // namespace mongo
