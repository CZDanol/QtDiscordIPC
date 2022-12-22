#pragma once

#include <QObject>

#include "qdiscordmessage.h"

class QDiscordReply : public QObject {
Q_OBJECT
	friend class QDiscord;

public:
	inline const QString &nonce() const {
		return nonce_;
	}

signals:
	/// The object gets deleted right after this signal is emitted (deleteLater)
	void finished(const QDiscordMessage &msg);

protected:
	QDiscordReply(const QString &nonce);

	/// Is managed internally in QDiscord
	~QDiscordReply() = default;

private:
	QString nonce_;

};
