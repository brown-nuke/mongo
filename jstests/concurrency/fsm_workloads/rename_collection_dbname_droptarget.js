/**
 * rename_collection_dbname_droptarget.js
 *
 * Creates a collection and then repeatedly executes the renameCollection
 * command against it, specifying a different database name in the namespace.
 * Inserts documents into the "to" namespace and specifies dropTarget=true.
 * @tags: [
 *     # Rename between DBs with different shard primary is not supported
 *     assumes_unsharded_collection,
 * ]
 */
export const $config = (function() {
    var data = {
        // Use the workload name as a prefix for the collection name,
        // since the workload name is assumed to be unique.
        prefix: 'rename_collection_dbname_droptarget'
    };

    var states = (function() {
        function uniqueDBName(prefix, tid, num) {
            return prefix + tid + '_' + num;
        }

        function insert(db, collName, numDocs) {
            for (var i = 0; i < numDocs; ++i) {
                var res = db[collName].insert({});
                assert.commandWorked(res);
                assert.eq(1, res.nInserted);
            }
        }

        function init(db, collName) {
            var num = 0;
            this.fromDBName = db.getName() + uniqueDBName(this.prefix, this.tid, num++);
            this.toDBName = db.getName() + uniqueDBName(this.prefix, this.tid, num++);

            var fromDB = db.getSiblingDB(this.fromDBName);
            assert.commandWorked(fromDB.createCollection(collName));
        }

        function rename(db, collName) {
            var fromDB = db.getSiblingDB(this.fromDBName);
            var toDB = db.getSiblingDB(this.toDBName);

            // Clear out the "from" collection and insert 'fromCollCount' documents
            var fromCollCount = 7;
            assert(fromDB[collName].drop());
            assert.commandWorked(fromDB.createCollection(collName));
            insert(fromDB, collName, fromCollCount);

            var toCollCount = 4;
            assert.commandWorked(toDB.createCollection(collName));
            insert(toDB, collName, toCollCount);

            // Verify that 'fromCollCount' documents exist in the "to" collection
            // after the rename occurs
            var renameCommand = {
                renameCollection: this.fromDBName + '.' + collName,
                to: this.toDBName + '.' + collName,
                dropTarget: true
            };

            assert.commandWorked(fromDB.adminCommand(renameCommand));
            assert.eq(fromCollCount, toDB[collName].find().itcount());
            assert.eq(0, fromDB[collName].find().itcount());

            // Swap "to" and "from" collections for next execution
            var temp = this.fromDBName;
            this.fromDBName = this.toDBName;
            this.toDBName = temp;
        }

        return {init: init, rename: rename};
    })();

    var transitions = {init: {rename: 1}, rename: {rename: 1}};

    return {
        threadCount: 10,
        iterations: 20,
        data: data,
        states: states,
        transitions: transitions,
    };
})();
