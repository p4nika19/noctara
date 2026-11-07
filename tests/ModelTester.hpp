#pragma once

// QAbstractItemModelTester for doctest. Its own failure reports never reach doctest: the QtTest mode reports to a
// QTest run that does not exist here, and the Warning mode only logs. This wrapper runs it in Warning mode and fails
// the current test case for every warning it logs (category qt.modeltest), when it goes out of scope.

#include "Doctest.hpp"

#include <QAbstractItemModelTester>
#include <QStringList>
#include <QtGlobal>

#include <cstring>
#include <mutex>

namespace noctara::test {

class ModelTester {
public:
    explicit ModelTester(QAbstractItemModel* model)
        : m_first(enter())
        , m_tester(model, QAbstractItemModelTester::FailureReportingMode::Warning)
    {
    }
    ~ModelTester()
    {
        QStringList violations;
        {
            const std::scoped_lock lock(state().mutex);
            violations = state().messages.mid(m_first);
            if (--state().users == 0) {
                qInstallMessageHandler(state().previous);
                state().messages.clear();
            }
        }
        for (const QString& violation : violations) {
            FAIL_CHECK("model tester: " << violation.toStdString());
        }
    }
    ModelTester(const ModelTester&) = delete;
    ModelTester& operator=(const ModelTester&) = delete;
    ModelTester(ModelTester&&) = delete;
    ModelTester& operator=(ModelTester&&) = delete;

private:
    struct State {
        std::mutex mutex;
        int users = 0;
        QtMessageHandler previous = nullptr;
        QStringList messages;
    };
    static State& state()
    {
        static State instance;
        return instance;
    }
    static qsizetype enter()
    {
        const std::scoped_lock lock(state().mutex);
        if (state().users++ == 0) {
            state().previous = qInstallMessageHandler(&ModelTester::handle);
        }
        return state().messages.size();
    }
    static void handle(QtMsgType type, const QMessageLogContext& context, const QString& message)
    {
        QtMessageHandler previous = nullptr;
        {
            const std::scoped_lock lock(state().mutex);
            if (type != QtDebugMsg && type != QtInfoMsg && context.category != nullptr &&
                std::strcmp(context.category, "qt.modeltest") == 0) {
                state().messages.append(message);
            }
            previous = state().previous;
        }
        if (previous != nullptr) {
            previous(type, context, message);
        }
    }

    qsizetype m_first;
    QAbstractItemModelTester m_tester;
};

}  // namespace noctara::test
