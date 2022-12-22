#include "qdiscordmessage.h"

#include <QMetaEnum>
#include <QRegularExpression>

QDiscordMessage QDiscordMessage::fromJson(const QJsonObject &json, int opcode) {
	static const QHash<QString, MessageType> messageTypes = [] {
		const auto me = QMetaEnum::fromType<MessageType>();
		const int cnt = me.keyCount();

		const QRegularExpression regex("([A-Z])");

		QHash<QString, MessageType> r;
		r.reserve(cnt);
		for(int i = 0; i < cnt; i++) {
			QString key = me.key(i);
			key.replace(regex, "_\\1"); // Add _ underscore before every capitalized letter (guildStatus -> guild_Status)
			key = key.toUpper(); // Convert all to uppercase (guild_Status -> GUILD_STATUS)

			r.insert(key, MessageType(me.value(i)));
		}

		return r;
	}();

	return QDiscordMessage{
		.type = messageTypes.value(json["evt"].toString()),
		.json = json,
		.nonce = json["nonce"].toString(),
		.opcode = opcode,
	};
}
