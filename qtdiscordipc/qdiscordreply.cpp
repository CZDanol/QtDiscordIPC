#include "qdiscordreply.h"

QDiscordReply::QDiscordReply(const QString &nonce) : nonce_(nonce) {
	connect(this, &QDiscordReply::finished, this, &QDiscordReply::onFinished);
}

void QDiscordReply::onFinished(const QDiscordMessage &msg) {
	if(msg.event == QDiscordMessage::EventType::error)
		emit error(msg);
	else
		emit success(msg);
}
