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

#pragma once

#include "mongo/db/matcher/schema/expression_internal_schema_num_properties.h"

namespace mongo {

/**
 * MatchExpression for $_internalSchemaMinProperties keyword. Takes an integer
 * argument that indicates the minimum amount of properties in an object.
 */
class InternalSchemaMinPropertiesMatchExpression final
    : public InternalSchemaNumPropertiesMatchExpression {
public:
    InternalSchemaMinPropertiesMatchExpression()
        : InternalSchemaNumPropertiesMatchExpression(MatchType::INTERNAL_SCHEMA_MIN_PROPERTIES,
                                                     "$_internalSchemaMinProperties") {}

    bool matches(const MatchableDocument* doc, MatchDetails* details) const {
        BSONObj obj = doc->toBSON();
        return (obj.nFields() >= numProperties());
    }

    bool matchesSingleElement(const BSONElement& elem) const {
        if (elem.type() != BSONType::Object) {
            return false;
        }
        return (elem.Obj().nFields() >= numProperties());
    }

    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        auto minProperties = stdx::make_unique<InternalSchemaMinPropertiesMatchExpression>();
        invariantOK(minProperties->init(numProperties()));
        if (getTag()) {
            minProperties->setTag(getTag()->clone());
        }
        return std::move(minProperties);
    }
};
}  // namespace mongo
