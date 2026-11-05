#include "output_panel.h"

#include <QMetaObject>
#include <QPointer>

namespace aether::editor::qt {

EditorLogConsole::EditorLogConsole(QWidget* parent) : QPlainTextEdit(parent) {
    setReadOnly(true);
    setMaximumBlockCount(1000);
    for (const LogLine& line : Logger::Instance().Recent()) Append(line);
    const QPointer<EditorLogConsole> self(this);
    sink_id_ = Logger::Instance().AddSink([self](const LogLine& line) {
        if (!self) return;
        const QString message = QString("[%1] [%2] %3")
            .arg(QString::fromUtf8(LogLevelName(line.level)))
            .arg(QString::fromStdString(line.category))
            .arg(QString::fromStdString(line.message));
        QMetaObject::invokeMethod(self, [self, message] {
            if (self) self->appendPlainText(message);
        }, Qt::QueuedConnection);
    });
}

EditorLogConsole::~EditorLogConsole() { Logger::Instance().RemoveSink(sink_id_); }

void EditorLogConsole::Append(const LogLine& line) {
    appendPlainText(QString("[%1] [%2] %3")
        .arg(QString::fromUtf8(LogLevelName(line.level)))
        .arg(QString::fromStdString(line.category))
        .arg(QString::fromStdString(line.message)));
}

} // namespace aether::editor::qt
