/*
 * Test the test command createUnsplittableCollection. This command is a temporary wrapper on
 * shardCollection that allows you to create unsplittable collection aka tracked unsharded
 * collection. Since we use the same coordinator, we both check the createUnsplittableCollection
 * works and that shardCollection won't generate unsplittable collection.
 * @tags: [
 *   featureFlagTrackUnshardedCollectionsOnShardingCatalog,
 *   multiversion_incompatible,
 *   assumes_balancer_off,
 * ]
 */

const kDbName = "test";

const st = new ShardingTest({shards: 2});
const mongos = st.s;
const shard0 = st.shard0.shardName;
const shard1 = st.shard1.shardName;

// Ensure the db primary is shard0. This will be expected later on.
st.s.adminCommand({enableSharding: kDbName, primaryShard: shard0});

jsTest.log("Running test command createUnsplittableCollection to track an unsharded collection");
{
    const kColl = "first_unsharded_collection";
    const kNssUnsharded = kDbName + "." + kColl;
    assert.commandWorked(mongos.getDB("admin").runCommand({enableSharding: kDbName}));

    let result = st.s.getDB(kDbName).runCommand({createUnsplittableCollection: kColl});
    assert.commandWorked(result);

    // checking consistency
    let configDb = mongos.getDB('config');

    let unshardedColl = configDb.collections.findOne({_id: kNssUnsharded});
    assert.eq(unshardedColl._id, kNssUnsharded);
    assert.eq(unshardedColl._id, kNssUnsharded);
    assert.eq(unshardedColl.unsplittable, true);
    assert.eq(unshardedColl.key, {_id: 1});

    let configChunks = configDb.chunks.find({uuid: unshardedColl.uuid}).toArray();
    assert.eq(configChunks.length, 1);
}

jsTest.log('Check that shardCollection won\'t generate an unsplittable collection');
{
    const kCollSharded = 'sharded_collection';
    const kNssSharded = kDbName + '.' + kCollSharded;

    let result = mongos.adminCommand({shardCollection: kNssSharded, key: {_id: 1}});
    assert.commandWorked(result);

    let shardedColl = mongos.getDB('config').collections.findOne({_id: kNssSharded});
    assert.eq(shardedColl.unsplittable, undefined);
}

st.stop();
