
/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/db/matcher/expression_leaf.h"

namespace mongo {

namespace fts {
class FTSQuery;
}  // namespace fts

/**
 * Common base class for $text match expression implementations.
 */
class TextMatchExpressionBase : public LeafMatchExpression {
public:
    struct TextParams {
        std::string query;
        std::string language;
        bool caseSensitive;
        bool diacriticSensitive;
    };

    static const bool kCaseSensitiveDefault;
    static const bool kDiacriticSensitiveDefault;

    TextMatchExpressionBase();
    virtual ~TextMatchExpressionBase() {}

    /**
     * Returns a reference to the parsed text query that this TextMatchExpressionBase owns.
     */
    virtual const fts::FTSQuery& getFTSQuery() const = 0;

    //
    // Methods inherited from MatchExpression.
    //

    void debugString(StringBuilder& debug, int level = 0) const final;

    void serialize(BSONObjBuilder* out) const final;

    bool equivalent(const MatchExpression* other) const final;

private:
    ExpressionOptimizerFunc getOptimizer() const final {
        return [](std::unique_ptr<MatchExpression> expression) { return expression; };
    }
};

}  // namespace mongo
