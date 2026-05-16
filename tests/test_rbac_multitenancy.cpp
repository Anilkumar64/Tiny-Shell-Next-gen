#include "../server/MultiTenantManager.h"
#include "../server/RbacManager.h"
#include <gtest/gtest.h>

namespace tsh {

// RBAC Manager Tests
class RbacManagerTest : public ::testing::Test {
protected:
  RbacManager rbac_manager;
};

TEST_F(RbacManagerTest, DefaultRolesCreated) {
  auto roles = rbac_manager.get_all_roles();
  EXPECT_GE(roles.size(), 4); // At least viewer, operator, admin, auditor

  EXPECT_TRUE(rbac_manager.has_role("viewer"));
  EXPECT_TRUE(rbac_manager.has_role("operator"));
  EXPECT_TRUE(rbac_manager.has_role("admin"));
  EXPECT_TRUE(rbac_manager.has_role("auditor"));
}

TEST_F(RbacManagerTest, CreateCustomRole) {
  rbac_manager.create_role("custom_role", "Custom role for testing");

  EXPECT_TRUE(rbac_manager.has_role("custom_role"));
}

TEST_F(RbacManagerTest, GrantPermission) {
  rbac_manager.create_role("test_role");
  rbac_manager.grant_permission("test_role", Permission::EXECUTE_COMMAND);

  EXPECT_TRUE(
      rbac_manager.verify_permission("test_role", Permission::EXECUTE_COMMAND));
}

TEST_F(RbacManagerTest, RevokePermission) {
  rbac_manager.create_role("test_role");
  rbac_manager.grant_permission("test_role", Permission::EXECUTE_COMMAND);
  rbac_manager.revoke_permission("test_role", Permission::EXECUTE_COMMAND);

  EXPECT_FALSE(
      rbac_manager.verify_permission("test_role", Permission::EXECUTE_COMMAND));
}

TEST_F(RbacManagerTest, ViewerHasReadOnlyAccess) {
  EXPECT_TRUE(
      rbac_manager.verify_permission("viewer", Permission::READ_PROCESS_INFO));
  EXPECT_TRUE(rbac_manager.verify_permission("viewer",
                                             Permission::READ_SYSTEM_METRICS));
  EXPECT_FALSE(
      rbac_manager.verify_permission("viewer", Permission::EXECUTE_COMMAND));
}

TEST_F(RbacManagerTest, OperatorCanExecute) {
  EXPECT_TRUE(
      rbac_manager.verify_permission("operator", Permission::EXECUTE_COMMAND));
  EXPECT_TRUE(
      rbac_manager.verify_permission("operator", Permission::MANAGE_JOBS));
  EXPECT_FALSE(
      rbac_manager.verify_permission("operator", Permission::MANAGE_TENANTS));
}

TEST_F(RbacManagerTest, AdminHasAllPermissions) {
  EXPECT_TRUE(rbac_manager.verify_permission("admin", Permission::ADMIN));
  EXPECT_TRUE(
      rbac_manager.verify_permission("admin", Permission::EXECUTE_COMMAND));
  EXPECT_TRUE(
      rbac_manager.verify_permission("admin", Permission::MANAGE_TENANTS));
}

TEST_F(RbacManagerTest, DuplicateRoleThrows) {
  rbac_manager.create_role("duplicate");

  EXPECT_THROW(rbac_manager.create_role("duplicate"), std::runtime_error);
}

TEST_F(RbacManagerTest, NonexistentRoleThrows) {
  EXPECT_THROW(
      rbac_manager.grant_permission("nonexistent", Permission::EXECUTE_COMMAND),
      std::runtime_error);
}

TEST_F(RbacManagerTest, GetRolePermissions) {
  auto role_str = rbac_manager.get_role_permissions_string("admin");
  EXPECT_NE(role_str.find("ADMIN"), std::string::npos);
}

// Multi-Tenant Manager Tests
class MultiTenantManagerTest : public ::testing::Test {
protected:
  MultiTenantManager mt_manager;
};

TEST_F(MultiTenantManagerTest, CreateTenant) {
  std::string tenant_id =
      mt_manager.create_tenant("Test Tenant", "owner@example.com");

  EXPECT_FALSE(tenant_id.empty());
  EXPECT_TRUE(mt_manager.tenant_exists(tenant_id));
}

TEST_F(MultiTenantManagerTest, GetTenant) {
  std::string tenant_id =
      mt_manager.create_tenant("Test Tenant", "owner@example.com");
  const auto &tenant = mt_manager.get_tenant(tenant_id);

  EXPECT_EQ(tenant.tenant_name, "Test Tenant");
  EXPECT_EQ(tenant.owner, "owner@example.com");
  EXPECT_TRUE(tenant.is_active);
}

TEST_F(MultiTenantManagerTest, DeleteTenant) {
  std::string tenant_id =
      mt_manager.create_tenant("Test Tenant", "owner@example.com");
  mt_manager.delete_tenant(tenant_id);

  EXPECT_FALSE(mt_manager.tenant_exists(tenant_id));
}

TEST_F(MultiTenantManagerTest, SetTenantQuota) {
  std::string tenant_id =
      mt_manager.create_tenant("Test Tenant", "owner@example.com");
  mt_manager.set_tenant_quota(tenant_id, 50, 512, 25.0);

  const auto &tenant = mt_manager.get_tenant(tenant_id);
  EXPECT_EQ(tenant.resource_quota_jobs, 50);
  EXPECT_EQ(tenant.resource_quota_memory_mb, 512);
  EXPECT_EQ(tenant.resource_quota_cpu_percent, 25.0);
}

TEST_F(MultiTenantManagerTest, AddUserToTenant) {
  std::string tenant_id =
      mt_manager.create_tenant("Test Tenant", "owner@example.com");
  std::string user_id =
      mt_manager.add_user_to_tenant(tenant_id, "testuser", "viewer");

  EXPECT_FALSE(user_id.empty());
  EXPECT_TRUE(mt_manager.user_exists_in_tenant(user_id));
}

TEST_F(MultiTenantManagerTest, GetTenantUsers) {
  std::string tenant_id =
      mt_manager.create_tenant("Test Tenant", "owner@example.com");
  mt_manager.add_user_to_tenant(tenant_id, "user1", "viewer");
  mt_manager.add_user_to_tenant(tenant_id, "user2", "operator");

  auto users = mt_manager.get_tenant_users(tenant_id);
  EXPECT_EQ(users.size(), 2);
}

TEST_F(MultiTenantManagerTest, RemoveUserFromTenant) {
  std::string tenant_id =
      mt_manager.create_tenant("Test Tenant", "owner@example.com");
  std::string user_id =
      mt_manager.add_user_to_tenant(tenant_id, "testuser", "viewer");

  mt_manager.remove_user_from_tenant(user_id);

  EXPECT_FALSE(mt_manager.user_exists_in_tenant(user_id));
}

TEST_F(MultiTenantManagerTest, CreateContextForUser) {
  std::string tenant_id =
      mt_manager.create_tenant("Test Tenant", "owner@example.com");
  std::string user_id =
      mt_manager.add_user_to_tenant(tenant_id, "testuser", "operator");

  auto context = mt_manager.create_context(user_id);
  EXPECT_EQ(context.tenant_id, tenant_id);
  EXPECT_EQ(context.user_id, user_id);
  EXPECT_EQ(context.role, "operator");
}

TEST_F(MultiTenantManagerTest, VerifyUserPermission) {
  std::string tenant_id =
      mt_manager.create_tenant("Test Tenant", "owner@example.com");
  std::string viewer_id =
      mt_manager.add_user_to_tenant(tenant_id, "viewer_user", "viewer");
  std::string operator_id =
      mt_manager.add_user_to_tenant(tenant_id, "operator_user", "operator");

  EXPECT_TRUE(mt_manager.verify_user_permission(viewer_id,
                                                Permission::READ_PROCESS_INFO));
  EXPECT_FALSE(mt_manager.verify_user_permission(viewer_id,
                                                 Permission::EXECUTE_COMMAND));

  EXPECT_TRUE(mt_manager.verify_user_permission(operator_id,
                                                Permission::EXECUTE_COMMAND));
}

TEST_F(MultiTenantManagerTest, CheckResourceQuota) {
  std::string tenant_id =
      mt_manager.create_tenant("Test Tenant", "owner@example.com");
  mt_manager.set_tenant_quota(tenant_id, 10, 512, 50.0);

  // Should be within quota
  EXPECT_TRUE(mt_manager.is_operation_within_quota(tenant_id, 5, 256));

  // Should exceed job quota
  EXPECT_FALSE(mt_manager.is_operation_within_quota(tenant_id, 15, 256));

  // Should exceed memory quota
  EXPECT_FALSE(mt_manager.is_operation_within_quota(tenant_id, 5, 600));
}

TEST_F(MultiTenantManagerTest, TenantStatusReport) {
  std::string tenant_id =
      mt_manager.create_tenant("Test Tenant", "owner@example.com");
  mt_manager.add_user_to_tenant(tenant_id, "user1", "viewer");
  mt_manager.add_user_to_tenant(tenant_id, "user2", "operator");

  auto report = mt_manager.get_tenant_status_report(tenant_id);
  EXPECT_NE(report.find("Test Tenant"), std::string::npos);
  EXPECT_NE(report.find("ACTIVE"), std::string::npos);
  EXPECT_NE(report.find("Users: 2"), std::string::npos);
}

TEST_F(MultiTenantManagerTest, TenantCount) {
  mt_manager.create_tenant("Tenant1", "owner1@example.com");
  mt_manager.create_tenant("Tenant2", "owner2@example.com");

  EXPECT_EQ(mt_manager.get_tenant_count(), 2);
}

TEST_F(MultiTenantManagerTest, UserCount) {
  std::string tenant_id1 =
      mt_manager.create_tenant("Tenant1", "owner1@example.com");
  std::string tenant_id2 =
      mt_manager.create_tenant("Tenant2", "owner2@example.com");

  mt_manager.add_user_to_tenant(tenant_id1, "user1", "viewer");
  mt_manager.add_user_to_tenant(tenant_id1, "user2", "operator");
  mt_manager.add_user_to_tenant(tenant_id2, "user3", "viewer");

  EXPECT_EQ(mt_manager.get_user_count(), 3);
}

TEST_F(MultiTenantManagerTest, InvalidRoleThrows) {
  std::string tenant_id =
      mt_manager.create_tenant("Test Tenant", "owner@example.com");

  EXPECT_THROW(mt_manager.add_user_to_tenant(tenant_id, "user", "invalid_role"),
               std::runtime_error);
}

} // namespace tsh
