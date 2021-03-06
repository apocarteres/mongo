/**
 *    Copyright (C) 2018 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/query/query_planner_test_fixture.h"

namespace mongo {

const std::string kIndexName = "indexName";

/**
 * A specialization of the QueryPlannerTest fixture which makes it easy to present the planner with
 * a view of the available $** indexes.
 */
class QueryPlannerAllPathsTest : public QueryPlannerTest {
protected:
    void setUp() final {
        QueryPlannerTest::setUp();

        // We're interested in testing plans that use a $** index, so don't generate collection
        // scans.
        params.options &= ~QueryPlannerParams::INCLUDE_COLLSCAN;
    }

    void addAllPathsIndex(BSONObj keyPattern,
                          const std::set<std::string>& multikeyPathSet = {},
                          BSONObj wildcardProjection = BSONObj{},
                          MatchExpression* partialFilterExpr = nullptr) {
        // Convert the set of std::string to a set of FieldRef.
        std::set<FieldRef> multikeyFieldRefs;
        for (auto&& path : multikeyPathSet) {
            ASSERT_TRUE(multikeyFieldRefs.emplace(path).second);
        }
        ASSERT_EQ(multikeyPathSet.size(), multikeyFieldRefs.size());

        const bool isMultikey = !multikeyPathSet.empty();
        BSONObj infoObj = BSON("wildcardProjection" << wildcardProjection);

        params.indices.push_back(IndexEntry{std::move(keyPattern),
                                            IndexType::INDEX_ALLPATHS,
                                            isMultikey,
                                            {},  // multikeyPaths
                                            std::move(multikeyFieldRefs),
                                            false,  // sparse
                                            false,  // unique
                                            IndexEntry::Identifier{kIndexName},
                                            partialFilterExpr,
                                            std::move(infoObj),
                                            nullptr});  // collator
    }
};

//
// Null comparison and existence tests.
//

TEST_F(QueryPlannerAllPathsTest, ExistsTrueQueriesUseAllPathsIndexes) {
    addIndex(BSON("$**" << 1));

    runQuery(fromjson("{x: {$exists: true}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {$_path: 1, x: 1}}}}}");
}

TEST_F(QueryPlannerAllPathsTest, ExistsFalseQueriesDontUseAllPathsIndexes) {
    addIndex(BSON("$**" << 1));

    runQuery(fromjson("{x: {$exists: false}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerAllPathsTest, EqualsNullQueriesDontUseAllPathsIndexes) {
    addIndex(BSON("$**" << 1));

    runQuery(fromjson("{x: {$eq: null}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerAllPathsTest, NotEqualsNullQueriesDontUseAllPathsIndexes) {
    addIndex(BSON("$**" << 1));

    runQuery(fromjson("{x: {$ne: null}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerAllPathsTest, NotEqualsNullAndExistsQueriesUseAllPathsIndexes) {
    addIndex(BSON("$**" << 1));

    runQuery(fromjson("{x: {$ne: null, $exists: true}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {$_path: 1, x: 1}}}}}");
}

TEST_F(QueryPlannerAllPathsTest, EqualsNullAndExistsQueriesUseAllPathsIndexes) {
    addIndex(BSON("$**" << 1));

    runQuery(fromjson("{x: {$eq: null, $exists: true}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {$_path: 1, x: 1}}}}}");
}

TEST_F(QueryPlannerAllPathsTest, EmptyBoundsWithAllPathsIndexes) {
    addIndex(BSON("$**" << 1));

    runQuery(fromjson("{x: {$lte: 5, $gte: 10}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {$_path: 1, x: 1}}}}}");
}

//
// Multikey planning tests.
//

TEST_F(QueryPlannerAllPathsTest, MultiplePredicatesOverMultikeyFieldNoElemMatch) {
    addAllPathsIndex(BSON("$**" << 1), {"a"});
    runQuery(fromjson("{a: {$gt: 0, $lt: 9}}"));

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists(
        "{fetch: {filter: {a: {$gt: 0}}, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [[-Infinity,9,true,false]]}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {a: {$lt: 9}}, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [[0,Infinity,false,true]]}}}}}");
}

TEST_F(QueryPlannerAllPathsTest, MultiplePredicatesOverMultikeyFieldWithElemMatch) {
    addAllPathsIndex(BSON("$**" << 1), {"a"});
    runQuery(fromjson("{a: {$elemMatch: {$gt: 0, $lt: 9}}}"));

    ASSERT_EQUALS(getNumSolutions(), 1U);
    assertSolutionExists(
        "{fetch: {filter: {a: {$elemMatch: {$gt: 0, $lt: 9}}}, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [[0,9,false,false]]}}}}}");
}

TEST_F(QueryPlannerAllPathsTest, MultiplePredicatesOverNonMultikeyFieldWithMultikeyIndex) {
    addAllPathsIndex(BSON("$**" << 1), {"b"});
    runQuery(fromjson("{a: {$gt: 0, $lt: 9}}"));

    ASSERT_EQUALS(getNumSolutions(), 1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {filter: null, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [[0,9,false,false]]}}}}}");
}

TEST_F(QueryPlannerAllPathsTest, MultiplePredicatesOverNestedFieldWithFirstComponentMultikey) {
    addAllPathsIndex(BSON("$**" << 1), {"a"});
    runQuery(fromjson("{'a.b': {$gt: 0, $lt: 9}}"));

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists(
        "{fetch: {filter: {'a.b': {$gt: 0}}, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, 'a.b': 1},"
        "bounds: {'$_path': [['a.b','a.b',true,true]], 'a.b': [[-Infinity,9,true,false]]}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {'a.b': {$lt: 9}}, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, 'a.b': 1},"
        "bounds: {'$_path': [['a.b','a.b',true,true]], 'a.b': [[0,Infinity,false,true]]}}}}}");
}

TEST_F(QueryPlannerAllPathsTest, MultiplePredicatesOverNestedFieldWithElemMatchObject) {
    addAllPathsIndex(BSON("$**" << 1), {"a"});
    runQuery(fromjson("{a: {$elemMatch: {b: {$gt: 0, $lt: 9}}}}"));

    ASSERT_EQUALS(getNumSolutions(), 1U);
    assertSolutionExists(
        "{fetch: {filter: {a: {$elemMatch: {b: {$gt: 0, $lt: 9}}}}, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, 'a.b': 1},"
        "bounds: {'$_path': [['a.b','a.b',true,true]], 'a.b': [[0,9,false,false]]}}}}}");
}

TEST_F(QueryPlannerAllPathsTest,
       MultiplePredicatesOverNestedFieldWithElemMatchObjectBothComponentsMultikey) {
    addAllPathsIndex(BSON("$**" << 1), {"a", "a.b"});
    runQuery(fromjson("{a: {$elemMatch: {b: {$gt: 0, $lt: 9}}}}"));

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists(
        "{fetch: {filter: {a: {$elemMatch: {b: {$gt: 0, $lt: 9}}}}, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, 'a.b': 1},"
        "bounds: {'$_path': [['a.b','a.b',true,true]], 'a.b': [[-Infinity,9,true,false]]}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {a: {$elemMatch: {b: {$gt: 0, $lt: 9}}}}, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, 'a.b': 1},"
        "bounds: {'$_path': [['a.b','a.b',true,true]], 'a.b': [[0,Infinity,false,true]]}}}}}");
}

TEST_F(QueryPlannerAllPathsTest, MultiplePredicatesOverNestedFieldWithTwoElemMatches) {
    addAllPathsIndex(BSON("$**" << 1), {"a", "a.b"});
    runQuery(fromjson("{a: {$elemMatch: {b: {$elemMatch: {$gt: 0, $lt: 9}}}}}"));

    ASSERT_EQUALS(getNumSolutions(), 1U);
    assertSolutionExists(
        "{fetch: {filter: {a: {$elemMatch: {b: {$elemMatch: {$gt: 0, $lt: 9}}}}}, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, 'a.b': 1},"
        "bounds: {'$_path': [['a.b','a.b',true,true]], 'a.b': [[0,9,false,false]]}}}}}");
}

TEST_F(QueryPlannerAllPathsTest, ElemMatchOnInnermostMultikeyPathPermitsTightBounds) {
    addAllPathsIndex(BSON("$**" << 1), {"a", "a.b", "a.b.c"});
    runQuery(fromjson("{'a.b.c': {$elemMatch: {'d.e.f': {$gt: 0, $lt: 9}}}}"));

    ASSERT_EQUALS(getNumSolutions(), 1U);
    assertSolutionExists(
        "{fetch: {filter: {'a.b.c': {$elemMatch: {'d.e.f': {$gt: 0, $lt: 9}}}}, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, 'a.b.c.d.e.f': 1},"
        "bounds: {'$_path': [['a.b.c.d.e.f','a.b.c.d.e.f',true,true]],"
        "'a.b.c.d.e.f': [[0,9,false,false]]}}}}}");
}

TEST_F(QueryPlannerAllPathsTest, AllPredsEligibleForIndexUseGenerateCandidatePlans) {
    addAllPathsIndex(BSON("a.$**" << 1), {"a.b", "a.c"});
    runQuery(
        fromjson("{'a.b': {$gt: 0, $lt: 9}, 'a.c': {$gt: 11, $lt: 20}, d: {$gt: 31, $lt: 40}}"));

    ASSERT_EQUALS(getNumSolutions(), 4U);
    assertSolutionExists(
        "{fetch: {filter: {'a.b':{$gt:0,$lt: 9},'a.c':{$gt:11},d:{$gt:31,$lt:40}}, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, 'a.c': 1},"
        "bounds: {'$_path': [['a.c','a.c',true,true]], 'a.c': [[-Infinity,20,true,false]]}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {'a.b':{$gt:0,$lt: 9},'a.c':{$lt:20},d:{$gt:31,$lt:40}}, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, 'a.c': 1},"
        "bounds: {'$_path': [['a.c','a.c',true,true]], 'a.c': [[11,Infinity,false,true]]}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {'a.b':{$gt:0},'a.c':{$gt:11,$lt:20},d:{$gt:31,$lt:40}}, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, 'a.b': 1},"
        "bounds: {'$_path': [['a.b','a.b',true,true]], 'a.b': [[-Infinity,9,true,false]]}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {'a.b':{$lt:9},'a.c':{$gt:11,$lt:20},d:{$gt:31,$lt:40}}, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, 'a.b': 1},"
        "bounds: {'$_path': [['a.b','a.b',true,true]], 'a.b': [[0,Infinity,false,true]]}}}}}");
}

TEST_F(QueryPlannerAllPathsTest, RangeIndexScan) {
    addAllPathsIndex(BSON("$**" << 1));
    runQuery(fromjson("{a: {$gt: 0, $lt: 9}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [[0,9,false,false]]}}}}}");
}

TEST_F(QueryPlannerAllPathsTest, RangeIndexScanEmptyRange) {
    addAllPathsIndex(BSON("$**" << 1));
    runQuery(fromjson("{a: {$gt: 9, $lt: 0}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], 'a': []}}}}}");
}

TEST_F(QueryPlannerAllPathsTest, RangeIndexScanMinKeyMaxKey) {
    addAllPathsIndex(BSON("$**" << 1));
    runQuery(fromjson("{a: {$gt: {$minKey: 1}, $lt: {$maxKey: 1}}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true], ['a.', 'a/', true, false]], 'a': [['MinKey', "
        "'MaxKey', true, true]]}}}}}");
}

TEST_F(QueryPlannerAllPathsTest, RangeIndexScanNestedField) {
    addAllPathsIndex(BSON("$**" << 1));
    runQuery(fromjson("{'a.b': {$gt: 0, $lt: 9}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, 'a.b': 1},"
        "bounds: {'$_path': [['a.b','a.b',true,true]], 'a.b': [[0,9,false,false]]}}}}}");
}

TEST_F(QueryPlannerAllPathsTest, EqualityIndexScan) {
    addAllPathsIndex(BSON("$**" << 1));
    runQuery(fromjson("{a: {$eq: 5}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [[5,5,true,true]]}}}}}");
}

TEST_F(QueryPlannerAllPathsTest, EqualityIndexScanOverNestedField) {
    addAllPathsIndex(BSON("$**" << 1));
    runQuery(fromjson("{'a.b': {$eq: 5}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, 'a.b': 1},"
        "bounds: {'$_path': [['a.b','a.b',true,true]], 'a.b': [[5,5,true,true]]}}}}}");
}

TEST_F(QueryPlannerAllPathsTest, ExprEqCanUseIndex) {
    addAllPathsIndex(BSON("$**" << 1));
    runQuery(fromjson("{a: {$_internalExprEq: 1}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [[1,1,true,true]]}}}}}");
}

TEST_F(QueryPlannerAllPathsTest, ExprEqCanUseSparseIndexForEqualityToNull) {
    addAllPathsIndex(BSON("$**" << 1));
    runQuery(fromjson("{a: {$_internalExprEq: null}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {a: {$_internalExprEq: null}}, node: {ixscan: {pattern: {'$_path': 1, a: "
        "1}, bounds: {'$_path': [['a','a',true,true]], a: [[undefined,undefined,true,true], "
        "[null,null,true,true]]}}}}}");
}

TEST_F(QueryPlannerAllPathsTest, PrefixRegex) {
    addIndex(BSON("$**" << 1));
    runQuery(fromjson("{a: /^foo/}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]],"
        "a: [['foo','fop',true,false], [/^foo/,/^foo/,true,true]]}}}}}");
}

TEST_F(QueryPlannerAllPathsTest, NonPrefixRegex) {
    addIndex(BSON("$**" << 1));
    runQuery(fromjson("{a: /foo/}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {filter: {a: /foo/}, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]],"
        "a: [['',{},true,false], [/foo/,/foo/,true,true]]}}}}}");
}

TEST_F(QueryPlannerAllPathsTest, GreaterThan) {
    addAllPathsIndex(BSON("$**" << 1));
    runQuery(fromjson("{a: {$gt: 5}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [[5,Infinity,false,true]]}}}}}");
}

TEST_F(QueryPlannerAllPathsTest, GreaterThanEqualTo) {
    addAllPathsIndex(BSON("$**" << 1));
    runQuery(fromjson("{a: {$gte: 5}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [[5,Infinity,true,true]]}}}}}");
}

TEST_F(QueryPlannerAllPathsTest, LessThan) {
    addAllPathsIndex(BSON("$**" << 1));
    runQuery(fromjson("{a: {$lt: 5}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [[-Infinity,5,true,false]]}}}}}");
}

TEST_F(QueryPlannerAllPathsTest, LessThanEqualTo) {
    addAllPathsIndex(BSON("$**" << 1));
    runQuery(fromjson("{a: {$lte: 5}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [[-Infinity,5,true,true]]}}}}}");
}

TEST_F(QueryPlannerAllPathsTest, Mod) {
    addAllPathsIndex(BSON("$**" << 1));
    runQuery(fromjson("{a: {$mod: [2, 0]}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: "
        "{ixscan: {filter: {a: {$mod: [2, 0]}}, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [[NaN,Infinity, true, true]]}}}}}");
}

TEST_F(QueryPlannerAllPathsTest, ExistsTrue) {
    addAllPathsIndex(BSON("$**" << 1));
    runQuery(fromjson("{x: {$exists: true}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, x: 1},"
        "bounds: {'$_path': [['x','x',true,true],['x.','x/',true,false]], x: "
        "[['MinKey','MaxKey',true,true]]}}}}}");
}

TEST_F(QueryPlannerAllPathsTest, ExistsFalseDoesNotUseIndex) {
    addAllPathsIndex(BSON("$**" << 1));
    runQuery(fromjson("{x: {$exists: false}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerAllPathsTest, AndEqualityWithTwoPredicatesIndexesOnePath) {
    addAllPathsIndex(BSON("$**" << 1));
    runQuery(fromjson("{a: 5, b: 10}"));

    assertNumSolutions(2U);
    assertSolutionExists(
        "{fetch: {filter: {b: {$eq: 10}}, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [[5,5,true,true]]}}}}}");
}

TEST_F(QueryPlannerAllPathsTest, OrEqualityWithTwoPredicatesUsesTwoPaths) {
    addAllPathsIndex(BSON("$**" << 1));
    runQuery(fromjson("{$or: [{a: 5}, {b: 10}]}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: {or: {nodes: ["
        "{ixscan: {filter: null, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [[5,5,true,true]]}}}, "
        "{ixscan: {filter: null, pattern: {'$_path': 1, b: 1},"
        "bounds: {'$_path': [['b','b',true,true]], b: [[10,10,true,true]]}}}]}}}}");
    ;
}

TEST_F(QueryPlannerAllPathsTest, OrWithOneRegularAndOneAllPathsIndexPathUsesTwoIndexes) {
    addAllPathsIndex(BSON("a.$**" << 1));
    addIndex(BSON("b" << 1));
    runQuery(fromjson("{$or: [{a: 5}, {b: 10}]}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: {or: {nodes: ["
        "{ixscan: {filter: null, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [[5,5,true,true]]}}}, "
        "{ixscan: {filter: null, pattern: {b: 1},"
        "bounds: {b: [[10,10,true,true]]}}}]}}}}");
    ;
}

TEST_F(QueryPlannerAllPathsTest, BasicSkip) {
    addAllPathsIndex(BSON("$**" << 1));
    runQuerySkipNToReturn(BSON("a" << 5), 8, 0);

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: {skip: {n: 8, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [[5,5,true,true]]}}}}}}}");
}

TEST_F(QueryPlannerAllPathsTest, CoveredSkip) {
    addAllPathsIndex(BSON("$**" << 1));
    runQuerySortProjSkipNToReturn(fromjson("{a: 5}"), BSONObj(), fromjson("{_id: 0, a: 1}"), 8, 0);

    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1}, node: {skip: {n: 8, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [[5,5,true,true]]}}}}}}}");
}

TEST_F(QueryPlannerAllPathsTest, BasicLimit) {
    addAllPathsIndex(BSON("$**" << 1));
    runQuerySkipNToReturn(BSON("a" << 5), 0, -5);

    assertNumSolutions(1U);
    assertSolutionExists(
        "{limit: {n: 5, node: {fetch: {filter: null, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [[5,5,true,true]]}}}}}}}");
}

TEST_F(QueryPlannerAllPathsTest, BasicCovering) {
    addAllPathsIndex(BSON("$**" << 1));
    runQuerySortProj(fromjson("{ x : {$gt: 1}}"), BSONObj(), fromjson("{_id: 0, x: 1}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, x: 1}, node: {ixscan: {filter: null, pattern: {'$_path': 1, x: 1},"
        "bounds: {'$_path': [['x','x',true,true]], x: [[1,Infinity,false,true]]}}}}}");
}

TEST_F(QueryPlannerAllPathsTest, DottedFieldCovering) {
    addAllPathsIndex(BSON("$**" << 1));
    runQuerySortProj(fromjson("{'a.b': 5}"), BSONObj(), fromjson("{_id: 0, 'a.b': 1}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, 'a.b': 1}, node: {ixscan: {filter: null, pattern: {'$_path': 1, "
        "'a.b': 1},"
        "bounds: {'$_path': [['a.b','a.b',true,true]], 'a.b': [[5,5,true,true]]}}}}}");
}

TEST_F(QueryPlannerAllPathsTest, CoveredIxscanForCountOnIndexedPath) {
    params.options = QueryPlannerParams::IS_COUNT;
    addAllPathsIndex(BSON("$**" << 1));
    runQuery(fromjson("{a: 5}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{ixscan: {filter: null, pattern: {'$_path': 1, 'a': 1},"
        "bounds: {'$_path': [['a','a',true,true]], 'a': [[5,5,true,true]]}}}");
}

TEST_F(QueryPlannerAllPathsTest, InBasic) {
    addAllPathsIndex(BSON("$**" << 1));
    runQuery(fromjson("{a: {$in: [1, 2]}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, "
        "node: {ixscan: {filter: null, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [[1,1,true,true],[2,2,true,true]]}}}}}");
}

// Logically equivalent to the preceding $in query.
// Indexed solution should be the same.
TEST_F(QueryPlannerAllPathsTest, InBasicOrEquivalent) {
    addAllPathsIndex(BSON("$**" << 1));
    runQuery(fromjson("{$or: [{a: 1}, {a: 2}]}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, "
        "node: {ixscan: {filter: null, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [[1,1,true,true],[2,2,true,true]]}}}}}");
}

TEST_F(QueryPlannerAllPathsTest, PartialIndexCanAnswerPredicateOnFilteredField) {
    auto filterObj = fromjson("{a: {$gt: 0}}");
    auto filterExpr = QueryPlannerTest::parseMatchExpression(filterObj);
    addAllPathsIndex(BSON("$**" << 1), {}, BSONObj{}, filterExpr.get());

    runQuery(fromjson("{a: {$gte: 5}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [[5,Infinity,true,true]]}}}}}");

    runQuery(fromjson("{a: 5}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [[5,5,true,true]]}}}}}");

    runQuery(fromjson("{a: {$gte: 1, $lte: 10}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [[1,10,true,true]]}}}}}");
}

TEST_F(QueryPlannerAllPathsTest, PartialIndexDoesNotAnswerPredicatesExcludedByFilter) {
    // Must keep 'filterObj' around since match expressions will store pointers into the BSON they
    // were parsed from.
    auto filterObj = fromjson("{a: {$gt: 0}}");
    auto filterExpr = QueryPlannerTest::parseMatchExpression(filterObj);
    addAllPathsIndex(BSON("$**" << 1), {}, BSONObj{}, filterExpr.get());

    runQuery(fromjson("{a: {$gte: -1}}"));
    assertHasOnlyCollscan();

    runQuery(fromjson("{a: {$lte: 10}}"));
    assertHasOnlyCollscan();

    runQuery(fromjson("{a: {$eq: 0}}"));
    assertHasOnlyCollscan();
}

TEST_F(QueryPlannerAllPathsTest, PartialIndexCanAnswerPredicateOnUnrelatedField) {
    auto filterObj = fromjson("{a: {$gt: 0}}");
    auto filterExpr = QueryPlannerTest::parseMatchExpression(filterObj);
    addAllPathsIndex(BSON("$**" << 1), {}, BSONObj{}, filterExpr.get());

    // Test when the field query is not included by the partial filter expression.
    runQuery(fromjson("{b: {$gte: -1}, a: {$gte: 5}}"));
    assertNumSolutions(2U);
    assertSolutionExists(
        "{fetch: {filter: {a: {$gte: 5}}, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, b: 1},"
        "bounds: {'$_path': [['b','b',true,true]], b: [[-1,Infinity,true,true]]}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {b: {$gte: -1}}, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [[5,Infinity,true,true]]}}}}}");
}

TEST_F(QueryPlannerAllPathsTest, PartialIndexWithExistsTrueFilterCanAnswerExistenceQuery) {
    auto filterObj = fromjson("{x: {$exists: true}}");
    auto filterExpr = QueryPlannerTest::parseMatchExpression(filterObj);
    addAllPathsIndex(BSON("$**" << 1), {}, BSONObj{}, filterExpr.get());
    runQuery(fromjson("{x: {$exists: true}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, x: 1},"
        "bounds: {'$_path': [['x','x',true,true],['x.','x/',true,false]], x: "
        "[['MinKey','MaxKey',true,true]]}}}}}");
}

//
// Index intersection tests.
//

TEST_F(QueryPlannerAllPathsTest, AllPathsIndexDoesNotParticipateInIndexIntersection) {
    // Enable both AND_SORTED and AND_HASH index intersection for this test.
    params.options |= QueryPlannerParams::INDEX_INTERSECTION;
    internalQueryPlannerEnableHashIntersection.store(true);

    // Add two standard single-field indexes.
    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1));

    // Run a point query on both fields and confirm that an AND_SORTED plan is generated.
    runQuery(fromjson("{a:10, b:10}"));
    // Three plans are generated: one IXSCAN for each index, and an AND_SORTED on both.
    ASSERT_EQUALS(getNumSolutions(), 3U);
    assertSolutionExists(
        "{fetch: {filter: {a:10}, node: {ixscan: {filter: null, pattern: {b:1}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {b:10}, node: {ixscan: {filter: null, pattern: {a:1}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {a:10, b:10}, node: {andSorted: {nodes: [{ixscan: {filter: null, "
        "pattern: {a:1}}},{ixscan: {filter: null, pattern: {b:1}}}]}}}}");

    // Run a range query on both fields and confirm that an AND_HASH plan is generated.
    runQuery(fromjson("{a:{$gt: 10}, b:{$gt: 10}}"));
    // Three plans are generated: one IXSCAN for each index, and an AND_HASH on both.
    ASSERT_EQUALS(getNumSolutions(), 3U);
    assertSolutionExists(
        "{fetch: {filter: {a:{$gt: 10}}, node: {ixscan: {filter: null, pattern: {b:1}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {b:{$gt: 10}}, node: {ixscan: {filter: null, pattern: {a:1}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {a:{$gt: 10}, b:{$gt: 10}}, node: {andHash: {nodes: [{ixscan: "
        "{filter: null, pattern: {a:1}}},{ixscan: {filter: null, pattern: {b:1}}}]}}}}");

    // Now add a $** index and re-run the tests.
    addAllPathsIndex(BSON("$**" << 1));

    // First re-run the AND_SORTED test.
    runQuery(fromjson("{a:10, b:10}"));
    // Solution count has increased from 3 to 5, as $** 'duplicates' the {a:1} and {b:1} IXSCANS.
    ASSERT_EQUALS(getNumSolutions(), 5U);
    assertSolutionExists(
        "{fetch: {filter: {a:10}, node: {ixscan: {filter: null, pattern: {b:1}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {b:10}, node: {ixscan: {filter: null, pattern: {a:1}}}}}");
    // The previous AND_SORTED solution is still present...
    assertSolutionExists(
        "{fetch: {filter: {a:10, b:10}, node: {andSorted: {nodes: [{ixscan: {filter: null, "
        "pattern: {a:1}}},{ixscan: {filter: null, pattern: {b:1}}}]}}}}");
    // ... but there are no additional AND_SORTED solutions contributed by the $** index.
    assertSolutionExists(
        "{fetch: {filter: {a:10}, node: {ixscan: {filter: null, pattern: {$_path:1, b:1}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {b:10}, node: {ixscan: {filter: null, pattern: {$_path:1, a:1}}}}}");

    // Now re-run the AND_HASH test.
    runQuery(fromjson("{a:{$gt: 10}, b:{$gt: 10}}"));
    // Solution count has increased from 3 to 5, as $** 'duplicates' the {a:1} and {b:1} IXSCANS.
    ASSERT_EQUALS(getNumSolutions(), 5U);
    assertSolutionExists(
        "{fetch: {filter: {a:{$gt:10}}, node: {ixscan: {filter: null, pattern: {b:1}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {b:{$gt:10}}, node: {ixscan: {filter: null, pattern: {a:1}}}}}");
    // The previous AND_HASH solution is still present...
    assertSolutionExists(
        "{fetch: {filter: {a:{$gt:10}, b:{$gt:10}}, node: {andHash: {nodes: [{ixscan: "
        "{filter: null, pattern: {a:1}}},{ixscan: {filter: null, pattern: {b:1}}}]}}}}");
    // ... but there are no additional AND_HASH solutions contributed by the $** index.
    assertSolutionExists(
        "{fetch: {filter:{a:{$gt:10}}, node: {ixscan: {filter: null, pattern: {$_path:1, b:1}}}}}");
    assertSolutionExists(
        "{fetch: {filter:{b:{$gt:10}}, node: {ixscan: {filter: null, pattern: {$_path:1, a:1}}}}}");
}

//
// AllPaths and $text index tests.
//

TEST_F(QueryPlannerAllPathsTest, AllPathsIndexDoesNotSupplyCandidatePlanForTextSearch) {
    addAllPathsIndex(BSON("$**" << 1));
    addIndex(BSON("a" << 1 << "_fts"
                      << "text"
                      << "_ftsx"
                      << 1));

    // Confirm that the allPaths index generates candidate plans for queries which do not include a
    // $text predicate.
    runQuery(fromjson("{a: 10, b: 10}"));
    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists(
        "{fetch: {filter: {b: 10}, node: {ixscan: {filter: null, pattern: {'$_path': 1, a: 1}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {a: 10}, node: {ixscan: {filter: null, pattern: {'$_path': 1, b: 1}}}}}");

    // Confirm that the allPaths index does not produce any candidate plans when a query includes a
    // $text predicate, even for non-$text predicates which may be present in the query.
    runQuery(fromjson("{a: 10, b: 10, $text: {$search: 'banana'}}"));
    ASSERT_EQUALS(getNumSolutions(), 1U);
    assertSolutionExists(
        "{fetch: {filter: {b: 10}, node: {text: {prefix: {a: 10}, search: 'banana'}}}}");
}

TEST_F(QueryPlannerAllPathsTest, AllPathsDoesNotSupportNegationPredicate) {
    // AllPaths indexes can't support negation queries because they are sparse, and {a: {$ne: 5}}
    // will match documents which don't have an "a" field.
    addAllPathsIndex(BSON("$**" << 1));
    runQuery(fromjson("{a: {$ne: 5}}"));
    assertHasOnlyCollscan();

    runQuery(fromjson("{a: {$not: {$gt: 3, $lt: 5}}}"));
    assertHasOnlyCollscan();
}

TEST_F(QueryPlannerAllPathsTest,
       AllPathsDoesNotSupportNegationPredicateInsideElemMatchMultiKeyPath) {
    // Logically, there's no reason a (sparse) allPaths index could not support a negation inside a
    // "$elemMatch value", but it is not something we've implemented.
    addAllPathsIndex(BSON("$**" << 1), {"a"});
    runQuery(fromjson("{a: {$elemMatch: {$ne: 5}}}"));
    assertHasOnlyCollscan();

    runQuery(fromjson("{a: {$elemMatch: {$not: {$gt: 3, $lt: 5}}}}"));
    assertHasOnlyCollscan();
}

TEST_F(QueryPlannerAllPathsTest, AllPathsDoesNotSupportNegationPredicateInsideElemMatch) {
    // Test the case where we use $elemMatch on a path which isn't even multikey. In this case,
    // we'd know up front that the results would be empty, but this is not an optimization we
    // support.
    addAllPathsIndex(BSON("$**" << 1));
    runQuery(fromjson("{a: {$elemMatch: {$ne: 5}}}"));
    assertHasOnlyCollscan();

    runQuery(fromjson("{a: {$elemMatch: {$not: {$gt: 3, $lt: 5}}}}"));
    assertHasOnlyCollscan();
}

//
// Hinting with all paths index tests.
//

TEST_F(QueryPlannerTest, ChooseAllPathsIndexHint) {
    addIndex(BSON("$**" << 1));
    addIndex(BSON("x" << 1));

    runQueryHint(fromjson("{x: {$eq: 1}}"), BSON("$**" << 1));

    assertNumSolutions(1U);
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {$_path: 1, x: 1}}}}}");
}

TEST_F(QueryPlannerTest, ChooseAllPathsIndexHintByName) {
    StringData allPaths = "allPaths";
    CollatorInterface* nullCollator = nullptr;
    addIndex(BSON("$**" << 1), nullCollator, allPaths);
    addIndex(BSON("x" << 1));

    runQueryHint(fromjson("{x: {$eq: 1}}"),
                 BSON("$hint"
                      << "allPaths"));

    assertNumSolutions(1U);
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {$_path: 1, x: 1}}}}}");
}

TEST_F(QueryPlannerTest, ChooseAllPathsIndexHintWithPath) {
    addIndex(BSON("x.$**" << 1));
    addIndex(BSON("x" << 1));

    runQueryHint(fromjson("{x: {$eq: 1}}"), BSON("x.$**" << 1));

    assertNumSolutions(1U);
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {$_path: 1, x: 1}}}}}");
}

TEST_F(QueryPlannerTest, ChooseAllPathsIndexHintWithOr) {
    addIndex(BSON("$**" << 1));
    addIndex(BSON("x" << 1 << "y" << 1));

    runQueryHint(fromjson("{$or: [{x: 1}, {y: 1}]}"), BSON("$**" << 1));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {node: {or: {nodes: [{ixscan: {pattern: {$_path: 1, x: 1}}},"
        " {ixscan: {pattern: {$_path: 1, y: 1}}}]}}}}");
}

TEST_F(QueryPlannerTest, ChooseAllPathsIndexHintWithCompoundIndex) {
    addIndex(BSON("$**" << 1));
    addIndex(BSON("x" << 1 << "y" << 1));

    runQueryHint(fromjson("{x: 1, y: 1}"), BSON("$**" << 1));

    assertNumSolutions(2U);
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {$_path: 1, x: 1}}}}}");
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {$_path: 1, y: 1}}}}}");
}

TEST_F(QueryPlannerTest, QueryNotInAllPathsIndexHint) {
    addIndex(BSON("a.$**" << 1));
    addIndex(BSON("x" << 1));

    runQueryHint(fromjson("{x: {$eq: 1}}"), BSON("a.$**" << 1));
    assertNumSolutions(0U);
}

TEST_F(QueryPlannerTest, AllPathsIndexDoesNotExist) {
    addIndex(BSON("x" << 1));

    runInvalidQueryHint(fromjson("{x: {$eq: 1}}"), BSON("$**" << 1));
}

TEST_F(QueryPlannerTest, AllPathsIndexHintWithPartialFilter) {
    auto filterObj = fromjson("{a: {$gt: 100}}");
    auto filterExpr = QueryPlannerTest::parseMatchExpression(filterObj);
    addIndex(BSON("$**" << 1), filterExpr.get());

    runQueryHint(fromjson("{a: {$eq: 1}}"), BSON("$**" << 1));
    assertNumSolutions(0U);
}

TEST_F(QueryPlannerTest, MultipleAllPathsIndexesHintWithPartialFilter) {
    auto filterObj = fromjson("{a: {$gt: 100}, b: {$gt: 100}}");
    auto filterExpr = QueryPlannerTest::parseMatchExpression(filterObj);
    addIndex(BSON("$**" << 1), filterExpr.get());

    runQueryHint(fromjson("{a: {$eq: 1}, b: {$eq: 1}}"), BSON("$**" << 1));
    assertNumSolutions(0U);
}

TEST_F(QueryPlannerAllPathsTest, AllPathsIndexesDoNotSupportObjectEquality) {
    addIndex(BSON("$**" << 1));

    runQuery(fromjson("{x: {abc: 1}}"));
    assertHasOnlyCollscan();

    runQuery(fromjson("{$or: [{z: {abc: 1}}]}"));
    assertHasOnlyCollscan();

    // We can only use the index for the predicate on 'x'.
    runQuery(fromjson("{x: 5, y: {abc: 1}}"));
    assertNumSolutions(1U);
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {$_path: 1, x: 1}}}}}");
}

TEST_F(QueryPlannerAllPathsTest, AllPathsIndexesDoNotSupportObjectInequality) {
    addIndex(BSON("$**" << 1));

    runQuery(fromjson("{x: {$lt: {abc: 1}}}"));
    assertHasOnlyCollscan();
    runQuery(fromjson("{x: {$lte: {abc: 1}}}"));
    assertHasOnlyCollscan();
    runQuery(fromjson("{x: {$gte: {abc: 1}}}"));
    assertHasOnlyCollscan();
    runQuery(fromjson("{x: {$gt: {abc: 1}}}"));
    assertHasOnlyCollscan();
    runQuery(fromjson("{x: {ne: {abc: 1}}}"));
    assertHasOnlyCollscan();

    runQuery(fromjson("{x: {$lt: [1, 2, 'a string']}}"));
    assertHasOnlyCollscan();
    runQuery(fromjson("{x: {$lte: [1, 2, 'a string']}}"));
    assertHasOnlyCollscan();
    runQuery(fromjson("{x: {$gte: [1, 2, 'a string']}}"));
    assertHasOnlyCollscan();
    runQuery(fromjson("{x: {$gt: [1, 2, 'a string']}}"));
    assertHasOnlyCollscan();
    runQuery(fromjson("{x: {ne: [1, 2, 'a string']}}"));
    assertHasOnlyCollscan();

    runQuery(fromjson("{$or: [{z: {$ne: {abc: 1}}}]}"));
    assertHasOnlyCollscan();

    runQuery(fromjson("{$and: [{x: 5}, {$or: [{x: 1}, {y: {abc: 1}}]}]}"));
    assertNumSolutions(1U);
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {$_path: 1, x: 1}}}}}");
}

TEST_F(QueryPlannerAllPathsTest, AllPathsIndexesDoNotSupportInWithUnsupportedValues) {
    addIndex(BSON("$**" << 1));

    runQuery(fromjson("{x: {$in: [1, 2, 3, {abc: 1}]}}"));
    assertHasOnlyCollscan();
    runQuery(fromjson("{x: {$in: [1, 2, 3, ['a', 'b', 'c']]}}"));
    assertHasOnlyCollscan();
    runQuery(fromjson("{x: {$in: [1, 2, 3, null]}}"));
    assertHasOnlyCollscan();
}

TEST_F(QueryPlannerAllPathsTest, AllPathsIndexesSupportElemMatchWithNull) {
    addIndex(BSON("$**" << 1));

    // Simple case.
    runQuery(fromjson("{x: {$elemMatch: {$lt: 5, $gt: 0}}}"));
    assertNumSolutions(1U);
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {$_path: 1, x: 1}}}}}");

    // null inside an $in inside an $elemMatch is supported by the allPaths index, since it means
    // we're searching for an explicit null value.
    runQuery(fromjson("{x: {$elemMatch: {$in: [1, 2, 3, null]}}}"));
    assertNumSolutions(1U);
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {$_path: 1, x: 1}}}}}");
}

TEST_F(QueryPlannerAllPathsTest, AllPathsIndexesDoNotSupportElemMatchWithUnsupportedValues) {
    runQuery(fromjson("{x: {$elemMatch: {$eq: ['a', 'b', 'c']}}}"));
    assertHasOnlyCollscan();

    // An object or array inside an $in inside a $elemMatch is not supported by the index.
    runQuery(fromjson("{x: {$elemMatch: {$in: [1, 2, 3, {a: 1}]}}}"));
    assertHasOnlyCollscan();

    runQuery(fromjson("{x: {$elemMatch: {$in: [1, 2, 3, ['a', 'b', 'c']]}}}"));
    assertHasOnlyCollscan();
}

TEST_F(QueryPlannerAllPathsTest, AllPathsIndexesDoNotSupportElemMatchObject) {
    runQuery(fromjson("{x: {$elemMatch: {a: 1}}}"));
    assertHasOnlyCollscan();
}

TEST_F(QueryPlannerAllPathsTest, AllPathsIndexCanProvideNonBlockingSort) {
    addAllPathsIndex(BSON("$**" << 1));

    runQuerySortProj(fromjson("{a: 1}"), BSON("a" << 1), BSONObj());
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {'$_path': 1, a: 1}, "
        "bounds: {'$_path': [['a','a',true,true]], a: [[1,1,true,true]]}}}}}");
}

TEST_F(QueryPlannerAllPathsTest,
       AllPathsIndexCanProvideNonBlockingSortWhenFilterIncludesAdditionalFields) {
    addAllPathsIndex(BSON("$**" << 1));

    runQuerySortProj(fromjson("{a: {$gte: 3}, b: 1}"), BSON("a" << 1), BSONObj());
    assertNumSolutions(2U);
    // The non-blocking sort solution.
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {'$_path': 1, a: 1}, "
        "bounds: {'$_path': [['a','a',true,true]], a: [[3,Infinity,true,true]]}}}}}");

    // A blocking sort solution (by doing a scan with a filter on 'b').
    assertSolutionExists(
        "{sort: {pattern: {a: 1}, limit: 0, node: {sortKeyGen: {node: "
        "{fetch: {filter: {a: {$gte: 3}}, node: "
        "{ixscan: {pattern: {'$_path': 1, b: 1},"
        "bounds: {'$_path': [['b','b',true,true]], b: [[1, 1, true, true]]}}}}}}}}}");
}

TEST_F(QueryPlannerAllPathsTest, AllPathsIndexMustUseBlockingSortWithElemMatch) {
    addAllPathsIndex(BSON("$**" << 1), {"a"});

    runQuerySortProj(fromjson("{a: {$elemMatch: {$eq: 1}}}"), BSON("a" << 1), BSONObj());
    assertNumSolutions(1U);
    assertSolutionExists(
        "{sort: {pattern: {a: 1}, limit: 0, node: {sortKeyGen: {node: "
        "{fetch: {filter: {a: {$elemMatch: {$eq: 1}}}, node: "
        "{ixscan: {pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [[1, 1, true, true]]}}}}}}}}}");
}

TEST_F(QueryPlannerAllPathsTest, AllPathsIndexMustUseBlockingSortWithCompoundSort) {
    addAllPathsIndex(BSON("$**" << 1));

    runQuerySortProj(fromjson("{a: {$lte: 3}}"), BSON("a" << 1 << "b" << 1), BSONObj());
    assertNumSolutions(1U);
    assertSolutionExists(
        "{sort: {pattern: {a: 1, b: 1}, limit: 0, node: {sortKeyGen: {node: "
        "{fetch: {filter: null, node: "
        "{ixscan: {pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [[-Infinity, 3, true, true]]}}}}}}}}}");
}

TEST_F(QueryPlannerAllPathsTest, AllPathsIndexMustUseBlockingSortWithExistsQueries) {
    addAllPathsIndex(BSON("$**" << 1));

    runQuerySortProj(fromjson("{a: {$exists: true}}"), BSON("a" << 1), BSONObj());
    assertNumSolutions(1U);
    assertSolutionExists(
        "{sort: {pattern: {a: 1}, limit: 0, node: {sortKeyGen: {node: "
        "{fetch: {filter: null, node: "
        "{ixscan: {pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [['MinKey', 'MaxKey', true, "
        "true]]}}}}}}}}}");
}

TEST_F(QueryPlannerAllPathsTest, AllPathsIndexMustUseBlockingSortWhenFilterNotPresent) {
    // Since there's no filter on the field that we're sorting by, we cannot use an index scan to
    // answer the query as $** indexes are sparse.
    runQuerySortProj(BSONObj(), fromjson("{a: 1}"), BSONObj());
    assertSolutionExists(
        "{sort: {pattern: {a: 1}, limit: 0, node: {sortKeyGen: {node: "
        "{cscan: {dir: 1}}}}}}");
}

TEST_F(QueryPlannerAllPathsTest, AllPathsIndexMustUseBlockingSortWhenFilterDoesNotIncludeSortKey) {
    addAllPathsIndex(BSON("$**" << 1));

    runQuerySortProj(fromjson("{b: 1, c: 1}"), fromjson("{a: 1}"), BSONObj());
    assertNumSolutions(2U);
    assertSolutionExists(
        "{sort: {pattern: {a: 1}, limit: 0, node: {sortKeyGen: {node: "
        "{fetch: {filter: {c: 1}, node: "
        "{ixscan: {pattern: {'$_path': 1, b: 1},"
        "bounds: {'$_path': [['b','b',true,true]], b: [[1, 1, true, true]]}}}}}}}}}");
    assertSolutionExists(
        "{sort: {pattern: {a: 1}, limit: 0, node: {sortKeyGen: {node: "
        "{fetch: {filter: {b: 1}, node: "
        "{ixscan: {pattern: {'$_path': 1, c: 1},"
        "bounds: {'$_path': [['c','c',true,true]], c: [[1, 1, true, true]]}}}}}}}}}");
}

TEST_F(QueryPlannerAllPathsTest, AllPathsIndexMustUseBlockingSortWhenFieldIsNotIncluded) {
    addAllPathsIndex(BSON("$**" << 1), {}, BSON("b" << 0));

    runQuerySortProj(fromjson("{b: 1}"), fromjson("{b: 1}"), BSONObj());
    assertNumSolutions(1U);
    assertSolutionExists(
        "{sort: {pattern: {b: 1}, limit: 0, node: "
        "{sortKeyGen: {node: "
        "{cscan: {dir: 1, filter: {b: 1}}}"
        "}}}}");
}

// TODO SERVER-35335: Add testing for Min/Max.
// TODO SERVER-36517: Add testing for DISTINCT_SCAN.
// TODO SERVER-35331: Add testing for hints.

}  // namespace mongo
