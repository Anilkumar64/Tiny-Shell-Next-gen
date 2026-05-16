#include "ServerControlPanel.h"
#include <QApplication>

int main(int argc, char *argv[]) {
  QApplication app(argc, argv);

  // Parse command-line arguments
  QString serverUrl = "https://127.0.0.1:8080";
  QString token;

  for (int i = 1; i < argc; ++i) {
    const QString arg = argv[i];
    if ((arg == "-u" || arg == "--url") && i + 1 < argc) {
      serverUrl = argv[++i];
    } else if ((arg == "-t" || arg == "--token") && i + 1 < argc) {
      token = argv[++i];
    }
  }

  auto panel = new ServerControlPanel(serverUrl, token);
  panel->show();

  return app.exec();
}
