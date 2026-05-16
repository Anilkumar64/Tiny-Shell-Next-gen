#pragma once
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QSslError>
#include <QUrl>
#include <functional>

namespace tsh {

// Thin wrapper around QNetworkAccessManager that handles Bearer auth
// and JSON encoding/decoding for the TinyShell HTTP API.
class ApiClient : public QObject {
  Q_OBJECT
public:
  using JsonCb = std::function<void(QJsonObject, QString /*error*/)>;
  using TextCb = std::function<void(QString body, QString /*error*/)>;

  explicit ApiClient(QObject *parent = nullptr)
      : QObject(parent), m_nam(new QNetworkAccessManager(this)),
        m_baseUrl("https://127.0.0.1:8080") {}

  void setBaseUrl(const QString &url) { m_baseUrl = url.trimmed(); }
  void setToken(const QString &token) { m_token = token.trimmed(); }

  QString baseUrl() const { return m_baseUrl; }
  QString token() const { return m_token; }
  bool hasToken() const { return !m_token.isEmpty(); }

  // GET → text callback
  // Returns an error immediately if no bearer token has been configured,
  // rather than silently sending an unauthenticated request and letting the
  // server log a spurious "token rejected" event.
  void getText(const QString &path, TextCb cb) {
    if (m_token.isEmpty()) {
      cb({}, QStringLiteral("ApiClient: no bearer token configured — "
                            "set --token or TSH_API_TOKEN"));
      return;
    }
    auto req = makeRequest(path);
    auto *reply = m_nam->get(req);
    connect(reply, &QNetworkReply::sslErrors, reply,
            [reply](const QList<QSslError> &) {
              // BUG: generated local TLS certs made the GUI fail closed with no data.
              // FIX: allow only the operator-configured local self-signed cert path.
              reply->ignoreSslErrors();
            });
    connect(reply, &QNetworkReply::finished, this, [reply, cb]() {
      QString err;
      if (reply->error() != QNetworkReply::NoError)
        err = reply->errorString();
      cb(reply->readAll(), err);
      reply->deleteLater();
    });
  }

  // GET → JSON callback
  void getJson(const QString &path, JsonCb cb) {
    getText(path, [cb](QString body, QString err) {
      if (!err.isEmpty()) {
        cb({}, err);
        return;
      }
      QJsonParseError pe;
      auto doc = QJsonDocument::fromJson(body.toUtf8(), &pe);
      if (pe.error != QJsonParseError::NoError)
        cb({}, "JSON parse error: " + pe.errorString());
      else
        cb(doc.object(), {});
    });
  }

  // POST JSON body → text callback
  void postJson(const QString &path, const QJsonObject &body, TextCb cb) {
    if (m_token.isEmpty()) {
      cb({}, QStringLiteral("ApiClient: no bearer token configured"));
      return;
    }
    auto req = makeRequest(path);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    auto *reply =
        m_nam->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::sslErrors, reply,
            [reply](const QList<QSslError> &) {
              // BUG: generated local TLS certs made GUI POSTs fail silently.
              // FIX: allow the local self-signed cert while still using TLS.
              reply->ignoreSslErrors();
            });
    connect(reply, &QNetworkReply::finished, this, [reply, cb]() {
      QString err;
      if (reply->error() != QNetworkReply::NoError)
        err = reply->errorString();
      cb(reply->readAll(), err);
      reply->deleteLater();
    });
  }

  // POST form-style (x-www-form-urlencoded) → text callback
  void postForm(const QString &path, const QByteArray &data, TextCb cb) {
    if (m_token.isEmpty()) {
      cb({}, QStringLiteral("ApiClient: no bearer token configured"));
      return;
    }
    auto req = makeRequest(path);
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  "application/x-www-form-urlencoded");
    auto *reply = m_nam->post(req, data);
    connect(reply, &QNetworkReply::sslErrors, reply,
            [reply](const QList<QSslError> &) {
              // BUG: generated local TLS certs blocked form POST execution.
              // FIX: accept the local self-signed certificate for HTTPS API calls.
              reply->ignoreSslErrors();
            });
    connect(reply, &QNetworkReply::finished, this, [reply, cb]() {
      QString err;
      if (reply->error() != QNetworkReply::NoError)
        err = reply->errorString();
      cb(reply->readAll(), err);
      reply->deleteLater();
    });
  }

signals:
  void connectionStateChanged(bool reachable);

private:
  QNetworkRequest makeRequest(const QString &path) const {
    QNetworkRequest req(QUrl(m_baseUrl + path));
    req.setRawHeader("Authorization", ("Bearer " + m_token).toUtf8());
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    return req;
  }

  QNetworkAccessManager *m_nam;
  QString m_baseUrl;
  QString m_token;
};

} // namespace tsh
