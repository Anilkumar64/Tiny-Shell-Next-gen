#pragma once

#include "AgentIdentity.h"
#include "RbacManager.h"
#include <chrono>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace tsh {

struct Tenant {
  std::string tenant_id;
  std::string tenant_name;
  std::string owner;
  std::chrono::steady_clock::time_point created_at;
  bool is_active;
  int resource_quota_jobs;           // Max concurrent jobs
  int resource_quota_memory_mb;      // Max memory usage
  double resource_quota_cpu_percent; // Max CPU percentage

  Tenant(const std::string &id, const std::string &name, const std::string &own)
      : tenant_id(id), tenant_name(name), owner(own),
        created_at(std::chrono::steady_clock::now()), is_active(true),
        resource_quota_jobs(100), resource_quota_memory_mb(1024),
        resource_quota_cpu_percent(50.0) {}
};

struct TenantUser {
  std::string user_id;
  std::string username;
  std::string tenant_id;
  std::string role;
  bool is_active;
  std::chrono::steady_clock::time_point created_at;

  TenantUser(const std::string &uid, const std::string &uname,
             const std::string &tid, const std::string &r)
      : user_id(uid), username(uname), tenant_id(tid), role(r), is_active(true),
        created_at(std::chrono::steady_clock::now()) {}
};

struct TenantContext {
  std::string tenant_id;
  std::string user_id;
  std::string role;
  std::map<std::string, std::string> metadata;

  TenantContext(const std::string &tid, const std::string &uid,
                const std::string &r)
      : tenant_id(tid), user_id(uid), role(r) {}
};

class MultiTenantManager {
public:
  MultiTenantManager() : rbac_manager_(std::make_unique<RbacManager>()) {}

  std::string create_tenant(const std::string &tenant_name,
                            const std::string &owner) {
    // BUG: libuuid made tenant creation non-portable across build targets.
    // FIX: use the project-local UUIDv4 generator already used for agents.
    std::string tenant_id = AgentIdentity::new_uuid();
    Tenant new_tenant(tenant_id, tenant_name, owner);

    tenants_.emplace(tenant_id, new_tenant);
    return tenant_id;
  }

  void delete_tenant(const std::string &tenant_id) {
    if (!tenants_.count(tenant_id)) {
      throw std::runtime_error("Tenant not found: " + tenant_id);
    }

    // Remove all users associated with this tenant
    auto user_it = tenant_users_.begin();
    while (user_it != tenant_users_.end()) {
      if (user_it->second.tenant_id == tenant_id) {
        user_it = tenant_users_.erase(user_it);
      } else {
        ++user_it;
      }
    }

    tenants_.erase(tenant_id);
  }

  bool tenant_exists(const std::string &tenant_id) const {
    return tenants_.count(tenant_id) > 0;
  }

  const Tenant &get_tenant(const std::string &tenant_id) const {
    if (!tenants_.count(tenant_id)) {
      throw std::runtime_error("Tenant not found: " + tenant_id);
    }
    return tenants_.at(tenant_id);
  }

  void set_tenant_quota(const std::string &tenant_id, int job_quota,
                        int memory_mb, double cpu_percent) {
    if (!tenants_.count(tenant_id)) {
      throw std::runtime_error("Tenant not found: " + tenant_id);
    }

    auto &tenant = tenants_.at(tenant_id);
    tenant.resource_quota_jobs = job_quota;
    tenant.resource_quota_memory_mb = memory_mb;
    tenant.resource_quota_cpu_percent = cpu_percent;
  }

  std::string add_user_to_tenant(const std::string &tenant_id,
                                 const std::string &username,
                                 const std::string &role) {
    if (!tenants_.count(tenant_id)) {
      throw std::runtime_error("Tenant not found: " + tenant_id);
    }

    if (!rbac_manager_->has_role(role)) {
      throw std::runtime_error("Role not found: " + role);
    }

    // BUG: libuuid made user creation non-portable across build targets.
    // FIX: use the project-local UUIDv4 generator already used for agents.
    std::string user_id = AgentIdentity::new_uuid();
    TenantUser new_user(user_id, username, tenant_id, role);

    tenant_users_.emplace(user_id, new_user);
    return user_id;
  }

  void remove_user_from_tenant(const std::string &user_id) {
    if (!tenant_users_.count(user_id)) {
      throw std::runtime_error("User not found: " + user_id);
    }
    tenant_users_.erase(user_id);
  }

  bool user_exists_in_tenant(const std::string &user_id) const {
    return tenant_users_.count(user_id) > 0;
  }

  const TenantUser &get_user(const std::string &user_id) const {
    if (!tenant_users_.count(user_id)) {
      throw std::runtime_error("User not found: " + user_id);
    }
    return tenant_users_.at(user_id);
  }

  std::vector<TenantUser> get_tenant_users(const std::string &tenant_id) const {
    std::vector<TenantUser> users;
    for (const auto &[uid, user] : tenant_users_) {
      if (user.tenant_id == tenant_id) {
        users.push_back(user);
      }
    }
    return users;
  }

  TenantContext create_context(const std::string &user_id) {
    if (!tenant_users_.count(user_id)) {
      throw std::runtime_error("User not found: " + user_id);
    }

    const auto &user = tenant_users_.at(user_id);
    return TenantContext(user.tenant_id, user_id, user.role);
  }

  bool verify_user_permission(const std::string &user_id, Permission perm) {
    if (!tenant_users_.count(user_id)) {
      return false;
    }

    const auto &user = tenant_users_.at(user_id);
    return rbac_manager_->verify_permission(user.role, perm);
  }

  bool is_operation_within_quota(const std::string &tenant_id, int current_jobs,
                                 int memory_used_mb) {
    if (!tenants_.count(tenant_id)) {
      return false;
    }

    const auto &tenant = tenants_.at(tenant_id);
    return current_jobs < tenant.resource_quota_jobs &&
           memory_used_mb < tenant.resource_quota_memory_mb;
  }

  std::string get_tenant_status_report(const std::string &tenant_id) const {
    if (!tenants_.count(tenant_id)) {
      return "Tenant not found";
    }

    const auto &tenant = tenants_.at(tenant_id);
    std::string report;

    report += "=== Tenant Status Report ===\n";
    report += "Tenant ID: " + tenant.tenant_id + "\n";
    report += "Tenant Name: " + tenant.tenant_name + "\n";
    report += "Owner: " + tenant.owner + "\n";
    report +=
        "Status: " + std::string(tenant.is_active ? "ACTIVE" : "INACTIVE") +
        "\n";
    report += "Job Quota: " + std::to_string(tenant.resource_quota_jobs) + "\n";
    report +=
        "Memory Quota: " + std::to_string(tenant.resource_quota_memory_mb) +
        " MB\n";
    report +=
        "CPU Quota: " + std::to_string(tenant.resource_quota_cpu_percent) +
        "%\n";

    auto users = get_tenant_users(tenant_id);
    report += "Users: " + std::to_string(users.size()) + "\n";
    for (const auto &user : users) {
      report += "  - " + user.username + " (" + user.role + ")\n";
    }

    return report;
  }

  int get_tenant_count() const { return tenants_.size(); }

  int get_user_count() const { return tenant_users_.size(); }

  RbacManager &get_rbac_manager() { return *rbac_manager_; }

private:
  std::map<std::string, Tenant> tenants_;
  std::map<std::string, TenantUser> tenant_users_;
  std::unique_ptr<RbacManager> rbac_manager_;
};

} // namespace tsh
