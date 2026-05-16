#pragma once

#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace tsh {

enum class Permission {
  // Command execution
  EXECUTE_COMMAND = 0x0001,
  EXECUTE_PIPELINE = 0x0002,

  // System introspection
  READ_PROCESS_INFO = 0x0004,
  READ_SYSTEM_METRICS = 0x0008,

  // Administrative
  MANAGE_JOBS = 0x0010,
  MANAGE_RESOURCES = 0x0020,

  // Audit
  READ_AUDIT_LOG = 0x0040,

  // Cluster
  MANAGE_CLUSTER = 0x0080,
  READ_CLUSTER_STATUS = 0x0100,

  // Tenancy
  MANAGE_TENANTS = 0x0200,

  // Full admin
  ADMIN = 0xFFFF
};

struct Role {
  std::string name;
  std::set<Permission> permissions;
  std::string description;

  bool has_permission(Permission perm) const {
    return permissions.count(perm) > 0 ||
           permissions.count(Permission::ADMIN) > 0;
  }
};

class RbacManager {
public:
  RbacManager() {
    // Initialize default roles
    setup_default_roles();
  }

  void create_role(const std::string &name,
                   const std::string &description = "") {
    if (roles_.count(name)) {
      throw std::runtime_error("Role already exists: " + name);
    }
    roles_[name] = Role{name, {}, description};
  }

  void grant_permission(const std::string &role_name, Permission perm) {
    if (!roles_.count(role_name)) {
      throw std::runtime_error("Role not found: " + role_name);
    }
    roles_[role_name].permissions.insert(perm);
  }

  void revoke_permission(const std::string &role_name, Permission perm) {
    if (!roles_.count(role_name)) {
      throw std::runtime_error("Role not found: " + role_name);
    }
    roles_[role_name].permissions.erase(perm);
  }

  bool has_role(const std::string &role_name) const {
    return roles_.count(role_name) > 0;
  }

  bool verify_permission(const std::string &role_name, Permission perm) const {
    if (!roles_.count(role_name)) {
      return false;
    }
    return roles_.at(role_name).has_permission(perm);
  }

  std::vector<std::string> get_all_roles() const {
    std::vector<std::string> role_names;
    for (const auto &[name, role] : roles_) {
      role_names.push_back(name);
    }
    return role_names;
  }

  const Role &get_role(const std::string &role_name) const {
    if (!roles_.count(role_name)) {
      throw std::runtime_error("Role not found: " + role_name);
    }
    return roles_.at(role_name);
  }

  std::string get_role_permissions_string(const std::string &role_name) const {
    if (!roles_.count(role_name)) {
      return "Role not found";
    }

    const auto &role = roles_.at(role_name);
    std::string perms = "Role: " + role_name + "\nPermissions:\n";

    for (auto perm : role.permissions) {
      perms += "  - " + permission_to_string(perm) + "\n";
    }

    return perms;
  }

private:
  std::map<std::string, Role> roles_;

  void setup_default_roles() {
    // Viewer role - read-only access
    create_role("viewer", "Read-only access to metrics and status");
    grant_permission("viewer", Permission::READ_PROCESS_INFO);
    grant_permission("viewer", Permission::READ_SYSTEM_METRICS);
    grant_permission("viewer", Permission::READ_AUDIT_LOG);
    grant_permission("viewer", Permission::READ_CLUSTER_STATUS);

    // Operator role - can execute commands
    create_role("operator", "Can execute commands and manage jobs");
    grant_permission("operator", Permission::EXECUTE_COMMAND);
    grant_permission("operator", Permission::EXECUTE_PIPELINE);
    grant_permission("operator", Permission::MANAGE_JOBS);
    grant_permission("operator", Permission::READ_PROCESS_INFO);
    grant_permission("operator", Permission::READ_SYSTEM_METRICS);
    grant_permission("operator", Permission::READ_CLUSTER_STATUS);
    // FIX: operator role needs READ_AUDIT_LOG to view the audit stream and
    // server panel audit tab without flooding the event log with AUTH_DENIED.
    grant_permission("operator", Permission::READ_AUDIT_LOG);

    // Administrator role - full access
    create_role("admin", "Full administrative access");
    grant_permission("admin", Permission::ADMIN);

    // Auditor role - audit trail access
    create_role("auditor", "Can read audit logs and cluster status");
    grant_permission("auditor", Permission::READ_AUDIT_LOG);
    grant_permission("auditor", Permission::READ_CLUSTER_STATUS);

    // Cluster manager role
    create_role("cluster_manager", "Can manage cluster nodes and resources");
    grant_permission("cluster_manager", Permission::MANAGE_CLUSTER);
    grant_permission("cluster_manager", Permission::READ_CLUSTER_STATUS);
  }

  std::string permission_to_string(Permission perm) const {
    switch (perm) {
    case Permission::EXECUTE_COMMAND:
      return "EXECUTE_COMMAND";
    case Permission::EXECUTE_PIPELINE:
      return "EXECUTE_PIPELINE";
    case Permission::READ_PROCESS_INFO:
      return "READ_PROCESS_INFO";
    case Permission::READ_SYSTEM_METRICS:
      return "READ_SYSTEM_METRICS";
    case Permission::MANAGE_JOBS:
      return "MANAGE_JOBS";
    case Permission::MANAGE_RESOURCES:
      return "MANAGE_RESOURCES";
    case Permission::READ_AUDIT_LOG:
      return "READ_AUDIT_LOG";
    case Permission::MANAGE_CLUSTER:
      return "MANAGE_CLUSTER";
    case Permission::READ_CLUSTER_STATUS:
      return "READ_CLUSTER_STATUS";
    case Permission::MANAGE_TENANTS:
      return "MANAGE_TENANTS";
    case Permission::ADMIN:
      return "ADMIN";
    default:
      return "UNKNOWN";
    }
  }
};

} // namespace tsh