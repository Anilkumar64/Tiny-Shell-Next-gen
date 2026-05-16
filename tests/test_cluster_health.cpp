#include <gtest/gtest.h>
#include "../server/ClusterHealthManager.h"
#include <chrono>
#include <thread>

namespace tsh {

class ClusterHealthManagerTest : public ::testing::Test {
protected:
    ClusterHealthManager manager;
};

TEST_F(ClusterHealthManagerTest, AddNode) {
    manager.add_node("localhost", 9000);
    
    EXPECT_EQ(manager.get_cluster_size(), 1);
}

TEST_F(ClusterHealthManagerTest, AddMultipleNodes) {
    manager.add_node("node1", 9000);
    manager.add_node("node2", 9001);
    manager.add_node("node3", 9002);
    
    EXPECT_EQ(manager.get_cluster_size(), 3);
}

TEST_F(ClusterHealthManagerTest, RemoveNode) {
    manager.add_node("node1", 9000);
    manager.add_node("node2", 9001);
    
    manager.remove_node("node1", 9000);
    
    EXPECT_EQ(manager.get_cluster_size(), 1);
}

TEST_F(ClusterHealthManagerTest, MarkSuccess) {
    manager.add_node("node1", 9000);
    manager.mark_success("node1", 9000, 10.5);
    
    auto healthy = manager.get_healthy_nodes();
    EXPECT_EQ(healthy.size(), 1);
}

TEST_F(ClusterHealthManagerTest, MarkFailure) {
    manager.add_node("node1", 9000);
    
    for (int i = 0; i < 5; ++i) {
        manager.mark_failure("node1", 9000);
    }
    
    auto all = manager.get_all_nodes();
    EXPECT_EQ(all[0]->status.load(), WorkerHealthStatus::DEGRADED);
}

TEST_F(ClusterHealthManagerTest, ConsecutiveFailureTracking) {
    manager.add_node("node1", 9000);
    
    manager.mark_failure("node1", 9000);
    manager.mark_failure("node1", 9000);
    
    auto all = manager.get_all_nodes();
    EXPECT_EQ(all[0]->consecutive_failures.load(), 2);
}

TEST_F(ClusterHealthManagerTest, HealthScore) {
    manager.add_node("node1", 9000);
    
    // New node with no jobs - neutral score
    auto all = manager.get_all_nodes();
    EXPECT_EQ(all[0]->health_score(), 50.0);
    
    // Mark successful jobs
    for (int i = 0; i < 5; ++i) {
        manager.mark_success("node1", 9000);
    }
    
    all = manager.get_all_nodes();
    double score = all[0]->health_score();
    EXPECT_GT(score, 50.0); // Should be better than neutral
}

TEST_F(ClusterHealthManagerTest, GetHealthyNodes) {
    manager.add_node("node1", 9000);
    manager.add_node("node2", 9001);
    manager.add_node("node3", 9002);
    
    manager.mark_success("node1", 9000);
    manager.mark_success("node2", 9001);
    manager.mark_heartbeat_missed("node3", 9002);
    manager.mark_heartbeat_missed("node3", 9002);
    manager.mark_heartbeat_missed("node3", 9002);
    
    auto healthy = manager.get_healthy_nodes();
    EXPECT_EQ(healthy.size(), 2);
}

TEST_F(ClusterHealthManagerTest, ClusterHealthPercentage) {
    manager.add_node("node1", 9000);
    manager.add_node("node2", 9001);
    manager.add_node("node3", 9002);
    
    manager.mark_success("node1", 9000);
    manager.mark_success("node2", 9001);
    manager.mark_heartbeat_missed("node3", 9002);
    manager.mark_heartbeat_missed("node3", 9002);
    manager.mark_heartbeat_missed("node3", 9002);
    
    double health = manager.get_cluster_health_percentage();
    EXPECT_DOUBLE_EQ(health, 66.66666666666666); // 2 out of 3
}

TEST_F(ClusterHealthManagerTest, EmptyClusterHealth) {
    double health = manager.get_cluster_health_percentage();
    EXPECT_EQ(health, 0.0);
}

TEST_F(ClusterHealthManagerTest, StatusReport) {
    manager.add_node("node1", 9000);
    manager.mark_success("node1", 9000, 5.0);
    
    std::string report = manager.get_cluster_status_report();
    EXPECT_NE(report.find("Cluster Health Report"), std::string::npos);
    EXPECT_NE(report.find("node1:9000"), std::string::npos);
    EXPECT_NE(report.find("HEALTHY"), std::string::npos);
}

TEST_F(ClusterHealthManagerTest, ThreeMissedHeartbeatsMarksUnreachable) {
    manager.add_node("worker-02", 9002);

    manager.mark_heartbeat_missed("worker-02", 9002);
    manager.mark_heartbeat_missed("worker-02", 9002);
    manager.mark_heartbeat_missed("worker-02", 9002);

    auto healthy = manager.get_healthy_nodes();
    EXPECT_TRUE(healthy.empty());
    auto all = manager.get_all_nodes();
    EXPECT_EQ(all[0]->status.load(), WorkerHealthStatus::UNREACHABLE);
}

TEST_F(ClusterHealthManagerTest, FiveJobFailuresMarksDegraded) {
    manager.add_node("worker-01", 9001);

    for (int i = 0; i < 5; ++i) {
        manager.mark_failure("worker-01", 9001);
    }

    auto all = manager.get_all_nodes();
    EXPECT_EQ(all[0]->status.load(), WorkerHealthStatus::DEGRADED);
}

TEST_F(ClusterHealthManagerTest, ResponseTimeTracking) {
    manager.add_node("node1", 9000);
    manager.mark_success("node1", 9000, 25.5);
    
    auto all = manager.get_all_nodes();
    EXPECT_DOUBLE_EQ(all[0]->get_response_time_ms(), 25.5);
}

TEST_F(ClusterHealthManagerTest, RetryExecutionSuccess) {
    manager.add_node("node1", 9000);
    
    int call_count = 0;
    bool success = manager.execute_with_retry("node1", 9000, [&call_count]() {
        call_count++;
        return true;
    });
    
    EXPECT_TRUE(success);
    EXPECT_EQ(call_count, 1); // Should succeed on first try
    
    auto all = manager.get_all_nodes();
    EXPECT_EQ(all[0]->successful_jobs.load(), 1);
}

TEST_F(ClusterHealthManagerTest, RetryExecutionWithFailures) {
    manager.add_node("node1", 9000);
    
    int call_count = 0;
    bool success = manager.execute_with_retry("node1", 9000, [&call_count]() {
        call_count++;
        return call_count >= 3; // Succeed on 3rd attempt
    }, 3);
    
    EXPECT_TRUE(success);
    EXPECT_EQ(call_count, 3); // Should require 3 attempts
}

TEST_F(ClusterHealthManagerTest, RetryExecutionMaxRetriesExceeded) {
    manager.add_node("node1", 9000);
    
    int call_count = 0;
    bool success = manager.execute_with_retry("node1", 9000, [&call_count]() {
        call_count++;
        return false; // Always fail
    }, 2);
    
    EXPECT_FALSE(success);
    EXPECT_EQ(call_count, 2);
    
    auto all = manager.get_all_nodes();
    EXPECT_EQ(all[0]->failed_jobs.load(), 2);
}

TEST_F(ClusterHealthManagerTest, HealthCheckThread) {
    manager.add_node("node1", 9000);
    manager.mark_success("node1", 9000);
    
    manager.start_health_check();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    manager.stop();
    
    EXPECT_EQ(manager.get_cluster_size(), 1);
}

TEST_F(ClusterHealthManagerTest, NodeSorting) {
    manager.add_node("node1", 9000);
    manager.add_node("node2", 9001);
    
    // Node1 with high success rate
    for (int i = 0; i < 10; ++i) {
        manager.mark_success("node1", 9000);
    }
    
    // Node2 with lower success rate
    for (int i = 0; i < 3; ++i) {
        manager.mark_success("node2", 9001);
        manager.mark_failure("node2", 9001);
    }
    
    auto healthy = manager.get_healthy_nodes();
    EXPECT_EQ(healthy[0]->host, "node1"); // Should be first (higher score)
}

TEST_F(ClusterHealthManagerTest, RetryNodeNotFound) {
    bool success = manager.execute_with_retry("nonexistent", 9999, []() {
        return true;
    });
    
    EXPECT_FALSE(success);
}

} // namespace tsh
