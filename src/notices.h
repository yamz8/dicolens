#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
#include <functional>

// How to say something, kept rather than what was said, so it can be said
// again in another language: a closure that translates when it's called.
using Words = std::function<QString()>;

// What a pane tells the user: a problem, which stays until it's dismissed,
// and a notice, which goes after a moment. Both follow a language switch
// made while they're up.
class Notices : public QObject {
    Q_OBJECT

public:
    explicit Notices(QObject *parent = nullptr);

    QString problem() const { return m_problemText; }
    QString notice() const { return m_noticeText; }

    // Nothing to say clears it.
    void setProblem(const Words &words);
    void flash(const Words &words);
    // Words made elsewhere, already in the user's language.
    void flashText(const QString &text);
    void retranslate();

signals:
    void problemChanged();
    void noticeChanged();

private:
    Words m_problem;
    Words m_notice;
    QString m_problemText;
    QString m_noticeText;
    QTimer m_timer;
};
