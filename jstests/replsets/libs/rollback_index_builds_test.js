/**
 * Fixture to test rollback permutations with index builds.
 */

"use strict";

load("jstests/noPassthrough/libs/index_build.js");  // for IndexBuildTest
load('jstests/replsets/libs/rollback_test.js');     // for RollbackTest

class RollbackIndexBuildsTest {
    constructor() {
        this.rollbackTest = new RollbackTest();
    }

    // Given two ordered arrays, returns all permutations of the two using all elements of each.
    static makeSchedules(rollbackOps, indexBuildOps) {
        // Appends to the 'result' array all permutations of the interleavings between two ordered
        // arrays.
        function makeCombinations(listA, listB, result, accum = []) {
            if (listA.length == 0 && listB.length == 0) {
                result.push(accum);
                accum = [];
            }

            if (listA.length) {
                let copy = accum.slice();
                copy.push(listA[0]);
                makeCombinations(listA.slice(1), listB, result, copy);
            }
            if (listB.length) {
                let copy = accum.slice();
                copy.push(listB[0]);
                makeCombinations(listA, listB.slice(1), result, copy);
            }
        }

        let schedules = [];
        // This function is exponential. Limit the number of operations to prevent generating
        // extremely large schedules.
        assert.lte(rollbackOps.length, 5);
        assert.lte(indexBuildOps.length, 5);
        makeCombinations(rollbackOps, indexBuildOps, schedules);
        jsTestLog("Generated " + schedules.length + " schedules ");
        return schedules;
    }

    runSchedules(schedules) {
        const self = this;
        let i = 0;
        schedules.forEach(function(schedule) {
            const collName = "coll_" + i;
            const indexSpec = {a: 1};

            jsTestLog("iteration: " + i + " collection: " + collName +
                      " schedule: " + tojson(schedule));

            const primary = self.rollbackTest.getPrimary();
            const primaryDB = primary.getDB('test');
            const collection = primaryDB.getCollection(collName);
            collection.insert({a: "this is a test"});

            schedule.forEach(function(op) {
                print("Running operation: " + op);
                switch (op) {
                    case "holdStableTimestamp":
                        assert.commandWorked(primary.adminCommand(
                            {configureFailPoint: 'disableSnapshotting', mode: 'alwaysOn'}));
                        break;
                    case "transitionToRollback":
                        const curPrimary = self.rollbackTest.transitionToRollbackOperations();
                        assert.eq(curPrimary, primary);
                        break;
                    case "start":
                        IndexBuildTest.pauseIndexBuilds(primary);
                        IndexBuildTest.startIndexBuild(
                            primary, collection.getFullName(), indexSpec);
                        IndexBuildTest.waitForIndexBuildToStart(primaryDB, collName, "a_1");
                        break;
                    case "commit":
                        IndexBuildTest.resumeIndexBuilds(primary);
                        IndexBuildTest.waitForIndexBuildToStop(primaryDB, collName, "a_1");
                        break;
                    case "drop":
                        collection.dropIndexes(indexSpec);
                        break;
                    default:
                        assert(false, "unknown operation for test: " + op);
                }
            });

            self.rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
            self.rollbackTest.transitionToSyncSourceOperationsDuringRollback();
            try {
                // To speed up the test, defer data validation until the fixture shuts down.
                self.rollbackTest.transitionToSteadyStateOperations(
                    {skipDataConsistencyChecks: true});
            } finally {
                assert.commandWorked(
                    primary.adminCommand({configureFailPoint: 'disableSnapshotting', mode: 'off'}));
            }
            i++;
        });
    }

    stop() {
        this.rollbackTest.stop();
    }
}
