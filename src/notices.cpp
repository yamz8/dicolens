#include "notices.h"

Notices::Notices(QObject *parent) : QObject(parent) {
    m_timer.setSingleShot(true);
    m_timer.setInterval(2400);
    connect(&m_timer, &QTimer::timeout, this, [this] {
        m_notice = nullptr;
        m_noticeText.clear();
        emit noticeChanged();
    });
}

void Notices::setProblem(const Words &words) {
    const QString text = words ? words() : QString();
    m_problem = text.isEmpty() ? Words() : words;
    if (text == m_problemText)
        return;
    m_problemText = text;
    emit problemChanged();
}

void Notices::flash(const Words &words) {
    m_notice = words;
    m_noticeText = words ? words() : QString();
    emit noticeChanged();
    m_timer.start();
}

void Notices::flashText(const QString &text) {
    flash([text] { return text; });
}

void Notices::retranslate() {
    if (m_problem) {
        m_problemText = m_problem();
        emit problemChanged();
    }
    if (m_notice) {
        m_noticeText = m_notice();
        emit noticeChanged();
    }
}
