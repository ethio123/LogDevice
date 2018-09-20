/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#include <gtest/gtest.h>

#include <utility>
#include <numeric>
#include <functional>
#include <folly/Memory.h>
#include <folly/String.h>
#include "logdevice/common/configuration/Configuration.h"
#include "logdevice/common/NodeSetSelectorFactory.h"
#include "logdevice/common/configuration/LocalLogsConfig.h"
#include "logdevice/common/test/NodeSetTestUtil.h"
#include "logdevice/common/util.h"
#include <logdevice/common/toString.h>

using namespace facebook::logdevice;
using namespace facebook::logdevice::configuration;
using namespace facebook::logdevice::NodeSetTestUtil;

using Decision = NodeSetSelector::Decision;

using verify_func_t = std::function<void(StorageSet*)>;

static void verify_result(NodeSetSelector* selector,
                          std::shared_ptr<Configuration>& config,
                          logid_t logid,
                          Decision expected_decision,
                          verify_func_t verify,
                          const NodeSetSelector::Options* options = nullptr,
                          size_t iteration = 10) {
  SCOPED_TRACE("log " + toString(logid.val_));

  ld_check(iteration > 0);
  for (size_t i = 0; i < iteration; ++i) {
    std::unique_ptr<StorageSet> new_storage_set;
    NodeSetSelector::Decision decision;
    std::tie(decision, new_storage_set) =
        selector->getStorageSet(logid, config, nullptr, options);

    ASSERT_EQ(expected_decision, decision);
    if (decision != Decision::NEEDS_CHANGE) {
      ASSERT_EQ(nullptr, new_storage_set);
      continue;
    }

    ASSERT_NE(nullptr, new_storage_set);

    // perform basic checks
    // nodes in nodeset must be unique and in increasing order
    ASSERT_TRUE(std::is_sorted(new_storage_set->begin(),
                               new_storage_set->end(),
                               std::less_equal<ShardID>()));

    // must comply with the config
    const LogsConfig::LogGroupNode* logcfg = config->getLogGroupByIDRaw(logid);
    ASSERT_NE(nullptr, logcfg);
    const auto& attrs = logcfg->attrs();
    const auto& all_nodes = config->serverConfig()->getNodes();
    ASSERT_TRUE(ServerConfig::validStorageSet(
        all_nodes,
        *new_storage_set,
        ReplicationProperty::fromLogAttributes(attrs)));

    int nodeset_size = attrs.nodeSetSize().value().value_or(all_nodes.size());
    ReplicationProperty replication =
        ReplicationProperty::fromLogAttributes(attrs);
    auto predicted_size = selector->getStorageSetSize(
        logid, config, nodeset_size, replication, options);
    EXPECT_EQ(new_storage_set->size(), predicted_size);

    if (!options || options->exclude_nodes.size() == 0) {
      // Verifying that it passes checks in EpochMetaData as well
      EpochMetaData epoch_metadata(
          *new_storage_set,
          ReplicationProperty::fromLogAttributes(attrs),
          epoch_t(1),
          epoch_t(1));
      epoch_metadata.nodesconfig_hash.assign(
          config->serverConfig()->getStorageNodesConfigHash());
      epoch_metadata.h.flags |= MetaDataLogRecordHeader::HAS_NODESCONFIG_HASH;
      EXPECT_EQ(true, epoch_metadata.matchesConfig(logid, config));
    }

    // perform the user provided check
    verify(new_storage_set.get());
  }
}

// return true if nodesets for a given log based on 2 node configs are the same;
// false otherwise
static std::pair<size_t, size_t>
compare_nodesets(NodeSetSelector* selector,
                 std::shared_ptr<Configuration>& config1,
                 std::shared_ptr<Configuration>& config2,
                 logid_t logid,
                 std::map<ShardID, size_t>& old_distribution,
                 std::map<ShardID, size_t>& new_distribution,
                 const NodeSetSelector::Options* options = nullptr) {
  std::unique_ptr<StorageSet> old_storage_set;
  std::unique_ptr<StorageSet> new_storage_set;
  NodeSetSelector::Decision old_decision;
  NodeSetSelector::Decision new_decision;
  std::tie(old_decision, old_storage_set) =
      selector->getStorageSet(logid, config1, nullptr, options);

  std::tie(new_decision, new_storage_set) =
      selector->getStorageSet(logid, config2, nullptr, options);

  assert(std::is_sorted(old_storage_set->begin(), old_storage_set->end()));
  assert(std::is_sorted(new_storage_set->begin(), new_storage_set->end()));

  std::vector<ShardID> common_nodes(
      std::min(old_storage_set->size(), new_storage_set->size()));
  std::vector<ShardID>::iterator node_it =
      std::set_intersection(old_storage_set->begin(),
                            old_storage_set->end(),
                            new_storage_set->begin(),
                            new_storage_set->end(),
                            common_nodes.begin());
  common_nodes.resize(node_it - common_nodes.begin());

  for (ShardID current_shard_id : *old_storage_set) {
    old_distribution[current_shard_id]++;
  }

  for (ShardID current_shard_id : *new_storage_set) {
    new_distribution[current_shard_id]++;
  }

  return std::make_pair(old_storage_set->size() - common_nodes.size(),
                        new_storage_set->size() - common_nodes.size());
}

TEST(RandomCrossDomainNodeSetSelectorTest, RackAssignment) {
  // 100-node cluster with nodes from 5 different racks
  Nodes nodes;
  addNodes(&nodes, 10, 5, {}, "region0.datacenter1.01.a.a", 10);
  addNodes(&nodes, 35, 5, {}, "region0.datacenter2.01.a.a", 35);
  addNodes(&nodes, 20, 5, {}, "region0.datacenter1.01.a.b", 10);
  addNodes(&nodes, 20, 5, {}, "region1.datacenter1.02.a.a", 20);
  addNodes(&nodes, 15, 5, {}, "region1.datacenter1.02.a.b", 15);

  ld_check(nodes.size() == 100);

  Configuration::NodesConfig nodes_config(std::move(nodes));

  auto logs_config = std::make_shared<LocalLogsConfig>();
  addLog(logs_config.get(), logid_t{1}, 3, 0, 10, {}, NodeLocationScope::RACK);
  addLog(logs_config.get(), logid_t{2}, 3, 0, 20, {}, NodeLocationScope::RACK);
  addLog(logs_config.get(), logid_t{3}, 5, 0, 18, {}, NodeLocationScope::RACK);

  auto config = std::make_shared<Configuration>(
      ServerConfig::fromData("nodeset_selector_test", std::move(nodes_config)),
      std::move(logs_config));

  auto selector =
      NodeSetSelectorFactory::create(NodeSetSelectorType::RANDOM_CROSSDOMAIN);

  const Configuration& cfg = *config;
  // generate a verify_func_t function for checking nodeset with racks
  auto gen = [&cfg](size_t racks, size_t nodes_per_rack) {
    return [racks, nodes_per_rack, &cfg](StorageSet* storage_set) {
      ld_check(storage_set != nullptr);
      std::map<std::string, StorageSet> node_map;
      for (const ShardID i : *storage_set) {
        const Configuration::Node* node = cfg.serverConfig()->getNode(i.node());
        ASSERT_NE(nullptr, node);
        ASSERT_TRUE(node->location.hasValue());
        node_map[node->locationStr()].push_back(i);
      }

      ASSERT_EQ(racks, node_map.size());
      for (const auto& kv : node_map) {
        ASSERT_EQ(nodes_per_rack, kv.second.size());
      }
    };
  };

  verify_result(
      selector.get(), config, logid_t{1}, Decision::NEEDS_CHANGE, gen(5, 2));
  verify_result(
      selector.get(), config, logid_t{2}, Decision::NEEDS_CHANGE, gen(5, 4));
  verify_result(
      selector.get(), config, logid_t{3}, Decision::NEEDS_CHANGE, gen(5, 4));
}

TEST(RandomNodeSetSelectorTest, NodeExclusion) {
  // 10 node cluster
  configuration::Nodes nodes;
  const int SHARDS_PER_NODE = 5;
  addNodes(&nodes, 10, SHARDS_PER_NODE, {}, "", 10);
  ld_check(nodes.size() == 10);

  Configuration::NodesConfig nodes_config(std::move(nodes));

  auto logs_config = std::make_shared<LocalLogsConfig>();
  addLog(logs_config.get(), logid_t{1}, 3, 0, 5, {}, NodeLocationScope::NODE);
  addLog(logs_config.get(), logid_t{5}, 3, 0, 8, {}, NodeLocationScope::NODE);
  addLog(logs_config.get(), logid_t{6}, 3, 0, 8, {}, NodeLocationScope::NODE);

  auto config = std::make_shared<Configuration>(
      ServerConfig::fromData("nodeset_selector_test", std::move(nodes_config)),
      std::move(logs_config));

  auto selector =
      NodeSetSelectorFactory::create(NodeSetSelectorType::RANDOM_CROSSDOMAIN);

  NodeSetSelector::Options options;

  // generate a verify_func_t function
  auto gen = [](std::vector<node_index_t> exclude) {
    return [exclude](StorageSet* storage_set) {
      ld_check(storage_set);
      for (ShardID n : *storage_set) {
        for (node_index_t e : exclude) {
          ASSERT_NE(e, n.node());
        }
      }
    };
  };

  options.exclude_nodes = {1, 2, 3};
  verify_result(selector.get(),
                config,
                logid_t{1},
                Decision::NEEDS_CHANGE,
                gen({1, 2, 3}),
                &options);

  options.exclude_nodes = {1, 3};
  verify_result(selector.get(),
                config,
                logid_t{5},
                Decision::NEEDS_CHANGE,
                gen({1, 3}),
                &options);

  options.exclude_nodes = {1, 2, 3};
  // there are not enough nodes for log 6
  verify_result(selector.get(),
                config,
                logid_t{6},
                Decision::FAILED,
                gen({1, 2, 3}),
                &options);
}

TEST(RandomNodeSetSelector, ImpreciseNodeSetSize) {
  // 26-node cluster with nodes from 5 different racks
  dbg::currentLevel = dbg::Level::SPEW;
  Nodes nodes;
  addNodes(&nodes, 5, 1, {}, "region0.datacenter1.01.a.a", 5);
  addNodes(&nodes, 5, 1, {}, "region0.datacenter2.01.a.a", 5);
  addNodes(&nodes, 5, 1, {}, "region0.datacenter1.01.a.b", 5);
  addNodes(&nodes, 5, 1, {}, "region1.datacenter1.02.a.a", 5);
  addNodes(&nodes, 6, 1, {}, "region1.datacenter1.02.a.b", 6);

  ASSERT_EQ(26, nodes.size());

  Configuration::NodesConfig nodes_config(std::move(nodes));

  auto logs_config = std::make_shared<LocalLogsConfig>();
  for (size_t i = 1; i <= 200; ++i) {
    // log_id == nodeset_size for r=3 logs, log_id == nodeset_size + 100 for r=6
    // logs
    size_t nodeset_size = (i - 1) % 100 + 1;
    size_t replication_factor = i <= 100 ? 3 : 6;
    addLog(logs_config.get(),
           logid_t(i),
           replication_factor,
           0,
           nodeset_size,
           {},
           NodeLocationScope::RACK);
  }

  MetaDataLogsConfig metadata_config;
  metadata_config.nodeset_selector_type =
      NodeSetSelectorType::RANDOM_CROSSDOMAIN;

  auto config = std::make_shared<Configuration>(
      ServerConfig::fromData("nodeset_selector_test",
                             std::move(nodes_config),
                             std::move(metadata_config)),
      std::move(logs_config));

  auto selector =
      NodeSetSelectorFactory::create(NodeSetSelectorType::RANDOM_CROSSDOMAIN);

  const Configuration& cfg = *config;

  auto check_ns_size = [&](logid_t log_id, size_t expected_actual_size) {
    verify_result(selector.get(),
                  config,
                  log_id,
                  Decision::NEEDS_CHANGE,
                  [expected_actual_size, &cfg](StorageSet* storage_set) {
                    ld_check(storage_set != nullptr);
                    ASSERT_EQ(expected_actual_size, storage_set->size());
                  });
  };

  auto check_ns_size_r3 = [&](size_t setting_size,
                              size_t expected_actual_size) {
    check_ns_size(logid_t(setting_size), expected_actual_size);
  };

  auto check_ns_size_r6 = [&](size_t setting_size,
                              size_t expected_actual_size) {
    check_ns_size(logid_t(setting_size + 100), expected_actual_size);
  };

  // r = 3
  check_ns_size_r3(1, 5);
  check_ns_size_r3(7, 5);
  check_ns_size_r3(8, 10);
  check_ns_size_r3(12, 10);
  check_ns_size_r3(13, 15);
  check_ns_size_r3(17, 15);
  check_ns_size_r3(18, 20);
  check_ns_size_r3(20, 20);
  check_ns_size_r3(22, 20);
  check_ns_size_r3(23, 25);
  check_ns_size_r3(26, 25);
  check_ns_size_r3(100, 25);

  // r = 6
  check_ns_size_r6(1, 10);
  check_ns_size_r6(4, 10);
  check_ns_size_r6(5, 10);
  check_ns_size_r6(6, 10);
  check_ns_size_r6(10, 10);
  check_ns_size_r6(12, 10);
  check_ns_size_r6(26, 25);
}

TEST(RandomCrossDomainNodeSetSelectorTest, NodeExclusion) {
  // 26-node cluster with nodes from 5 different racks
  Nodes nodes;
  addNodes(&nodes, 5, 1, {}, "region0.datacenter1.01.a.a", 5);
  addNodes(&nodes, 5, 1, {}, "region0.datacenter2.01.a.a", 5);
  addNodes(&nodes, 5, 1, {}, "region0.datacenter1.01.a.b", 5);
  addNodes(&nodes, 5, 1, {}, "region1.datacenter1.02.a.a", 5);
  addNodes(&nodes, 6, 1, {}, "region1.datacenter1.02.a.b", 6);

  ASSERT_EQ(26, nodes.size());

  Configuration::NodesConfig nodes_config(std::move(nodes));

  auto logs_config = std::make_shared<LocalLogsConfig>();
  addLog(logs_config.get(),
         logid_t(1),
         3 /* replication_factor */,
         0,
         25 /* nodeset_size */,
         {},
         NodeLocationScope::RACK);

  auto config = std::make_shared<Configuration>(
      ServerConfig::fromData("nodeset_selector_test", std::move(nodes_config)),
      std::move(logs_config));

  auto selector =
      NodeSetSelectorFactory::create(NodeSetSelectorType::RANDOM_CROSSDOMAIN);

  const Configuration& cfg = *config;

  auto verify_domains = [&](size_t num_domains,
                            size_t nodes_per_domain,
                            StorageSet* storage_set) {
    ld_check(storage_set != nullptr);
    std::unordered_map<std::string, int> domains; // location to count map
    for (ShardID shard : *storage_set) {
      auto node = cfg.serverConfig()->getNode(shard.node());
      ld_check(node);
      ++domains[node->locationStr()];
    }
    ASSERT_EQ(num_domains, domains.size());
    for (auto& d : domains) {
      ASSERT_EQ(nodes_per_domain, d.second);
    }
  };

  // nodeset_size with one fully excluded rack in options
  NodeSetSelector::Options options;
  options.exclude_nodes = {20, 21, 22, 23, 24, 25};
  verify_result(selector.get(),
                config,
                logid_t(1),
                Decision::NEEDS_CHANGE,
                // should select 4 racks of 5 nodes each
                std::bind(verify_domains, 4, 5, std::placeholders::_1),
                &options);

  // nodeset generation and nodeset size if one rack is partially removed
  options.exclude_nodes = {20, 21, 22, 23};
  verify_result(selector.get(),
                config,
                logid_t(1),
                Decision::NEEDS_CHANGE,
                // should select 4 racks of 5 nodes each
                std::bind(verify_domains, 4, 5, std::placeholders::_1),
                &options);

  // nodeset generation and nodeset size if two racks is partially removed
  options.exclude_nodes = {15, 16, 17, 20, 21, 22, 23};
  verify_result(selector.get(),
                config,
                logid_t(1),
                Decision::NEEDS_CHANGE,
                // should select 3 racks of 5 nodes each
                std::bind(verify_domains, 3, 5, std::placeholders::_1),
                &options);

  // nodeset generation and nodeset size if three racks is partially removed
  options.exclude_nodes = {10, 11, 15, 16, 20, 21, 22};
  verify_result(selector.get(),
                config,
                logid_t(1),
                Decision::NEEDS_CHANGE,
                // should select 5 racks of 3 nodes each, not 2 racks of 5
                // nodes each
                std::bind(verify_domains, 5, 3, std::placeholders::_1),
                &options);
}

void basic_test(NodeSetSelectorType ns_type) {
  // 22-node cluster with nodes from 5 different racks
  Nodes nodes;
  std::vector<int> rack_sizes = {1, 5, 5, 6, 5};
  addNodes(&nodes, rack_sizes[0], 1, {}, "region0.datacenter1.01.a.a", 1);
  addNodes(&nodes, rack_sizes[1], 1, {}, "region0.datacenter2.01.a.a", 5);
  // Only 2 out 5 nodes are storage nodes.
  addNodes(&nodes, rack_sizes[2], 1, {}, "region0.datacenter1.01.a.b", 2);
  addNodes(&nodes, rack_sizes[3], 1, {}, "region1.datacenter1.02.a.a", 6);
  addNodes(&nodes, rack_sizes[4], 1, {}, "region1.datacenter1.02.a.b", 5);

  ASSERT_EQ(22, nodes.size());

  Configuration::NodesConfig nodes_config(std::move(nodes));

  auto logs_config = std::make_shared<LocalLogsConfig>();
  addLog(logs_config.get(),
         logid_t(1),
         ReplicationProperty(
             {{NodeLocationScope::RACK, 2}, {NodeLocationScope::NODE, 3}}),
         0,
         14 /* nodeset_size */);
  addLog(logs_config.get(),
         logid_t(2),
         ReplicationProperty(
             {{NodeLocationScope::RACK, 1}, {NodeLocationScope::NODE, 3}}),
         0,
         5 /* nodeset_size */);
  addLog(logs_config.get(),
         logid_t(3),
         ReplicationProperty({{NodeLocationScope::NODE, 4}}),
         0,
         2 /* nodeset_size */);
  addLog(logs_config.get(),
         logid_t(4),
         ReplicationProperty(
             {{NodeLocationScope::RACK, 3}, {NodeLocationScope::NODE, 4}}),
         0,
         150 /* nodeset_size */);
  addLog(logs_config.get(),
         logid_t(5),
         ReplicationProperty({{NodeLocationScope::RACK, 3}}),
         0,
         6 /* nodeset_size */);

  auto config = std::make_shared<Configuration>(
      ServerConfig::fromData("nodeset_selector_test", std::move(nodes_config)),
      std::move(logs_config));

  auto selector = NodeSetSelectorFactory::create(ns_type);

  auto nodes_per_domain = [&](StorageSet ss) -> std::vector<int> {
    std::vector<int> count(rack_sizes.size());
    size_t rack = 0;
    int nodes_before_rack = 0;
    for (ShardID s : ss) {
      EXPECT_EQ(0, s.shard());
      assert(s.node() >= nodes_before_rack);
      while (rack < rack_sizes.size() &&
             s.node() >= nodes_before_rack + rack_sizes[rack]) {
        nodes_before_rack += rack_sizes[rack];
        ++rack;
      }
      if (rack == rack_sizes.size()) {
        ADD_FAILURE() << toString(ss);
        return {};
      }
      ++count[rack];
    }
    assert(std::accumulate(count.begin(), count.end(), 0) == (int)ss.size());
    return count;
  };

  verify_result(selector.get(),
                config,
                logid_t(1),
                Decision::NEEDS_CHANGE,
                [&](StorageSet* ss) {
                  auto count = nodes_per_domain(*ss);
                  EXPECT_EQ(14, ss->size());
                  EXPECT_EQ(1, count[0]);
                  EXPECT_EQ(2, count[2]);
                  EXPECT_GE(count[1], 3);
                  EXPECT_GE(count[3], 3);
                  EXPECT_GE(count[4], 3);
                  EXPECT_LE(count[1], 4);
                  EXPECT_LE(count[3], 4);
                  EXPECT_LE(count[4], 4);
                });

  verify_result(selector.get(),
                config,
                logid_t(2),
                Decision::NEEDS_CHANGE,
                [&](StorageSet* ss) {
                  auto count = nodes_per_domain(*ss);
                  EXPECT_EQ(12, ss->size());
                  EXPECT_EQ(std::vector<int>({1, 3, 2, 3, 3}), count);
                });

  verify_result(selector.get(),
                config,
                logid_t(3),
                Decision::NEEDS_CHANGE,
                [&](StorageSet* ss) { EXPECT_EQ(4, ss->size()); });

  verify_result(selector.get(),
                config,
                logid_t(4),
                Decision::NEEDS_CHANGE,
                [&](StorageSet* ss) {
                  // Should select all 19 storage nodes.
                  EXPECT_EQ(19, ss->size());
                });

  verify_result(selector.get(),
                config,
                logid_t(5),
                Decision::NEEDS_CHANGE,
                [&](StorageSet* ss) {
                  EXPECT_EQ(6, ss->size());
                  // Should cover all 5 racks.
                  const auto& all_nodes = config->serverConfig()->getNodes();
                  std::set<std::string> racks;
                  for (auto s : *ss) {
                    racks.insert(all_nodes.at(s.node()).location->getDomain(
                        NodeLocationScope::RACK));
                  }
                  EXPECT_EQ(5, racks.size()) << toString(racks);
                });

  // Exclude a rack in options.
  NodeSetSelector::Options options;
  options.exclude_nodes = {1, 2, 3, 4, 5};
  verify_result(selector.get(),
                config,
                logid_t(2),
                Decision::NEEDS_CHANGE,
                [&](StorageSet* ss) {
                  auto count = nodes_per_domain(*ss);
                  EXPECT_EQ(9, ss->size());
                  EXPECT_EQ(std::vector<int>({1, 0, 2, 3, 3}), count);
                },
                &options);
}

TEST(WeightAwareNodeSetSelectorTest, ExcludeFromNodesets) {
  // 6-node cluster with nodes in 2 different racks
  Nodes nodes;
  addNodes(&nodes, 3, 1, "region0.datacenter1.01.a.a");
  addNodes(&nodes, 3, 1, "region0.datacenter1.01.a.b");

  ASSERT_EQ(6, nodes.size());
  // Settings exclude_from_nodesets on 3 nodes
  for (node_index_t node_id : {0, 1, 3}) {
    nodes[node_id].exclude_from_nodesets = true;
  }

  Configuration::NodesConfig nodes_config(std::move(nodes));

  auto logs_config = std::make_shared<LocalLogsConfig>();
  addLog(logs_config.get(),
         logid_t(1),
         ReplicationProperty(
             {{NodeLocationScope::RACK, 2}, {NodeLocationScope::NODE, 3}}),
         0,
         5 /* nodeset_size */);

  auto config = std::make_shared<Configuration>(
      ServerConfig::fromData("nodeset_selector_test", std::move(nodes_config)),
      std::move(logs_config));

  auto selector =
      NodeSetSelectorFactory::create(NodeSetSelectorType::WEIGHT_AWARE);

  const Configuration& cfg = *config;

  verify_result(selector.get(),
                config,
                logid_t(1),
                Decision::NEEDS_CHANGE,
                [&](StorageSet* ss) { EXPECT_EQ(3, ss->size()); });
}

TEST(WeightAwareNodeSetSelectorTest, Basic) {
  basic_test(NodeSetSelectorType::WEIGHT_AWARE_V2);
}

TEST(ConsistentHashingWeightAwareNodeSetSelectorTest, Basic) {
  basic_test(NodeSetSelectorType::CONSISTENT_HASHING_V2);
}

TEST(ConsistentHashingWeightAwareNodeSetSelectorTest, AddNode) {
  Nodes nodes1, nodes2;
  addNodes(&nodes1, 16, 1, {}, "region0.datacenter1.01.a.a", 16);
  addNodes(&nodes1, 16, 1, {}, "region0.datacenter2.01.a.a", 16);
  addNodes(&nodes1, 16, 1, {}, "region0.datacenter1.01.a.b", 16);
  addNodes(&nodes1, 16, 1, {}, "region1.datacenter1.02.a.a", 16);
  addNodes(&nodes1, 15, 1, {}, "region1.datacenter1.02.a.b", 15);

  nodes2 = nodes1;

  // another node added to the 5th rack
  addNodes(&nodes2, 1, 1, {}, "region1.datacenter1.02.a.b", 1);
  Configuration::NodesConfig nodes_config1(std::move(nodes1));
  Configuration::NodesConfig nodes_config2(std::move(nodes2));

  auto logs_config = std::make_shared<LocalLogsConfig>();
  const int numlogs = 10000;

  for (int i = 1; i <= numlogs; i++) {
    addLog(logs_config.get(),
           logid_t(i),
           ReplicationProperty(
               {{NodeLocationScope::RACK, 2}, {NodeLocationScope::NODE, 3}}),
           0,
           21 /* nodeset_size */);
  }
  auto logs_config2 = logs_config;

  auto config1 = std::make_shared<Configuration>(
      ServerConfig::fromData("nodeset_selector_test", std::move(nodes_config1)),
      std::move(logs_config));

  auto config2 = std::make_shared<Configuration>(
      ServerConfig::fromData("nodeset_selector_test", std::move(nodes_config2)),
      std::move(logs_config2));

  auto selector =
      NodeSetSelectorFactory::create(NodeSetSelectorType::CONSISTENT_HASHING);

  auto old_selector =
      NodeSetSelectorFactory::create(NodeSetSelectorType::WEIGHT_AWARE);

  size_t old_totalremoved = 0, old_totaladded = 0;
  size_t new_totalremoved = 0, new_totaladded = 0;
  std::map<ShardID, size_t> old_before_adding_distribution;
  std::map<ShardID, size_t> old_after_adding_distribution;
  std::map<ShardID, size_t> new_before_adding_distribution;
  std::map<ShardID, size_t> new_after_adding_distribution;
  for (int i = 1; i <= numlogs; i++) {
    std::pair<size_t, size_t> old_diff, new_diff;
    new_diff = compare_nodesets(selector.get(),
                                config1,
                                config2,
                                logid_t(i),
                                new_before_adding_distribution,
                                new_after_adding_distribution);
    old_diff = compare_nodesets(old_selector.get(),
                                config1,
                                config2,
                                logid_t(i),
                                old_before_adding_distribution,
                                old_after_adding_distribution);
    old_totalremoved += old_diff.first;
    old_totaladded += old_diff.second;
    new_totalremoved += new_diff.first;
    new_totaladded += new_diff.second;
  }

  ld_info("\n\nNew selector: removed = %zu, added = %zu\n",
          new_totalremoved,
          new_totaladded);
  ld_info("Old selector: removed = %zu, added = %zu\n",
          old_totalremoved,
          old_totaladded);

  ld_info("Distribution before adding for old selector: %s",
          toString(old_before_adding_distribution).c_str());
  ld_info("Distribution after adding for old selector: %s",
          toString(old_after_adding_distribution).c_str());

  ld_info("Distribution before adding for new selector: %s",
          toString(new_before_adding_distribution).c_str());
  ld_info("Distribution after adding for new selector: %s",
          toString(new_after_adding_distribution).c_str());

  for (auto& kv : old_after_adding_distribution) {
    EXPECT_GE(kv.second, 500);
    EXPECT_LE(kv.second, 4500);
  }

  for (auto& kv : new_after_adding_distribution) {
    EXPECT_GE(kv.second, 500);
    EXPECT_LE(kv.second, 4500);
  }

  EXPECT_EQ(new_totalremoved, new_totaladded);
  EXPECT_LE(new_totalremoved, 5000);
}
