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

#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/projection_executor.h"
#include "mongo/db/exec/projection_executor_builder.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/expression_find_internal.h"
#include "mongo/db/query/projection_parser.h"
#include "mongo/unittest/unittest.h"

namespace mongo::projection_executor {
constexpr auto kProjectionPostImageVarName =
    projection_executor::ProjectionExecutor::kProjectionPostImageVarName;

auto createProjectionExecutor(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                              const BSONObj& projSpec,
                              ProjectionPolicies policies) {
    auto projection = projection_ast::parse(expCtx, projSpec, policies);
    return projection_executor::buildProjectionExecutor(
        expCtx, &projection, policies, true /* optimizeExecutor */);
}

class PositionalProjectionExecutionTest : public AggregationContextFixture {
protected:
    auto applyPositional(const BSONObj& projSpec,
                         const BSONObj& matchSpec,
                         const std::string& path,
                         const Document& input) {
        auto executor = createProjectionExecutor(getExpCtx(), projSpec, {});
        auto matchExpr = CopyableMatchExpression{matchSpec,
                                                 getExpCtx(),
                                                 std::make_unique<ExtensionsCallbackNoop>(),
                                                 MatchExpressionParser::kBanAllSpecialFeatures};
        auto expr = make_intrusive<ExpressionInternalFindPositional>(
            getExpCtx(),
            ExpressionFieldPath::parse(getExpCtx(), "$$ROOT", getExpCtx()->variablesParseState),
            ExpressionFieldPath::parse(
                getExpCtx(), "$$" + kProjectionPostImageVarName, getExpCtx()->variablesParseState),
            path,
            std::move(matchExpr));
        executor->setRootReplacementExpression(expr);
        return executor->applyTransformation(input);
    }
};

class SliceProjectionExecutionTest : public AggregationContextFixture {
protected:
    auto applySlice(const BSONObj& projSpec,
                    const std::string& path,
                    boost::optional<int> skip,
                    int limit,
                    const Document& input) {
        auto executor = createProjectionExecutor(getExpCtx(), projSpec, {});
        auto expr = make_intrusive<ExpressionInternalFindSlice>(
            getExpCtx(),
            ExpressionFieldPath::parse(
                getExpCtx(), "$$" + kProjectionPostImageVarName, getExpCtx()->variablesParseState),
            path,
            skip,
            limit);
        executor->setRootReplacementExpression(expr);
        return executor->applyTransformation(input);
    }
};

TEST_F(PositionalProjectionExecutionTest, CanApplyPositionalWithInclusionProjection) {
    ASSERT_DOCUMENT_EQ(Document{fromjson("{foo: [6]}")},
                       applyPositional(fromjson("{foo: 1}"),
                                       fromjson("{foo: {$gte: 5}}"),
                                       "foo",
                                       Document{fromjson("{foo: [1,2,6,10]}")}));

    ASSERT_DOCUMENT_EQ(Document{fromjson("{bar:1, foo: [6]}")},
                       applyPositional(fromjson("{bar: 1, foo: 1}"),
                                       fromjson("{bar: 1, foo: {$gte: 5}}"),
                                       "foo",
                                       Document{fromjson("{bar: 1, foo: [1,2,6,10]}")}));
}

TEST_F(PositionalProjectionExecutionTest, AppliesProjectionToPreImage) {
    ASSERT_DOCUMENT_EQ(Document{fromjson("{b: [6], c: 'abc'}")},
                       applyPositional(fromjson("{b: 1, c: 1}"),
                                       fromjson("{a: 1, b: {$gte: 5}}"),
                                       "b",
                                       Document{fromjson("{a: 1, b: [1,2,6,10], c: 'abc'}")}));
}

TEST_F(PositionalProjectionExecutionTest, ShouldAddInclusionFieldsAndWholeDocumentToDependencies) {
    auto executor = createProjectionExecutor(getExpCtx(), fromjson("{bar: 1, _id: 0}"), {});
    auto matchSpec = fromjson("{bar: 1, 'foo.bar': {$gte: 5}}");
    auto matchExpr = CopyableMatchExpression{matchSpec,
                                             getExpCtx(),
                                             std::make_unique<ExtensionsCallbackNoop>(),
                                             MatchExpressionParser::kBanAllSpecialFeatures};
    auto expr = make_intrusive<ExpressionInternalFindPositional>(
        getExpCtx(),
        ExpressionFieldPath::parse(getExpCtx(), "$$ROOT", getExpCtx()->variablesParseState),
        ExpressionFieldPath::parse(
            getExpCtx(), "$$" + kProjectionPostImageVarName, getExpCtx()->variablesParseState),
        "foo.bar",
        std::move(matchExpr));
    executor->setRootReplacementExpression(expr);

    DepsTracker deps;
    executor->addDependencies(&deps);

    ASSERT_EQ(deps.fields.size(), 2UL);
    ASSERT_EQ(deps.fields.count("bar"), 1UL);
    ASSERT_EQ(deps.fields.count("foo.bar"), 1UL);
    ASSERT(deps.needWholeDocument);
}

TEST_F(PositionalProjectionExecutionTest, ShouldConsiderAllPathsAsModified) {
    auto executor = createProjectionExecutor(getExpCtx(), fromjson("{bar: 1, _id: 0}"), {});
    auto matchSpec = fromjson("{bar: 1, 'foo.bar': {$gte: 5}}");
    auto matchExpr = CopyableMatchExpression{matchSpec,
                                             getExpCtx(),
                                             std::make_unique<ExtensionsCallbackNoop>(),
                                             MatchExpressionParser::kBanAllSpecialFeatures};
    auto expr = make_intrusive<ExpressionInternalFindPositional>(
        getExpCtx(),
        ExpressionFieldPath::parse(getExpCtx(), "$$ROOT", getExpCtx()->variablesParseState),
        ExpressionFieldPath::parse(
            getExpCtx(), "$$" + kProjectionPostImageVarName, getExpCtx()->variablesParseState),
        "foo.bar",
        std::move(matchExpr));
    executor->setRootReplacementExpression(expr);

    auto modifiedPaths = executor->getModifiedPaths();
    ASSERT(modifiedPaths.type == DocumentSource::GetModPathsReturn::Type::kAllPaths);
}

TEST_F(SliceProjectionExecutionTest, CanApplySliceWithInclusionProjection) {
    ASSERT_DOCUMENT_EQ(
        Document{fromjson("{foo: [1,2]}")},
        applySlice(
            fromjson("{foo: 1}"), "foo", boost::none, 2, Document{fromjson("{foo: [1,2,6,10]}")}));

    ASSERT_DOCUMENT_EQ(Document{fromjson("{bar:1, foo: [6]}")},
                       applySlice(fromjson("{bar: 1, foo: 1}"),
                                  "foo",
                                  2,
                                  1,
                                  Document{fromjson("{bar: 1, foo: [1,2,6,10]}")}));
}

TEST_F(SliceProjectionExecutionTest, AppliesProjectionToPostImage) {
    ASSERT_DOCUMENT_EQ(Document{fromjson("{b: [1,2], c: 'abc'}")},
                       applySlice(fromjson("{b: 1, c: 1}"),
                                  "b",
                                  boost::none,
                                  2,
                                  Document{fromjson("{a: 1, b: [1,2,6,10], c: 'abc'}")}));
}

TEST_F(SliceProjectionExecutionTest, CanApplySliceAndPositionalProjectionsTogether) {
    auto executor = createProjectionExecutor(getExpCtx(), fromjson("{foo: 1, bar: 1}"), {});
    auto matchSpec = fromjson("{foo: {$gte: 3}}");
    auto matchExpr = CopyableMatchExpression{matchSpec,
                                             getExpCtx(),
                                             std::make_unique<ExtensionsCallbackNoop>(),
                                             MatchExpressionParser::kBanAllSpecialFeatures};
    auto positionalExpr = make_intrusive<ExpressionInternalFindPositional>(
        getExpCtx(),
        ExpressionFieldPath::parse(getExpCtx(), "$$ROOT", getExpCtx()->variablesParseState),
        ExpressionFieldPath::parse(
            getExpCtx(), "$$" + kProjectionPostImageVarName, getExpCtx()->variablesParseState),
        "foo",
        std::move(matchExpr));
    auto sliceExpr =
        make_intrusive<ExpressionInternalFindSlice>(getExpCtx(), positionalExpr, "bar", 1, 1);
    executor->setRootReplacementExpression(sliceExpr);

    ASSERT_DOCUMENT_EQ(
        Document{fromjson("{foo: [3], bar: [6]}")},
        executor->applyTransformation(Document{fromjson("{foo: [1,2,3,4], bar: [5,6,7,8]}")}));
}

TEST_F(SliceProjectionExecutionTest, CanApplySliceWithExclusionProjection) {
    ASSERT_DOCUMENT_EQ(
        Document{fromjson("{foo: [6]}")},
        applySlice(
            fromjson("{bar: 0}"), "foo", 2, 1, Document{fromjson("{bar: 1, foo: [1,2,6,10]}")}));
}

TEST_F(SliceProjectionExecutionTest,
       ShouldAddFieldsAndWholeDocumentToDependenciesWithInclusionProjection) {
    auto executor = createProjectionExecutor(getExpCtx(), fromjson("{bar: 1, _id: 0}"), {});
    auto expr = make_intrusive<ExpressionInternalFindSlice>(
        getExpCtx(),
        ExpressionFieldPath::parse(
            getExpCtx(), "$$" + kProjectionPostImageVarName, getExpCtx()->variablesParseState),
        "foo.bar",
        1,
        1);
    executor->setRootReplacementExpression(expr);

    DepsTracker deps;
    executor->addDependencies(&deps);

    ASSERT_EQ(deps.fields.size(), 1UL);
    ASSERT_EQ(deps.fields.count("bar"), 1UL);
    ASSERT(deps.needWholeDocument);
}

TEST_F(SliceProjectionExecutionTest, ShouldConsiderAllPathsAsModifiedWithInclusionProjection) {
    auto executor = createProjectionExecutor(getExpCtx(), fromjson("{bar: 1}"), {});
    auto expr = make_intrusive<ExpressionInternalFindSlice>(
        getExpCtx(),
        ExpressionFieldPath::parse(
            getExpCtx(), "$$" + kProjectionPostImageVarName, getExpCtx()->variablesParseState),
        "foo.bar",
        1,
        1);
    executor->setRootReplacementExpression(expr);

    auto modifiedPaths = executor->getModifiedPaths();
    ASSERT(modifiedPaths.type == DocumentSource::GetModPathsReturn::Type::kAllPaths);
}

TEST_F(SliceProjectionExecutionTest, ShouldConsiderAllPathsAsModifiedWithExclusionProjection) {
    auto executor = createProjectionExecutor(getExpCtx(), fromjson("{bar: 0}"), {});
    auto expr = make_intrusive<ExpressionInternalFindSlice>(
        getExpCtx(),
        ExpressionFieldPath::parse(
            getExpCtx(), "$$" + kProjectionPostImageVarName, getExpCtx()->variablesParseState),
        "foo.bar",
        1,
        1);
    executor->setRootReplacementExpression(expr);

    auto modifiedPaths = executor->getModifiedPaths();
    ASSERT(modifiedPaths.type == DocumentSource::GetModPathsReturn::Type::kAllPaths);
}

TEST_F(SliceProjectionExecutionTest, ShouldAddWholeDocumentToDependenciesWithExclusionProjection) {
    auto executor = createProjectionExecutor(getExpCtx(), fromjson("{bar: 0}"), {});
    auto expr = make_intrusive<ExpressionInternalFindSlice>(
        getExpCtx(),
        ExpressionFieldPath::parse(
            getExpCtx(), "$$" + kProjectionPostImageVarName, getExpCtx()->variablesParseState),
        "foo.bar",
        1,
        1);
    executor->setRootReplacementExpression(expr);

    DepsTracker deps;
    executor->addDependencies(&deps);

    ASSERT_EQ(deps.fields.size(), 0UL);
    ASSERT(deps.needWholeDocument);
}
}  // namespace mongo::projection_executor
