/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/unittest.h"

namespace {
using boost::intrusive_ptr;
using std::string;
using namespace mongo;

intrusive_ptr<Expression> parse(const string& expressionName, ImplicitValue operand) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    VariablesParseState vps = expCtx->variablesParseState;
    Value operandValue = operand;
    const BSONObj obj = BSON(expressionName << operandValue);
    return Expression::parseExpression(expCtx, obj, vps);
}

Value eval(const string& expressionName,
           ImplicitValue input,
           ImplicitValue find,
           ImplicitValue replacement) {
    auto expression = parse(
        expressionName, Document{{"input", input}, {"find", find}, {"replacement", replacement}});
    return expression->evaluate({}, &expression->getExpressionContext()->variables);
}

Value replaceOne(ImplicitValue input, ImplicitValue find, ImplicitValue replacement) {
    return eval("$replaceOne", input, find, replacement);
}

Value replaceAll(ImplicitValue input, ImplicitValue find, ImplicitValue replacement) {
    return eval("$replaceAll", input, find, replacement);
}

TEST(ExpressionReplaceTest, Expects3NamedArgs) {
    ASSERT_THROWS(parse("$replaceOne", 1), AssertionException);
    ASSERT_THROWS(parse("$replaceOne", BSON_ARRAY(1 << 2)), AssertionException);
    ASSERT_THROWS(parse("$replaceOne", BSONNULL), AssertionException);
    ASSERT_THROWS(parse("$replaceOne", "string"_sd), AssertionException);
    parse("$replaceOne", Document{{"input", 1}, {"find", 1}, {"replacement", 1}});

    ASSERT_THROWS(parse("$replaceAll", 1), AssertionException);
    ASSERT_THROWS(parse("$replaceAll", BSON_ARRAY(1 << 2)), AssertionException);
    ASSERT_THROWS(parse("$replaceAll", BSONNULL), AssertionException);
    ASSERT_THROWS(parse("$replaceAll", "string"_sd), AssertionException);
    parse("$replaceAll", Document{{"input", 1}, {"find", 1}, {"replacement", 1}});
}
TEST(ExpressionReplaceTest, ExpectsStringsOrNullish) {
    // If any argument is non-string non-nullish, it's an error.

    ASSERT_THROWS(replaceOne(1, BSONNULL, BSONNULL), AssertionException);
    ASSERT_THROWS(replaceOne(BSONNULL, 1, BSONNULL), AssertionException);
    ASSERT_THROWS(replaceOne(BSONNULL, BSONNULL, 1), AssertionException);

    ASSERT_THROWS(replaceAll(1, BSONNULL, BSONNULL), AssertionException);
    ASSERT_THROWS(replaceAll(BSONNULL, 1, BSONNULL), AssertionException);
    ASSERT_THROWS(replaceAll(BSONNULL, BSONNULL, 1), AssertionException);

    ASSERT_THROWS(replaceOne(1, ""_sd, ""_sd), AssertionException);
    ASSERT_THROWS(replaceOne(""_sd, 1, ""_sd), AssertionException);
    ASSERT_THROWS(replaceOne(""_sd, ""_sd, 1), AssertionException);

    ASSERT_THROWS(replaceAll(1, ""_sd, ""_sd), AssertionException);
    ASSERT_THROWS(replaceAll(""_sd, 1, ""_sd), AssertionException);
    ASSERT_THROWS(replaceAll(""_sd, ""_sd, 1), AssertionException);
}
TEST(ExpressionReplaceTest, HandlesNullish) {
    // If any argument is nullish, the result is null.

    ASSERT_VALUE_EQ(replaceOne(BSONNULL, ""_sd, ""_sd), Value(BSONNULL));
    ASSERT_VALUE_EQ(replaceOne(""_sd, BSONNULL, ""_sd), Value(BSONNULL));
    ASSERT_VALUE_EQ(replaceOne(""_sd, ""_sd, BSONNULL), Value(BSONNULL));

    ASSERT_VALUE_EQ(replaceAll(BSONNULL, ""_sd, ""_sd), Value(BSONNULL));
    ASSERT_VALUE_EQ(replaceAll(""_sd, BSONNULL, ""_sd), Value(BSONNULL));
    ASSERT_VALUE_EQ(replaceAll(""_sd, ""_sd, BSONNULL), Value(BSONNULL));
}

TEST(ExpressionReplaceTest, ReplacesNothingWhenNoMatches) {
    // When there are no matches, the result is the input, unchanged.

    ASSERT_VALUE_EQ(replaceOne(""_sd, "x"_sd, "y"_sd), Value(""_sd));
    ASSERT_VALUE_EQ(replaceOne("a"_sd, "x"_sd, "y"_sd), Value("a"_sd));
    ASSERT_VALUE_EQ(replaceOne("abcd"_sd, "x"_sd, "y"_sd), Value("abcd"_sd));
    ASSERT_VALUE_EQ(replaceOne("abcd"_sd, "xyz"_sd, "y"_sd), Value("abcd"_sd));
    ASSERT_VALUE_EQ(replaceOne("xyyz"_sd, "xyz"_sd, "y"_sd), Value("xyyz"_sd));

    ASSERT_VALUE_EQ(replaceAll(""_sd, "x"_sd, "y"_sd), Value(""_sd));
    ASSERT_VALUE_EQ(replaceAll("a"_sd, "x"_sd, "y"_sd), Value("a"_sd));
    ASSERT_VALUE_EQ(replaceAll("abcd"_sd, "x"_sd, "y"_sd), Value("abcd"_sd));
    ASSERT_VALUE_EQ(replaceAll("abcd"_sd, "xyz"_sd, "y"_sd), Value("abcd"_sd));
    ASSERT_VALUE_EQ(replaceAll("xyyz"_sd, "xyz"_sd, "y"_sd), Value("xyyz"_sd));
}
TEST(ExpressionReplaceTest, ReplacesOnlyMatch) {
    ASSERT_VALUE_EQ(replaceOne(""_sd, ""_sd, "abc"_sd), Value("abc"_sd));
    ASSERT_VALUE_EQ(replaceOne("x"_sd, "x"_sd, "abc"_sd), Value("abc"_sd));
    ASSERT_VALUE_EQ(replaceOne("xyz"_sd, "xyz"_sd, "abc"_sd), Value("abc"_sd));
    ASSERT_VALUE_EQ(replaceOne("..xyz.."_sd, "xyz"_sd, "abc"_sd), Value("..abc.."_sd));
    ASSERT_VALUE_EQ(replaceOne("..xyz"_sd, "xyz"_sd, "abc"_sd), Value("..abc"_sd));
    ASSERT_VALUE_EQ(replaceOne("xyz.."_sd, "xyz"_sd, "abc"_sd), Value("abc.."_sd));

    ASSERT_VALUE_EQ(replaceAll(""_sd, ""_sd, "abc"_sd), Value("abc"_sd));
    ASSERT_VALUE_EQ(replaceAll("x"_sd, "x"_sd, "abc"_sd), Value("abc"_sd));
    ASSERT_VALUE_EQ(replaceAll("xyz"_sd, "xyz"_sd, "abc"_sd), Value("abc"_sd));
    ASSERT_VALUE_EQ(replaceAll("..xyz.."_sd, "xyz"_sd, "abc"_sd), Value("..abc.."_sd));
    ASSERT_VALUE_EQ(replaceAll("..xyz"_sd, "xyz"_sd, "abc"_sd), Value("..abc"_sd));
    ASSERT_VALUE_EQ(replaceAll("xyz.."_sd, "xyz"_sd, "abc"_sd), Value("abc.."_sd));
}
TEST(ExpressionReplaceOneTest, ReplacesFirstMatchOnly) {
    ASSERT_VALUE_EQ(replaceOne("."_sd, ""_sd, "abc"_sd), Value("abc."_sd));
    ASSERT_VALUE_EQ(replaceOne(".."_sd, ""_sd, "abc"_sd), Value("abc.."_sd));
    ASSERT_VALUE_EQ(replaceOne(".."_sd, "."_sd, "abc"_sd), Value("abc."_sd));
    ASSERT_VALUE_EQ(replaceOne("abc->defg->hij"_sd, "->"_sd, "."_sd), Value("abc.defg->hij"_sd));
}
TEST(ExpressionReplaceAllTest, ReplacesAllMatches) {
    ASSERT_VALUE_EQ(replaceAll("."_sd, ""_sd, "abc"_sd), Value("abc.abc"_sd));
    ASSERT_VALUE_EQ(replaceAll(".."_sd, ""_sd, "abc"_sd), Value("abc.abc.abc"_sd));
    ASSERT_VALUE_EQ(replaceAll(".."_sd, "."_sd, "abc"_sd), Value("abcabc"_sd));
    ASSERT_VALUE_EQ(replaceAll("abc->defg->hij"_sd, "->"_sd, "."_sd), Value("abc.defg.hij"_sd));
}
TEST(ExpressionReplaceTest, DoesNotReplaceInTheReplacement) {
    ASSERT_VALUE_EQ(replaceOne("a.b.c"_sd, "."_sd, ".."_sd), Value("a..b.c"_sd));
    ASSERT_VALUE_EQ(replaceAll("a.b.c"_sd, "."_sd, ".."_sd), Value("a..b..c"_sd));
}

TEST(ExpressionReplaceTest, DoesNotNormalizeUnicode) {
    StringData combiningAcute = "́"_sd;
    StringData combinedAcuteE = "é"_sd;
    ASSERT_EQ(combinedAcuteE[0], 'e');
    ASSERT_EQ(combinedAcuteE.substr(1), combiningAcute);

    StringData precomposedAcuteE = "é";
    ASSERT_NOT_EQUALS(precomposedAcuteE[0], 'e');

    // If the input has combining characters, you can match and replace the base letter.
    ASSERT_VALUE_EQ(replaceOne(combinedAcuteE, "e"_sd, "a"_sd), Value("á"_sd));
    ASSERT_VALUE_EQ(replaceAll(combinedAcuteE, "e"_sd, "a"_sd), Value("á"_sd));

    // If the input has precomposed characters, you can't replace the base letter.
    ASSERT_VALUE_EQ(replaceOne(precomposedAcuteE, "e"_sd, "x"_sd), Value(precomposedAcuteE));
    ASSERT_VALUE_EQ(replaceAll(precomposedAcuteE, "e"_sd, "x"_sd), Value(precomposedAcuteE));

    // Precomposed characters and combined forms can't match each other.
    ASSERT_VALUE_EQ(replaceOne(precomposedAcuteE, combinedAcuteE, "x"_sd),
                    Value(precomposedAcuteE));
    ASSERT_VALUE_EQ(replaceAll(precomposedAcuteE, combinedAcuteE, "x"_sd),
                    Value(precomposedAcuteE));
    ASSERT_VALUE_EQ(replaceOne(combinedAcuteE, precomposedAcuteE, "x"_sd), Value(combinedAcuteE));
    ASSERT_VALUE_EQ(replaceAll(combinedAcuteE, precomposedAcuteE, "x"_sd), Value(combinedAcuteE));
}

}  // namespace
