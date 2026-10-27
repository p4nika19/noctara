#define DOCTEST_CONFIG_IMPLEMENT
#include "Doctest.hpp"

#include <QGuiApplication>

#include <cstdlib>

// One GUI application (offscreen platform) for the whole run, so Qt-dependent tests can use
// event loops, fonts and the scene graph's software backend.
int main(int argc, char** argv)
{
    ::setenv("QT_QPA_PLATFORM", "offscreen", 0);
    ::setenv("QT_QUICK_BACKEND", "software", 0);
    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("noctara-tests"));

    doctest::Context context(argc, argv);
    return context.run();
}
