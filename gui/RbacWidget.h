#pragma once
#include "ApiClient.h"
#include "TshStyle.h"
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QSplitter>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QWidget>

// UI for managing RBAC roles and their permissions, mirroring RbacManager.h
class RbacWidget : public QWidget {
  Q_OBJECT

  struct RoleData {
    QString name;
    QString description;
    QSet<QString> permissions;
  };

  static QStringList allPermissions() {
    return {"EXECUTE_COMMAND",
            "EXECUTE_PIPELINE",
            "READ_PROCESS_INFO",
            "READ_SYSTEM_METRICS",
            "MANAGE_JOBS",
            "MANAGE_RESOURCES",
            "READ_AUDIT_LOG",
            "MANAGE_CLUSTER",
            "READ_CLUSTER_STATUS",
            "MANAGE_TENANTS",
            "ADMIN"};
  }

public:
  explicit RbacWidget(tsh::ApiClient *api, QWidget *parent = nullptr)
      : QWidget(parent), m_api(api) {
    buildUi();
    seedDefaultRoles();
  }

private:
  void buildUi() {
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(28, 24, 28, 24);
    root->setSpacing(16);

    // ── Header ──────────────────────────────────────────────────────────
    auto *hdrRow = new QHBoxLayout;
    auto *titleLbl = new QLabel("RBAC — Roles & Permissions", this);
    titleLbl->setStyleSheet("font-size:22px; font-weight:700; color:#e8eaf0;");
    hdrRow->addWidget(titleLbl);
    hdrRow->addStretch();
    root->addLayout(hdrRow);

    // ── Splitter: role list | permission editor ──────────────────────────
    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setHandleWidth(1);
    root->addWidget(splitter, 1);

    // Left: role list panel
    auto *leftPanel = new QFrame(splitter);
    leftPanel->setStyleSheet("QFrame { background:#1c1f2b; border:1px solid "
                             "#2a2d3e; border-radius:10px; }");
    auto *leftVbox = new QVBoxLayout(leftPanel);
    leftVbox->setContentsMargins(12, 12, 12, 12);
    leftVbox->setSpacing(8);

    auto *rolesHdr = new QLabel("ROLES", leftPanel);
    rolesHdr->setStyleSheet(
        "font-size:10px; font-weight:600; letter-spacing:1px; color:#4a5068;");
    leftVbox->addWidget(rolesHdr);

    m_roleList = new QListWidget(leftPanel);
    m_roleList->setStyleSheet(
        "QListWidget { background:transparent; border:none; }"
        "QListWidget::item { padding:10px 12px; border-radius:6px; "
        "color:#e8eaf0; }"
        "QListWidget::item:hover { background:#22263a; }"
        "QListWidget::item:selected { background:#0077a8; color:#fff; }");
    m_roleList->setFrameShape(QFrame::NoFrame);
    connect(m_roleList, &QListWidget::currentRowChanged, this,
            &RbacWidget::onRoleSelected);
    leftVbox->addWidget(m_roleList, 1);

    auto *addRoleBtn = new QPushButton("+ New Role", leftPanel);
    addRoleBtn->setProperty("role", "primary");
    addRoleBtn->setFixedHeight(34);
    connect(addRoleBtn, &QPushButton::clicked, this,
            &RbacWidget::showAddRoleDialog);
    leftVbox->addWidget(addRoleBtn);

    auto *delRoleBtn = new QPushButton("✕ Delete Role", leftPanel);
    delRoleBtn->setProperty("role", "danger");
    delRoleBtn->setFixedHeight(34);
    connect(delRoleBtn, &QPushButton::clicked, this,
            &RbacWidget::deleteCurrentRole);
    leftVbox->addWidget(delRoleBtn);

    splitter->addWidget(leftPanel);

    // Right: permission editor panel
    auto *rightPanel = new QFrame(splitter);
    rightPanel->setStyleSheet("QFrame { background:#1c1f2b; border:1px solid "
                              "#2a2d3e; border-radius:10px; }");
    auto *rightVbox = new QVBoxLayout(rightPanel);
    rightVbox->setContentsMargins(20, 16, 20, 16);
    rightVbox->setSpacing(12);

    m_roleNameLbl = new QLabel("Select a role", rightPanel);
    m_roleNameLbl->setStyleSheet(
        "font-size:18px; font-weight:700; color:#e8eaf0;");

    m_roleDescLbl = new QLabel("", rightPanel);
    m_roleDescLbl->setStyleSheet("font-size:12px; color:#8891b0;");

    auto *permHdr = new QLabel("PERMISSIONS", rightPanel);
    permHdr->setStyleSheet(
        "font-size:10px; font-weight:600; letter-spacing:1px; color:#4a5068; "
        "margin-top:8px;");

    // Permission checkboxes in a scroll area
    m_permScroll = new QScrollArea(rightPanel);
    m_permScroll->setWidgetResizable(true);
    m_permScroll->setFrameShape(QFrame::NoFrame);
    m_permScroll->setStyleSheet("background:transparent;");

    m_permContainer = new QWidget;
    m_permLayout = new QVBoxLayout(m_permContainer);
    m_permLayout->setContentsMargins(0, 0, 0, 0);
    m_permLayout->setSpacing(6);

    for (const QString &perm : allPermissions()) {
      auto *cb = new QCheckBox(perm, m_permContainer);
      cb->setStyleSheet(QString("QCheckBox { color:#e8eaf0; "
                                "font-family:'JetBrains Mono','Consolas',mono;"
                                " font-size:12px; }"
                                "QCheckBox::indicator:checked { background:%1; "
                                "border-color:%1; }")
                            .arg(perm == "ADMIN" ? "#fbbf24" : "#00b4d8"));
      connect(cb, &QCheckBox::toggled, this, &RbacWidget::onPermissionToggled);
      m_permLayout->addWidget(cb);
      m_permChecks[perm] = cb;
    }
    m_permLayout->addStretch();
    m_permScroll->setWidget(m_permContainer);

    auto *saveBtn = new QPushButton("Save Permissions", rightPanel);
    saveBtn->setProperty("role", "primary");
    saveBtn->setFixedHeight(36);
    connect(saveBtn, &QPushButton::clicked, this, &RbacWidget::savePermissions);

    rightVbox->addWidget(m_roleNameLbl);
    rightVbox->addWidget(m_roleDescLbl);
    rightVbox->addWidget(permHdr);
    rightVbox->addWidget(m_permScroll, 1);
    rightVbox->addWidget(saveBtn);
    splitter->addWidget(rightPanel);

    splitter->setSizes({220, 600});

    setPermEditorEnabled(false);
  }

  void seedDefaultRoles() {
    auto addRole = [&](const QString &name, const QString &desc,
                       const QSet<QString> &perms) {
      m_roles[name] = {name, desc, perms};
      auto *item = new QListWidgetItem(name, m_roleList);
      item->setToolTip(desc);
    };

    addRole("admin", "Full system administrator", {"ADMIN"});
    addRole("operator", "Day-to-day cluster operations",
            {"EXECUTE_COMMAND", "EXECUTE_PIPELINE", "READ_PROCESS_INFO",
             "READ_SYSTEM_METRICS", "MANAGE_JOBS", "READ_CLUSTER_STATUS"});
    addRole(
        "viewer", "Read-only system access",
        {"READ_PROCESS_INFO", "READ_SYSTEM_METRICS", "READ_CLUSTER_STATUS"});
    addRole("auditor", "Security and compliance review",
            {"READ_AUDIT_LOG", "READ_SYSTEM_METRICS"});
    addRole("scheduler", "Job scheduling only",
            {"MANAGE_JOBS", "EXECUTE_PIPELINE", "READ_CLUSTER_STATUS"});
  }

  void onRoleSelected(int row) {
    if (row < 0) {
      setPermEditorEnabled(false);
      return;
    }
    const QString name = m_roleList->item(row)->text();
    if (!m_roles.contains(name))
      return;
    m_currentRole = name;
    const auto &role = m_roles[name];
    m_roleNameLbl->setText(name);
    m_roleDescLbl->setText(role.description);
    setPermEditorEnabled(true);

    for (auto it = m_permChecks.begin(); it != m_permChecks.end(); ++it) {
      QSignalBlocker blk(it.value());
      it.value()->setChecked(role.permissions.contains(it.key()));
    }
  }

  void onPermissionToggled() {
    // If ADMIN checked, check all; if ADMIN unchecked, uncheck all if
    // applicable
    if (m_currentRole.isEmpty())
      return;
    auto *adminCb = m_permChecks.value("ADMIN");
    if (adminCb && adminCb->isChecked()) {
      for (auto *cb : m_permChecks) {
        if (cb != adminCb) {
          QSignalBlocker blk(cb);
          cb->setChecked(true);
          cb->setEnabled(false);
        }
      }
    } else {
      for (auto *cb : m_permChecks)
        cb->setEnabled(true);
    }
  }

  void savePermissions() {
    if (m_currentRole.isEmpty())
      return;
    auto &role = m_roles[m_currentRole];
    role.permissions.clear();
    for (auto it = m_permChecks.begin(); it != m_permChecks.end(); ++it) {
      if (it.value()->isChecked())
        role.permissions.insert(it.key());
    }
    QMessageBox::information(
        this, "Saved",
        QString("Permissions saved for role <b>%1</b>.").arg(m_currentRole));
  }

  void showAddRoleDialog() {
    auto *dlg = new QDialog(this);
    dlg->setWindowTitle("New Role");
    dlg->setFixedWidth(360);
    dlg->setStyleSheet("background:#1c1f2b; color:#e8eaf0;");
    auto *form = new QFormLayout(dlg);
    form->setContentsMargins(20, 20, 20, 16);
    form->setSpacing(12);
    auto *nameEdit = new QLineEdit(dlg);
    nameEdit->setPlaceholderText("e.g. deployer");
    auto *descEdit = new QLineEdit(dlg);
    descEdit->setPlaceholderText("brief description…");
    form->addRow("Role name:", nameEdit);
    form->addRow("Description:", descEdit);
    auto *btns = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dlg);
    btns->button(QDialogButtonBox::Ok)->setProperty("role", "primary");
    connect(btns, &QDialogButtonBox::accepted, dlg, &QDialog::accept);
    connect(btns, &QDialogButtonBox::rejected, dlg, &QDialog::reject);
    form->addRow(btns);

    if (dlg->exec() == QDialog::Accepted) {
      const QString n = nameEdit->text().trimmed().toLower().replace(' ', '_');
      if (n.isEmpty() || m_roles.contains(n)) {
        QMessageBox::warning(this, "Error",
                             n.isEmpty() ? "Name cannot be empty."
                                         : "Role already exists.");
        dlg->deleteLater();
        return;
      }
      m_roles[n] = {n, descEdit->text().trimmed(), {}};
      new QListWidgetItem(n, m_roleList);
      m_roleList->setCurrentRow(m_roleList->count() - 1);
    }
    dlg->deleteLater();
  }

  void deleteCurrentRole() {
    const int row = m_roleList->currentRow();
    if (row < 0)
      return;
    const QString name = m_roleList->item(row)->text();
    if (QMessageBox::question(
            this, "Delete Role",
            QString("Delete role <b>%1</b>? This cannot be undone.").arg(name),
            QMessageBox::Yes | QMessageBox::Cancel) == QMessageBox::Yes) {
      m_roles.remove(name);
      delete m_roleList->takeItem(row);
      m_currentRole.clear();
      m_roleNameLbl->setText("Select a role");
      m_roleDescLbl->clear();
      setPermEditorEnabled(false);
    }
  }

  void setPermEditorEnabled(bool on) {
    m_permScroll->setEnabled(on);
    for (auto *cb : m_permChecks)
      cb->setEnabled(on);
  }

  QListWidget *m_roleList = nullptr;
  QLabel *m_roleNameLbl = nullptr;
  QLabel *m_roleDescLbl = nullptr;
  QScrollArea *m_permScroll = nullptr;
  QWidget *m_permContainer = nullptr;
  QVBoxLayout *m_permLayout = nullptr;
  QMap<QString, QCheckBox *> m_permChecks;
  QMap<QString, RoleData> m_roles;
  QString m_currentRole;
  tsh::ApiClient *m_api = nullptr;
};
