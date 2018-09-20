/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#include <gtest/gtest.h>

#include "logdevice/common/debug.h"
#include "logdevice/common/configuration/ConfigParser.h"
#include "logdevice/common/event_log/EventLogStateMachine.h"
#include "logdevice/include/Client.h"
#include "logdevice/test/utils/IntegrationTestBase.h"
#include "logdevice/test/utils/IntegrationTestUtils.h"

using namespace facebook::logdevice;

namespace facebook { namespace logdevice {

class RebuildingSupervisorIntegrationTest : public IntegrationTestBase {};

// Counts number of rebuildings triggered by the rebuilding supervisor
static int count_requested_rebuildings(IntegrationTestUtils::Cluster* cluster) {
  int rebuildings = 0;
  for (auto& it : cluster->getNodes()) {
    auto& node = *it.second;
    if (!node.isRunning()) {
      continue; // node is dead
    }

    auto stats = node.stats();
    rebuildings += stats["shard_rebuilding_triggered"];
  }
  return rebuildings;
}

// Checks that rebuilding is requested for `shards' and nothing else.
static void expect_rebuildings(std::set<ShardID> shards,
                               IntegrationTestUtils::Cluster* cluster) {
  auto client = cluster->createClient();

  // Wait for rebuildings to be requested.
  wait_until("Rebuilding supervisor done", [&]() {
    // Wait until all shards rebuilding have been requested
    auto count = count_requested_rebuildings(cluster);
    EXPECT_TRUE(count <= shards.size());
    return count == shards.size();
  });

  // Read event log to check that rebuildings were requested no more than once.

  const logid_t event_log_id = configuration::InternalLogs::EVENT_LOG_DELTAS;

  lsn_t until_lsn = client->getTailLSNSync(event_log_id);
  ASSERT_NE(LSN_INVALID, until_lsn);

  auto reader = client->createReader(1);
  reader->startReading(event_log_id, LSN_OLDEST, until_lsn);
  std::set<ShardID> seen;
  while (reader->isReadingAny()) {
    std::vector<std::unique_ptr<DataRecord>> data;
    GapRecord gap;
    ssize_t nread = reader->read(1, &data, &gap);
    if (nread < 0) {
      EXPECT_EQ(-1, nread);
      EXPECT_EQ(E::GAP, err);
      EXPECT_TRUE(gap.type == GapType::BRIDGE || gap.type == GapType::HOLE ||
                  gap.type == GapType::TRIM);
      continue;
    }
    if (nread == 0) {
      continue;
    }
    ASSERT_EQ(1, nread);

    // TODO: improve this by providing proper api
    EventLogStateMachine::DeltaHeader header;
    std::unique_ptr<EventLogRecord> rec;
    int rv;
    if (EventLogStateMachine::deserializeDeltaHeader(
            data[0]->payload, header)) {
      const uint8_t* ptr =
          reinterpret_cast<const uint8_t*>(data[0]->payload.data());
      rv = EventLogRecord::fromPayload(
          Payload(ptr + header.header_sz,
                  data[0]->payload.size() - header.header_sz),
          rec);
    } else {
      rv = EventLogRecord::fromPayload(data[0]->payload, rec);
    }
    ASSERT_EQ(0, rv);
    EXPECT_NE(EventType::SHARD_ABORT_REBUILD, rec->getType());
    if (rec->getType() != EventType::SHARD_NEEDS_REBUILD) {
      continue;
    }
    ld_info("Got SHARD_NEEDS_REBUILD with lsn=%s timestamp=%s: %s",
            lsn_to_string(data[0]->attrs.lsn).c_str(),
            format_time(data[0]->attrs.timestamp).c_str(),
            rec->describe().c_str());
    auto ev = dynamic_cast<SHARD_NEEDS_REBUILD_Event*>(rec.get());
    auto s = ShardID(ev->header.nodeIdx, ev->header.shardIdx);
    EXPECT_TRUE(shards.count(s));
    EXPECT_FALSE(seen.count(s));
    seen.insert(s);
  }
  EXPECT_EQ(shards.size(), seen.size());
  // Check rebuilding supervisor stats, once more
  EXPECT_EQ(shards.size(), count_requested_rebuildings(cluster));
}

void waitForNodesToReadEventLog(IntegrationTestUtils::Cluster& cluster) {
  auto check_nodes = [=](IntegrationTestUtils::Node& node) {
    auto map = node.eventLogInfo();
    if (map.empty()) {
      return false;
    }
    ld_check(map.count("Delta read ptr"));
    ld_check(map.count("Delta replay tail"));
    return (!map["Delta replay tail"].empty() &&
            !map["Delta read ptr"].empty() &&
            (folly::to<uint64_t>(map["Delta replay tail"]) <=
             folly::to<uint64_t>(map["Delta read ptr"])));
  };

  cluster.waitUntilAll(
      "Nodes have read the event log up to the tail", check_nodes);
}

TEST_F(RebuildingSupervisorIntegrationTest, BasicFD) {
  // Replication factor is 2 by default.
  auto cluster = IntegrationTestUtils::ClusterFactory()
                     .enableSelfInitiatedRebuilding("1s")
                     .setParam("--event-log-grace-period", "1ms")
                     .setParam("--disable-event-log-trimming", "true")
                     .useHashBasedSequencerAssignment()
                     .setNumDBShards(2)
                     .deferStart()
                     .create(5);

  cluster->start({0, 1, 2, 3});

  ld_info("Waiting for rebuilding of N4 to be triggered");
  expect_rebuildings({{4, 0}, {4, 1}}, cluster.get());
}

// This test simulates the shutdown and removal of many nodes, then verifies
// that this doesn't casue the rebuilding trigger queue to fill up, preventing
// rebuildings to be triggered.
TEST_F(RebuildingSupervisorIntegrationTest, ShrinkAtBeginning) {
  int num_nodes = 5;
  shard_size_t num_shards = 2;

  // Replication factor is 2 by default.
  auto cluster = IntegrationTestUtils::ClusterFactory()
                     .enableSelfInitiatedRebuilding("1s")
                     // Start with self-initiated rebuilding disabled
                     .setParam("--enable-self-initiated-rebuilding", "false")
                     .setParam("--event-log-grace-period", "1ms")
                     .setParam("--disable-event-log-trimming", "true")
                     // Cap the rebuilding trigger queue to 1
                     .setParam("--max-rebuilding-trigger-queue-size", "1")
                     .useHashBasedSequencerAssignment()
                     .setNumDBShards(num_shards)
                     .create(num_nodes);

  waitForNodesToReadEventLog(*cluster);

  // Shutdown 0
  cluster->getNode(0).shutdown();

  // Now remove it from the config
  cluster->shrink(std::vector<node_index_t>({
      0,
  }));

  // And finally kill one node (eg: last).
  node_index_t dead_node_id = num_nodes - 1;
  cluster->getNode(dead_node_id).kill();
  // Restart N1 (rebuilding leader). it should trigger rebuilding for N<last>
  auto& leader = cluster->getNode(1);
  leader.shutdown();
  leader.start();
  leader.waitUntilStarted();
  leader.sendCommand("set enable-self-initiated-rebuilding true --ttl max");

  ld_info("Waiting for rebuilding of N%u to be triggered", dead_node_id);
  expect_rebuildings({{dead_node_id, 0}, {dead_node_id, 1}}, cluster.get());
}

// This test simulates the expansion of a cluster with dead nodes, then verifies
// that the nodes rebuilding is triggered
TEST_F(RebuildingSupervisorIntegrationTest, ExpandWithDeadNodes) {
  int num_nodes = 5;
  shard_size_t num_shards = 2;

  // Replication factor is 2 by default.
  auto cluster = IntegrationTestUtils::ClusterFactory()
                     .enableSelfInitiatedRebuilding("1s")
                     // Start with self-initiated rebuilding disabled
                     .setParam("--enable-self-initiated-rebuilding", "false")
                     .setParam("--event-log-grace-period", "1ms")
                     .setParam("--disable-event-log-trimming", "true")
                     // Cap the rebuilding trigger queue to 1
                     .setParam("--max-rebuilding-trigger-queue-size", "1")
                     .useHashBasedSequencerAssignment()
                     .setNumDBShards(num_shards)
                     .create(num_nodes);

  waitForNodesToReadEventLog(*cluster);

  // Enable self-initiated rebuilding
  cluster->applyToNodes([](auto& node) {
    node.sendCommand("set enable-self-initiated-rebuilding true --ttl max");
  });

  // Now expand cluster with one node, but do not start it.
  cluster->expand(1, false);
  // Dead node is the last one
  node_index_t dead_node_id = cluster->getNodes().size();
  ld_info("Waiting for rebuilding of N%u to be triggered", dead_node_id);
  expect_rebuildings({{dead_node_id, 0}, {dead_node_id, 1}}, cluster.get());
}

TEST_F(RebuildingSupervisorIntegrationTest, DontRebuildNonStorageNode) {
  int num_nodes = 4;
  shard_size_t num_shards = 2;
  node_index_t dead_node = 3;
  Configuration::Nodes nodes;
  // Make N3 a non-storage node, all the others storage nodes
  for (int i = 0; i < num_nodes; ++i) {
    Configuration::Node node;
    node.storage_state = (i == dead_node)
        ? configuration::StorageState::NONE
        : configuration::StorageState::READ_WRITE;
    node.generation = 1;
    node.sequencer_weight = 1;
    node.num_shards = num_shards;
    nodes[i] = node;
  }

  // Replication factor is 2 by default.
  auto cluster = IntegrationTestUtils::ClusterFactory()
                     .enableSelfInitiatedRebuilding("1s")
                     .setParam("--event-log-grace-period", "1ms")
                     .setParam("--disable-event-log-trimming", "true")
                     .useHashBasedSequencerAssignment()
                     .setNumDBShards(num_shards)
                     .setNodes(nodes)
                     .deferStart()
                     .create(num_nodes);

  // Start all nodes but N3
  cluster->start({0, 1, 2});

  // Expect no rebuildings
  wait_until("Rebuilding supervisor done", [&]() {
    // N3 is not started, skip it.
    for (int i = 0; i < num_nodes; ++i) {
      if (i == dead_node) {
        continue;
      }

      auto stats = cluster->getNode(i).stats();
      // Wait for this counter to be bumped
      if (stats["node_rebuilding_not_triggered_notstorage"] != 1) {
        return false;
      }
      // Make sure no rebuilding is triggered, or no rebuilding was
      // abandoned for a different reason.
      EXPECT_EQ(0, stats["shard_rebuilding_triggered"]);
      EXPECT_EQ(0, stats["shard_rebuilding_not_triggered_started"]);
      EXPECT_EQ(0, stats["node_rebuilding_not_triggered_notinconfig"]);
    }
    return true;
  });
}

TEST_F(RebuildingSupervisorIntegrationTest, IsolatedNode) {
  int num_nodes = 6;
  int num_shards = 2;

  auto cluster = IntegrationTestUtils::ClusterFactory()
                     .enableSelfInitiatedRebuilding("1s")
                     // Start with self-initiated rebuilding disabled
                     .setParam("--enable-self-initiated-rebuilding", "false")
                     .setParam("--event-log-grace-period", "1ms")
                     .setParam("--disable-event-log-trimming", "true")
                     .useHashBasedSequencerAssignment()
                     .setParam("--min-gossips-for-stable-state", "0")
                     .setNumDBShards(num_shards)
                     .oneConfigPerNode()
                     .create(num_nodes);

  waitForNodesToReadEventLog(*cluster);

  // Isolate N0 into its own partition
  std::set<int> partition1 = {0};
  std::set<int> partition2;
  for (int i = 1; i < num_nodes; ++i) {
    partition2.insert(i);
  }
  cluster->partition({partition1, partition2});

  // Enable self-initiated rebuilding
  cluster->applyToNodes([](auto& node) {
    node.sendCommand("set enable-self-initiated-rebuilding true --ttl max");
  });

  // Wait until N0 rebuilding is triggered by N1
  wait_until("N0 rebuilding triggered", [&]() {
    // check N1
    auto tmp_stats = cluster->getNode(1).stats();
    return (tmp_stats["shard_rebuilding_triggered"] == num_shards);
  });

  // No rebuilding should be triggered by N0
  auto stats = cluster->getNode(0).stats();
  ASSERT_EQ(0, stats["shard_rebuilding_triggered"]);

  // Now take N0 out of isalation
  partition1.erase(0);
  partition2.insert(0);
  cluster->partition({partition2});

  // Wait for N0 to cancel all its rebuilding triggers becasue nodes are alive
  wait_until("N0 cancels all rebuilding triggers", [&]() {
    auto tmp_stats = cluster->getNode(0).stats();
    return (tmp_stats["shard_rebuilding_not_triggered_nodealive"] ==
            stats["shard_rebuilding_not_triggered_nodealive"] +
                ((num_nodes - 1) * num_shards));
  });

  // Make sure N0 did not trigger any rebuilding
  stats = cluster->getNode(0).stats();
  ASSERT_EQ(0, stats["shard_rebuilding_triggered"]);
}

TEST_F(RebuildingSupervisorIntegrationTest, IsolatedRack) {
  int num_nodes = 6;
  int num_shards = 2;
  int num_racks = 3;

  Configuration::Log event_log;
  event_log.replicationFactor = 2;
  event_log.rangeName = "my-event-log";
  event_log.extraCopies = 0;
  event_log.syncedCopies = 0;
  event_log.singleWriter = false;
  event_log.syncReplicationScope = NodeLocationScope::RACK;

  auto cluster = IntegrationTestUtils::ClusterFactory()
                     .enableSelfInitiatedRebuilding("1s")
                     // Start with self-initiated rebuilding disabled
                     .setParam("--enable-self-initiated-rebuilding", "false")
                     .setParam("--event-log-grace-period", "1ms")
                     .setParam("--reader-stalled-grace-period", "1s")
                     .setParam("--disable-event-log-trimming", "true")
                     .useHashBasedSequencerAssignment()
                     .setNumDBShards(num_shards)
                     .setNumRacks(num_racks)
                     .setEventLogConfig(event_log)
                     .oneConfigPerNode()
                     .deferStart()
                     .create(num_nodes);

  cluster->start({});

  waitForNodesToReadEventLog(*cluster);

  // Isolate rack 0 into their own partition
  std::set<int> partition1;
  std::set<int> partition2;
  for (int i = 0; i < num_nodes; ++i) {
    if (i % num_racks == 0) {
      partition1.insert(i);
    } else {
      partition2.insert(i);
    }
  }
  cluster->partition({partition1, partition2});

  // Enable self-initiated rebuilding
  cluster->applyToNodes([](auto& node) {
    node.sendCommand("set enable-self-initiated-rebuilding true --ttl max");
  });

  // Wait until rebuilding of rack 0 (2 nodes) is triggered by N1
  wait_until("rack rebuilding triggered", [&]() {
    // check N1
    auto tmp_stats = cluster->getNode(1).stats();
    return (tmp_stats["shard_rebuilding_triggered"] ==
            (partition1.size() * num_shards));
  });

  // No rebuilding should be triggered by any isolated nodes
  for (int i : partition1) {
    auto stats = cluster->getNode(i).stats();
    ASSERT_EQ(0, stats["shard_rebuilding_triggered"]);
  }

  // Only N1 should have rebuilt the rack
  for (int i : partition2) {
    auto stats = cluster->getNode(i).stats();
    ASSERT_EQ((i == 1) ? (partition1.size() * num_shards) : 0,
              stats["shard_rebuilding_triggered"]);
  }

  // Now take the first node out of isalation
  int n = *partition1.begin();
  partition1.erase(n);
  partition2.insert(n);
  cluster->partition({partition1, partition2});

  // Wait for this node to try and trigger rebuilding of the rest of the rack
  // but cancel because rebuilding was already triggered earlier.
  wait_until("unisolated node tries to trigger rebuilding", [&]() {
    auto tmp_stats = cluster->getNode(n).stats();
    return (tmp_stats["shard_rebuilding_not_triggered_started"] ==
            partition1.size() * num_shards);
  });

  // Make sure this node did not trigger any rebuilding
  auto stats = cluster->getNode(n).stats();
  ASSERT_EQ(0, stats["shard_rebuilding_triggered"]);
}

TEST_F(RebuildingSupervisorIntegrationTest, s143309) {
  // Simulates the conditions that lead to SEV 143309.
  // This is basically the rack isolation test, with a
  // suspect duration period greater than the self initiated rebuilding
  // grace period to ensure that the rebuilding trigger fires while
  // nodes are in the SUSPECT state

  int num_nodes = 6;
  int num_shards = 2;
  int num_racks = 3;

  Configuration::Log event_log;
  event_log.replicationFactor = 2;
  event_log.rangeName = "my-event-log";
  event_log.extraCopies = 0;
  event_log.syncedCopies = 0;
  event_log.singleWriter = false;
  event_log.syncReplicationScope = NodeLocationScope::RACK;

  auto cluster = IntegrationTestUtils::ClusterFactory()
                     .setParam("--event-log-grace-period", "1ms")
                     .setParam("--reader-stalled-grace-period", "1s")
                     .setParam("--disable-event-log-trimming", "true")
                     .enableSelfInitiatedRebuilding("1s")
                     // Start with self-initiated rebuilding disabled
                     .setParam("--enable-self-initiated-rebuilding", "false")
                     .useHashBasedSequencerAssignment(100, "2s")
                     .setNumDBShards(num_shards)
                     .setNumRacks(num_racks)
                     .setEventLogConfig(event_log)
                     .oneConfigPerNode()
                     .create(num_nodes);

  waitForNodesToReadEventLog(*cluster);

  // Enable self-initiated rebuilding
  cluster->applyToNodes([](auto& node) {
    node.sendCommand("set enable-self-initiated-rebuilding true --ttl max");
  });

  // Isolate rack 0 into their own partition
  std::set<int> partition1;
  std::set<int> partition2;
  for (int i = 0; i < num_nodes; ++i) {
    if (i % num_racks == 0) {
      partition1.insert(i);
    } else {
      partition2.insert(i);
    }
  }
  cluster->partition({partition1, partition2});

  // Wait until rebuilding of rack 0 (2 nodes) is triggered by N1
  wait_until("rack rebuilding triggered", [&]() {
    // check N1
    auto tmp_stats = cluster->getNode(1).stats();
    return (tmp_stats["shard_rebuilding_triggered"] ==
            (partition1.size() * num_shards));
  });

  // No rebuilding should be triggered by any isolated nodes
  for (int i : partition1) {
    auto stats = cluster->getNode(i).stats();
    ASSERT_EQ(0, stats["shard_rebuilding_triggered"]);
  }

  // Only N1 should have rebuilt the rack
  for (int i : partition2) {
    auto stats = cluster->getNode(i).stats();
    ASSERT_EQ((i == 1) ? (partition1.size() * num_shards) : 0,
              stats["shard_rebuilding_triggered"]);
  }

  // Now take the first node out of isalation
  int n = *partition1.begin();
  partition1.erase(n);
  partition2.insert(n);
  cluster->partition({partition1, partition2});

  // Wait for this node to try and trigger rebuilding of the rest of the rack
  // but cancel because rebuilding was already triggered earlier.
  wait_until("unisolated node tries to trigger rebuilding", [&]() {
    auto tmp_stats = cluster->getNode(n).stats();
    return (tmp_stats["shard_rebuilding_not_triggered_started"] ==
            partition1.size() * num_shards);
  });

  // Make sure this node did not trigger any rebuilding
  auto stats = cluster->getNode(n).stats();
  ASSERT_EQ(0, stats["shard_rebuilding_triggered"]);
}

TEST_F(RebuildingSupervisorIntegrationTest, BasicShard) {
  Configuration::Nodes nodes_config;
  for (int i = 0; i < 5; ++i) {
    Configuration::Node node;
    node.generation = i != 2 ? 1 : 2;
    node.sequencer_weight = (i == 0);
    node.num_shards = 3;
    nodes_config[i] = node;
  }

  auto cluster = IntegrationTestUtils::ClusterFactory()
                     .enableSelfInitiatedRebuilding()
                     .setParam("--event-log-grace-period", "1ms")
                     .setParam("--disable-event-log-trimming", "true")
                     .setNodes(nodes_config)
                     .setNumDBShards(3)
                     .deferStart()
                     .create(5);

  // Node 2: generation = 2, shard 1 has no RebuildingCompleteMetadata.
  // Expect rebuilding of N2:S1.
  {
    // Write RebuildingCompleteMetadata to all shards except 1.
    auto& node = cluster->getNode(2);
    auto sharded_store = node.createLocalLogStore();
    for (int i = 0; i < sharded_store->numShards(); ++i) {
      if (i == 1) {
        continue;
      }
      auto store = sharded_store->getByIndex(i);
      ld_check(store != nullptr);
      RebuildingCompleteMetadata meta;
      EXPECT_EQ(0, store->writeStoreMetadata(meta));
    }
  }

  // Node 1: shard 2 is corrupted. Expect rebuilding of N1:S2.
  cluster->getNode(1).corruptShards({2});

  // Not starting all nodes, since otherwise rebuilding completing and ACKing
  // might race with the check below.
  cluster->start({0, 1, 2, 3});

  ld_info("Waiting for rebuilding of N2:S1 and N1:S2 to be triggered");
  expect_rebuildings({{2, 1}, {1, 2}}, cluster.get());
}

// Makes sure that the threshold for number of currently running rebuildings is
// applied.
// This test simulates the failure of two nodes, while the threshold is 1. The
// second rebuilding should not trigger.
TEST_F(RebuildingSupervisorIntegrationTest, NodeRebuildingThreshold) {
  int num_nodes = 6;

  Configuration::Nodes nodes_config;
  for (int i = 0; i < num_nodes; ++i) {
    Configuration::Node node;
    node.generation = 1;
    node.num_shards = 1;
    node.sequencer_weight = (i == 0);
    nodes_config[i] = node;
  }

  Configuration::Log event_log;
  event_log.replicationFactor = 3;
  event_log.rangeName = "my-event-log";
  event_log.extraCopies = 0;
  event_log.syncedCopies = 0;
  event_log.singleWriter = false;
  event_log.syncReplicationScope = NodeLocationScope::NODE;

  auto cluster =
      IntegrationTestUtils::ClusterFactory()
          // disable rebuilding to make sure that nodes won't complete
          // or abort rebuildings, which would interfere with this test.
          .setParam("--enable-self-initiated-rebuilding", "false")
          .setParam("--disable-rebuilding", "true")
          .setParam("--event-log-grace-period", "1ms")
          .setParam("--reader-stalled-grace-period", "1s")
          .setParam("--disable-event-log-trimming", "true")
          .useHashBasedSequencerAssignment()
          .setNodes(nodes_config)
          .setEventLogConfig(event_log)
          .deferStart()
          .create(num_nodes);

  cluster->getNode(0)
      .setParam("--disable-rebuilding", "false")
      .setParam("--enable-self-initiated-rebuilding", "false")
      .setParam("--self-initiated-rebuilding-grace-period", "3s")
      // Don't limit the trigger queue size, to make sure that the only
      // threshold we hit is the number of currently running rebuildings
      .setParam("--max-rebuilding-trigger-queue-size", "10")
      // Set the threshold to allow only one node rebuilding at a time
      .setParam("--max-node-rebuilding-percentage",
                folly::format("{}", (100 / num_nodes - 1)).str());

  cluster->start({});

  auto client = cluster->createClient();

  // Wait until all nodes are seen as alive
  for (const auto& n : cluster->getNodes()) {
    int rv = wait_until([&]() {
      for (const auto& it : n.second->gossipCount()) {
        if (it.second.first != "ALIVE" || it.second.second > 1000000) {
          return false;
        }
      }
      return true;
    });
  }

  auto stats = cluster->getNode(0).stats();
  auto prev_rebuilding_scheduled = stats["shard_rebuilding_scheduled"];

  // Manually trigger rebuilding of N1
  // (the reason to doing this manually is to mimic as close as possible what
  // the other test does)
  IntegrationTestUtils::requestShardRebuilding(*client, 1, 0);
  // Kill N3
  cluster->getNode(3).kill();
  // Enable self-initiated rebuilding on N0
  cluster->getNode(0).sendCommand(
      "set enable-self-initiated-rebuilding true --ttl max");

  // Rebuilding supervisor should hit the threshold of currently running
  // rebuildings and not trigger rebuilding for N2
  wait_until("rebuilding scheduled", [&]() {
    // Check N0
    auto tmp_stats = cluster->getNode(0).stats();
    return tmp_stats["shard_rebuilding_scheduled"] >=
        prev_rebuilding_scheduled + 1;
  });

  // Now wait a few more grace period, to make sure it does not trigger
  // rebuildings
  wait_until("rebuilding throttled",
             [&]() {
               // Check N0
               auto tmp_stats = cluster->getNode(0).stats();
               return tmp_stats["shard_rebuilding_triggered"] > 0;
             },
             std::chrono::steady_clock::now() + std::chrono::seconds(6));

  stats = cluster->getNode(0).stats();
  ASSERT_EQ(0, stats["shard_rebuilding_triggered"]);
}

// Makes sure that mini rebuildings are not counted towards the threshold of
// currently running rebuildings.
// This test simulates a mini-rebuilding and then the failure of one node, with
// a threshold of 1. The node rebuilding should trigger, since the threshold
// computation ignores mini rebuildings.
//
// Because nodes may rebuild faster than the test executes, rebuilding is
// disabled on all the nodes but the rebuilding supervisor leader (N0). That
// way, nodes won't abort the mini rebuilding or complete it (making the shard
// fully authoritative) before the rebuilding supervisor evaluates the
// threshold.
TEST_F(RebuildingSupervisorIntegrationTest,
       NodeRebuildingThresholdIgnoredForMiniRebuilding) {
  int num_nodes = 6;

  Configuration::Nodes nodes_config;
  for (int i = 0; i < num_nodes; ++i) {
    Configuration::Node node;
    node.generation = 1;
    node.num_shards = 1;
    node.sequencer_weight = (i == 0);
    nodes_config[i] = node;
  }

  Configuration::Log event_log;
  event_log.replicationFactor = 3;
  event_log.rangeName = "my-event-log";
  event_log.extraCopies = 0;
  event_log.syncedCopies = 0;
  event_log.singleWriter = false;
  event_log.syncReplicationScope = NodeLocationScope::NODE;

  auto cluster =
      IntegrationTestUtils::ClusterFactory()
          // disable rebuilding to make sure that nodes won't complete
          // or abort rebuildings, which would interfere with this test.
          .setParam("--enable-self-initiated-rebuilding", "false")
          .setParam("--disable-rebuilding", "true")
          .setParam("--event-log-grace-period", "1ms")
          .setParam("--reader-stalled-grace-period", "1s")
          .setParam("--disable-event-log-trimming", "true")
          .useHashBasedSequencerAssignment()
          .setNodes(nodes_config)
          .setEventLogConfig(event_log)
          .deferStart()
          .create(num_nodes);

  cluster->getNode(0)
      .setParam("--disable-rebuilding", "false")
      .setParam("--enable-self-initiated-rebuilding", "false")
      .setParam("--self-initiated-rebuilding-grace-period", "1s")
      // Don't limit the trigger queue size, to make sure that the only
      // threshold we hit is the number of currently running rebuildings
      .setParam("--max-rebuilding-trigger-queue-size", "10")
      // Set the threshold to allow only one node rebuilding at a time
      .setParam("--max-node-rebuilding-percentage",
                folly::format("{}", (100 / num_nodes - 1)).str());

  cluster->start({});

  auto client = cluster->createClient();

  // Manually trigger mini rebuilding for N1
  auto now = RecordTimestamp::now();
  auto dirtyStart = RecordTimestamp(now - std::chrono::minutes(10));
  auto dirtyEnd = RecordTimestamp(now - std::chrono::minutes(5));
  RebuildingRangesMetadata rrm;
  rrm.addTimeInterval(
      DataClass::APPEND, RecordTimeInterval(dirtyStart, dirtyEnd));
  IntegrationTestUtils::requestShardRebuilding(*client, 1, 0, 0, &rrm);
  // Now kill N3
  cluster->getNode(3).kill();
  // Enable self-initiated rebuilding on N0
  cluster->getNode(0).sendCommand(
      "set enable-self-initiated-rebuilding true --ttl max");

  // Rebuilding supervisor should ignore the time-ranged rebuilding to compute
  // threshold and trigger rebuilding for N2
  wait_until("rebuilding triggered", [&]() {
    // Check N0
    auto tmp_stats = cluster->getNode(0).stats();
    return tmp_stats["shard_rebuilding_triggered"] == 1;
  });
}

// Makes sure that the threshold for number of currently running rebuildings
// ignores nodes that are not in the config
TEST_F(RebuildingSupervisorIntegrationTest,
       NodeRebuildingThresholdIgnoresNotInConfig) {
  int num_nodes = 6;

  Configuration::Nodes nodes_config;
  for (int i = 0; i < num_nodes; ++i) {
    Configuration::Node node;
    node.generation = 1;
    node.num_shards = 1;
    node.sequencer_weight = (i == 0);
    nodes_config[i] = node;
  }

  Configuration::Log event_log;
  event_log.replicationFactor = 3;
  event_log.rangeName = "my-event-log";
  event_log.extraCopies = 0;
  event_log.syncedCopies = 0;
  event_log.singleWriter = false;
  event_log.syncReplicationScope = NodeLocationScope::NODE;

  auto cluster =
      IntegrationTestUtils::ClusterFactory()
          // disable rebuilding to make sure that nodes won't complete
          // or abort rebuildings, which would interfere with this test.
          .setParam("--enable-self-initiated-rebuilding", "false")
          .setParam("--disable-rebuilding", "true")
          .setParam("--event-log-grace-period", "1ms")
          .setParam("--reader-stalled-grace-period", "1s")
          .setParam("--disable-event-log-trimming", "true")
          .useHashBasedSequencerAssignment()
          .setNodes(nodes_config)
          .setEventLogConfig(event_log)
          .deferStart()
          .create(num_nodes);

  cluster->getNode(0)
      .setParam("--disable-rebuilding", "false")
      .setParam("--enable-self-initiated-rebuilding", "false")
      .setParam("--self-initiated-rebuilding-grace-period", "3s")
      // Don't limit the trigger queue size, to make sure that the only
      // threshold we hit is the number of currently running rebuildings
      .setParam("--max-rebuilding-trigger-queue-size", "10")
      // Set the threshold to allow only one node rebuilding at a time
      .setParam("--max-node-rebuilding-percentage",
                folly::format("{}", (100 / num_nodes - 1)).str());

  cluster->start({});

  auto client = cluster->createClient();

  // Wait until all nodes are seen as alive
  for (const auto& n : cluster->getNodes()) {
    int rv = wait_until([&]() {
      for (const auto& it : n.second->gossipCount()) {
        if (it.second.first != "ALIVE" || it.second.second > 1000000) {
          return false;
        }
      }
      return true;
    });
  }

  auto stats = cluster->getNode(0).stats();
  auto prev_rebuilding_scheduled = stats["shard_rebuilding_scheduled"];

  // Manually trigger rebuilding of N39 (is not in the config)
  IntegrationTestUtils::requestShardRebuilding(*client, 39, 0);
  // Now kill N3
  cluster->getNode(3).kill();
  // Enable self-initiated rebuilding on N0
  cluster->getNode(0).sendCommand(
      "set enable-self-initiated-rebuilding true --ttl max");

  // Rebuilding supervisor should ignore the rebuilding of non-existent node to
  // compute threshold and trigger rebuilding for N3
  wait_until("rebuilding triggered", [&]() {
    // check N0
    auto tmp_stats = cluster->getNode(0).stats();
    return tmp_stats["shard_rebuilding_triggered"] == 1;
  });
}

// Simulates an I/O error on the read path and verifies that the node initiates
// rebuilding for its broken shard.
TEST_F(RebuildingSupervisorIntegrationTest, ReadIOError) {
  Configuration::Nodes nodes_config;
  for (int i = 0; i < 5; ++i) {
    Configuration::Node node;
    node.generation = 1;
    node.sequencer_weight = (i == 0);
    node.num_shards = 1;
    nodes_config[i] = node;
  }

  auto cluster = IntegrationTestUtils::ClusterFactory()
                     .enableSelfInitiatedRebuilding()
                     .setParam("--event-log-grace-period", "1ms")
                     .setParam("--disable-event-log-trimming", "true")
                     .setParam("--sticky-copysets-block-size", "1")
                     .useHashBasedSequencerAssignment()
                     .setNodes(nodes_config)
                     .deferStart()
                     .create(5);

  cluster->start({0, 1, 2, 3, 4});
  cluster->waitForRecovery();

  auto client = cluster->createClient();

  // Append some records
  int num_records = 10;
  for (int i = 0; i < num_records; i++) {
    lsn_t lsn = client->appendSync(logid_t(1), Payload("hello", 5));
    EXPECT_NE(LSN_INVALID, lsn);
  }

  // Read the records
  auto reader = client->createReader(1);
  std::vector<std::unique_ptr<DataRecord>> records;
  GapRecord gap;
  ssize_t nread;
  size_t count = 0;
  reader->startReading(logid_t(1), LSN_OLDEST);
  do {
    nread = reader->read(num_records, &records, &gap);
    ASSERT_TRUE(nread > 0 || err == E::GAP);
    count += nread;
  } while (count < num_records);

  // Create a second reader
  auto reader2 = client->createReader(1);
  reader2->startReading(logid_t(1), LSN_OLDEST);

  // Now inject read errors into N1:S0
  if (cluster->getNode(1).injectShardFault("0", "data", "read", "io_error")) {
    // Then read the records again. N1 should hit a read/iterator error and
    // enter fail safe mode then trigger rebuilding for its broken shard.
    // Note that the read should still succeed, since recaords can be read from
    // other nodes.
    count = 0;
    do {
      nread = reader2->read(num_records, &records, &gap);
      ASSERT_TRUE(nread > 0 || err == E::GAP);
      count += nread;
    } while (count < num_records);

    ld_info("Waiting for rebuilding of N1:S0 to be triggered");
    expect_rebuildings({{1, 0}}, cluster.get());

    auto stats = cluster->getNode(1).stats();
    ASSERT_EQ(stats["failed_safe_log_stores"], 1);
  }
}

// Makes sure that the threshold for number of rebuilding triggers is
// applied.
// This test simulates the failure of two nodes, while the threshold is 1.
// No rebuilding should be started.
TEST_F(RebuildingSupervisorIntegrationTest, RebuildingTriggerQueueThreshold) {
  int num_nodes = 6;

  Configuration::Nodes nodes_config;
  for (int i = 0; i < num_nodes; ++i) {
    Configuration::Node node;
    node.generation = 1;
    node.num_shards = 1;
    node.sequencer_weight = (i == 0);
    nodes_config[i] = node;
  }

  Configuration::Log event_log;
  event_log.replicationFactor = 3;
  event_log.rangeName = "my-event-log";
  event_log.extraCopies = 0;
  event_log.syncedCopies = 0;
  event_log.singleWriter = false;
  event_log.syncReplicationScope = NodeLocationScope::NODE;

  auto cluster =
      IntegrationTestUtils::ClusterFactory()
          // Disable M2M rebuilding to make sure that nodes won't complete
          // or abort rebuildings, which would interfere with this test.
          .setParam("--enable-self-initiated-rebuilding", "false")
          .setParam("--disable-rebuilding", "true")
          .setParam("--event-log-grace-period", "1ms")
          .setParam("--reader-stalled-grace-period", "1s")
          .setParam("--disable-event-log-trimming", "true")
          .useHashBasedSequencerAssignment()
          .setNodes(nodes_config)
          .setEventLogConfig(event_log)
          .deferStart()
          .create(num_nodes);

  cluster->getNode(0)
      .setParam("--disable-rebuilding", "false")
      .setParam("--enable-self-initiated-rebuilding", "false")
      .setParam("--self-initiated-rebuilding-grace-period", "3s")
      // Set the threshold to 1 to limit the number of triggers
      .setParam("--max-rebuilding-trigger-queue-size", "1");

  cluster->start({});

  auto client = cluster->createClient();

  // Wait until all nodes are seen as alive
  for (const auto& n : cluster->getNodes()) {
    int rv = wait_until([&]() {
      for (const auto& it : n.second->gossipCount()) {
        if (it.second.first != "ALIVE" || it.second.second > 1000000) {
          return false;
        }
      }
      return true;
    });
  }

  auto stats = cluster->getNode(0).stats();
  // Check that the rebuilding supervisor is not throttled.
  stats = cluster->getNode(0).stats();
  ASSERT_EQ(0, stats["rebuilding_supervisor_throttled"]);
  auto prev_rebuilding_scheduled = stats["shard_rebuilding_scheduled"];

  // Kill N1 and N3
  cluster->getNode(1).kill();
  cluster->getNode(3).kill();
  // Enable self-initiated rebuilding on N0
  cluster->getNode(0).sendCommand(
      "set enable-self-initiated-rebuilding true --ttl max");

  // Rebuilding supervisor should hit the threshold of current number of
  // triggers and not trigger any rebuilding
  wait_until("rebuilding scheduled", [&]() {
    // Check N0
    auto tmp_stats = cluster->getNode(0).stats();
    return tmp_stats["shard_rebuilding_scheduled"] >=
        prev_rebuilding_scheduled + 1;
  });

  // Now wait a few more grace period, to make sure it does not trigger
  // rebuildings
  wait_until("rebuilding throttled",
             [&]() {
               // Check N0
               auto tmp_stats = cluster->getNode(0).stats();
               return tmp_stats["shard_rebuilding_triggered"] > 0;
             },
             std::chrono::steady_clock::now() + std::chrono::seconds(6));

  stats = cluster->getNode(0).stats();
  ASSERT_EQ(0, stats["shard_rebuilding_triggered"]);
  // Check that the rebuilding supervisor entered throttling mode.
  ASSERT_EQ(1, stats["rebuilding_supervisor_throttled"]);

  // Now start N3. This should cancel the rebuilding trigger, and casue the
  // rebuilding supervisor to exit throttling mode.
  cluster->getNode(3).start();

  // Rebuilding supervisor should trigger rebuilding for N2
  wait_until("rebuilding triggered", [&]() {
    // Check N0
    auto tmp_stats = cluster->getNode(0).stats();
    return tmp_stats["shard_rebuilding_triggered"] == 1;
  });

  stats = cluster->getNode(0).stats();
  // Check that the rebuilding supervisor exited throttling mode.
  ASSERT_EQ(0, stats["rebuilding_supervisor_throttled"]);
}

// Makes sure that rebuilding_supervisor_throttled stats resets even if the
// leader changed.
TEST_F(RebuildingSupervisorIntegrationTest,
       RebuildingTriggerQueueThresholdResetOnNonLeader) {
  int num_nodes = 6;

  Configuration::Nodes nodes_config;
  for (int i = 0; i < num_nodes; ++i) {
    Configuration::Node node;
    node.generation = 1;
    node.num_shards = 1;
    node.sequencer_weight = (i == 0);
    nodes_config[i] = node;
  }

  Configuration::Log event_log;
  event_log.replicationFactor = 3;
  event_log.rangeName = "my-event-log";
  event_log.extraCopies = 0;
  event_log.syncedCopies = 0;
  event_log.singleWriter = false;
  event_log.syncReplicationScope = NodeLocationScope::NODE;

  auto cluster =
      IntegrationTestUtils::ClusterFactory()
          // Disable M2M rebuilding to make sure that nodes won't complete
          // or abort rebuildings, which would interfere with this test.
          .setParam("--enable-self-initiated-rebuilding", "false")
          .setParam("--disable-rebuilding", "true")
          .setParam("--event-log-grace-period", "1ms")
          .setParam("--reader-stalled-grace-period", "1s")
          .setParam("--disable-event-log-trimming", "true")
          .useHashBasedSequencerAssignment()
          .setNodes(nodes_config)
          .setEventLogConfig(event_log)
          .deferStart()
          .create(num_nodes);

  cluster->getNode(1)
      .setParam("--disable-rebuilding", "false")
      .setParam("--enable-self-initiated-rebuilding", "false")
      .setParam("--self-initiated-rebuilding-grace-period", "3s")
      // Set the threshold to 1 to limit the number of triggers
      .setParam("--max-rebuilding-trigger-queue-size", "1");

  cluster->start({});

  auto client = cluster->createClient();

  // Wait until all nodes are seen as alive
  for (const auto& n : cluster->getNodes()) {
    int rv = wait_until([&]() {
      for (const auto& it : n.second->gossipCount()) {
        if (it.second.first != "ALIVE" || it.second.second > 1000000) {
          return false;
        }
      }
      return true;
    });
  }

  auto stats = cluster->getNode(1).stats();
  // Check that the rebuilding supervisor is not throttled.
  stats = cluster->getNode(1).stats();
  ASSERT_EQ(0, stats["rebuilding_supervisor_throttled"]);
  auto prev_rebuilding_scheduled = stats["shard_rebuilding_scheduled"];

  // Kill N0 and N3
  cluster->getNode(0).kill();
  cluster->getNode(3).kill();
  // Enable self-initiated rebuilding on N1
  cluster->getNode(1).sendCommand(
      "set enable-self-initiated-rebuilding true --ttl max");

  // Rebuilding supervisor should hit the threshold of current number of
  // triggers and not trigger any rebuilding
  wait_until("rebuilding scheduled", [&]() {
    // Check N1
    auto tmp_stats = cluster->getNode(1).stats();
    return tmp_stats["shard_rebuilding_scheduled"] >=
        prev_rebuilding_scheduled + 1;
  });

  // Now wait a few more grace period, to make sure it does not trigger
  // rebuildings
  wait_until("rebuilding throttled",
             [&]() {
               // Check N1
               auto tmp_stats = cluster->getNode(1).stats();
               return tmp_stats["shard_rebuilding_triggered"] > 0;
             },
             std::chrono::steady_clock::now() + std::chrono::seconds(6));

  stats = cluster->getNode(1).stats();
  ASSERT_EQ(0, stats["shard_rebuilding_triggered"]);
  // Check that the rebuilding supervisor entered throttling mode.
  ASSERT_EQ(1, stats["rebuilding_supervisor_throttled"]);

  // Now start N0. This should cancel the rebuilding trigger, and casue the
  // rebuilding supervisor to exit throttling mode.
  cluster->getNode(0).start();

  // Rebuilding supervisor should not trigger rebuilding for N3 becasue
  // the leader is now N0.
  wait_until("rebuilding blocked",
             [&]() {
               // Check N1
               auto tmp_stats = cluster->getNode(1).stats();
               return tmp_stats["shard_rebuilding_triggered"] > 0;
             },
             std::chrono::steady_clock::now() + std::chrono::seconds(6));

  stats = cluster->getNode(1).stats();
  // Check that the rebuilding supervisor exited throttling mode.
  ASSERT_EQ(0, stats["rebuilding_supervisor_throttled"]);
  // Check that it did not trigger any rebuilding
  ASSERT_EQ(0, stats["shard_rebuilding_triggered"]);
}

}} // namespace facebook::logdevice
